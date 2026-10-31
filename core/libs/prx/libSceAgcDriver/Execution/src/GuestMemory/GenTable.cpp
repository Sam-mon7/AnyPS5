#include "prx/libSceAgcDriver/Execution/include/GuestMemory/GenTable.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"
#include "ThreadOwned.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace AgcDriver::GuestMemory::Gen {

namespace {

void atomicMax(std::atomic<std::uint64_t>& field, std::uint64_t value) noexcept {
    auto current = field.load(std::memory_order_relaxed);
    while (current < value && !field.compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed)) {}
}

constexpr std::uint64_t AddressLimit = std::uint64_t{1} << Table::AddressBits;
constexpr std::uint16_t FullMask = 0xffffu;
static_assert(Table::PagesPerBlock == 16, "the page mask is 16 bits");

// The pages of [begin, end) inside `block`, as a mask of its 16 pages.
std::uint16_t pageMask(std::uint64_t block, std::uint64_t begin, std::uint64_t end) noexcept {
    const auto blockBegin = block << Table::BlockShift;
    const auto from = std::max(begin, blockBegin) - blockBegin;
    const auto to = std::min(end, blockBegin + (std::uint64_t{1} << Table::BlockShift)) - blockBegin;
    if (from >= to) return 0;
    const auto first = static_cast<unsigned>(from >> Table::PageShift);
    const auto last = static_cast<unsigned>((to - 1) >> Table::PageShift);
    const auto bits = last - first + 1;
    return static_cast<std::uint16_t>((bits >= 16 ? 0xffffu : ((1u << bits) - 1u)) << first);
}

UnknownReason reasonOf(BlockState state) noexcept {
    switch (state) {
    case BlockState::Excluded: return UnknownReason::Excluded;
    case BlockState::Compared: return UnknownReason::Compared;
    case BlockState::Uncommitted: return UnknownReason::Uncommitted;
    case BlockState::Untracked: return UnknownReason::Untracked;
    case BlockState::Tracked: break;
    }
    return UnknownReason::None;
}

}

struct Table::Leaf {
    std::array<std::atomic<Generation>, LeafBlocks> data{};
    std::array<std::atomic<Generation>, LeafBlocks> cpu{};
    std::array<std::atomic<Generation>, LeafBlocks> pending{};
    std::array<std::atomic<Generation>, LeafBlocks> meta{};
    // Slice S2: the newest refused (compared-when-shared) walk over the block, as the tracker's
    // `refused` map holds it; stored before the tracker's own entry (both under the tracker mutex).
    std::array<std::atomic<Generation>, LeafBlocks> refused{};
    std::array<std::atomic<Ticket>, LeafBlocks> swept{};
    std::array<std::atomic<Ticket>, LeafBlocks> maskTicket{};
    std::array<std::atomic<std::uint16_t>, LeafBlocks> mask{};
    std::array<std::atomic<std::uint16_t>, LeafBlocks> sweeper{};
    std::array<std::atomic<std::uint8_t>, LeafBlocks> state{};
    // Coarse units of 64 blocks: the max of data|pending|meta (block fields are stored before it,
    // so a reader that sees it unchanged may skip the unit), and the min of swept (0 while any
    // block of the unit is uncovered), recomputed by the sweeper.
    std::array<std::atomic<Generation>, LeafBlocks / CoarseBlocks> coarseChanged{};
    std::array<std::atomic<Ticket>, LeafBlocks / CoarseBlocks> coarseSwept{};
    // Blanket generations of a whole coarse unit: an event covering all 64 blocks of a unit bumps
    // these instead of each block (large pending and representation ranges); every block of the
    // unit reads max(its own field, the unit's).
    std::array<std::atomic<Generation>, LeafBlocks / CoarseBlocks> unitData{};
    std::array<std::atomic<Generation>, LeafBlocks / CoarseBlocks> unitCpu{};
    std::array<std::atomic<Generation>, LeafBlocks / CoarseBlocks> unitPending{};
    std::array<std::atomic<Generation>, LeafBlocks / CoarseBlocks> unitMeta{};
    // Set after the leaf is published: every event that skipped the missing leaf took its
    // generation before (seq_cst on GenClock and the leaf pointer), so a since below `born` reads
    // Changed. 0 while being published (also Changed).
    std::atomic<Generation> born{0};
};

struct Table::Slot {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> begin{0};
    std::atomic<std::uint64_t> end{0};
    std::atomic<Generation> generation{0};
    std::atomic<ProducerId> producer{0};
};

Table::Table()
    : leaves(new (std::nothrow) std::atomic<Leaf*>[LeafCount]()), journal(new (std::nothrow) Slot[JournalEntries]()) {}

Table::~Table() {
    if (leaves == nullptr) return;
    for (std::size_t i = 0; i < LeafCount; ++i) delete leaves[i].load(std::memory_order_relaxed);
}

Generation Table::Now() const noexcept {
    return clock.load(std::memory_order_acquire);
}

Generation Table::Bump() noexcept {
    return clock.fetch_add(1, std::memory_order_seq_cst) + 1;
}

Generation Table::BumpAbove(Generation floor) noexcept {
    auto generation = Bump();
    if (generation > floor) return generation;
    RaiseClock(floor);
    return Bump();
}

void Table::RaiseClock(Generation floor) noexcept {
    atomicMax(clock, floor);
}

Ticket Table::Issue() noexcept {
    return tickets.fetch_add(1, std::memory_order_acq_rel) + 1;
}

Ticket Table::LatestTicket() const noexcept {
    return tickets.load(std::memory_order_acquire);
}

Generation Table::Reset(Generation above) noexcept {
    RaiseClock(above);
    const auto floor = Bump();
    atomicMax(resetGeneration, floor);
    atomicMax(sweptFloor, Issue());
    return floor;
}

Generation Table::ResetFloor() const noexcept {
    return resetGeneration.load(std::memory_order_acquire);
}

Ticket Table::SweptFloor() const noexcept {
    return sweptFloor.load(std::memory_order_acquire);
}

Table::Leaf* Table::leafFor(std::uint64_t block, bool create) const noexcept {
    if (leaves == nullptr) return nullptr;
    const auto index = block / LeafBlocks;
    if (index >= LeafCount) return nullptr;
    auto* leaf = leaves[index].load(std::memory_order_seq_cst);
    if (leaf != nullptr || !create) return leaf;
    auto* fresh = new (std::nothrow) Leaf();
    if (fresh == nullptr) return nullptr;
    if (!leaves[index].compare_exchange_strong(leaf, fresh, std::memory_order_seq_cst)) {
        delete fresh;
        return leaf;
    }
    leafCount.fetch_add(1, std::memory_order_relaxed);
    fresh->born.store(const_cast<Table*>(this)->Bump(), std::memory_order_release);
    return fresh;
}

std::uint64_t Table::Apply(std::uint64_t begin, std::uint64_t end, std::uint8_t kinds, Generation generation, bool create) noexcept {
    end = std::min(end, AddressLimit);
    if (begin >= end || kinds == 0) return 0;
    const auto first = begin >> BlockShift;
    const auto last = (end - 1) >> BlockShift;
    const bool coarse = (kinds & (Data | Cpu | Pending | Meta)) != 0;
    std::uint64_t touched = 0;
    for (auto block = first; block <= last;) {
        auto* leaf = leafFor(block, create);
        const auto leafEnd = std::min(last + 1, (block / LeafBlocks + 1) * LeafBlocks);
        if (leaf == nullptr) {
            // A lost event (allocation failure): lock-free answers are refused from now on.
            if (create) incomplete.store(true, std::memory_order_seq_cst);
            block = leafEnd;
            continue;
        }
        while (block < leafEnd) {
            const auto i = static_cast<std::size_t>(block % LeafBlocks);
            if (i % CoarseBlocks == 0 && block + CoarseBlocks <= leafEnd) {
                const auto unit = i / CoarseBlocks;
                if ((kinds & Data) != 0) atomicMax(leaf->unitData[unit], generation);
                if ((kinds & Cpu) != 0) atomicMax(leaf->unitCpu[unit], generation);
                if ((kinds & Pending) != 0) atomicMax(leaf->unitPending[unit], generation);
                if ((kinds & Meta) != 0) atomicMax(leaf->unitMeta[unit], generation);
                if (coarse) atomicMax(leaf->coarseChanged[unit], generation);
                touched += CoarseBlocks;
                block += CoarseBlocks;
                continue;
            }
            if ((kinds & Data) != 0) atomicMax(leaf->data[i], generation);
            if ((kinds & Cpu) != 0) atomicMax(leaf->cpu[i], generation);
            if ((kinds & Pending) != 0) atomicMax(leaf->pending[i], generation);
            if ((kinds & Meta) != 0) atomicMax(leaf->meta[i], generation);
            ++touched;
            // After the unit's last block of this range (block fields first, then the unit).
            if (coarse && ((i + 1) % CoarseBlocks == 0 || block + 1 == leafEnd)) atomicMax(leaf->coarseChanged[i / CoarseBlocks], generation);
            ++block;
        }
    }
    return touched;
}

std::uint64_t Table::SetState(std::uint64_t begin, std::uint64_t end, BlockState state, bool create) noexcept {
    end = std::min(end, AddressLimit);
    if (begin >= end) return 0;
    const auto first = begin >> BlockShift;
    const auto last = (end - 1) >> BlockShift;
    std::uint64_t touched = 0;
    for (auto block = first; block <= last;) {
        auto* leaf = leafFor(block, create);
        const auto leafEnd = std::min(last + 1, (block / LeafBlocks + 1) * LeafBlocks);
        if (leaf == nullptr) {
            block = leafEnd;
            continue;
        }
        for (; block < leafEnd; ++block) {
            const auto i = static_cast<std::size_t>(block % LeafBlocks);
            if (state != BlockState::Tracked) {
                leaf->coarseSwept[i / CoarseBlocks].store(0, std::memory_order_release);
                leaf->swept[i].store(0, std::memory_order_release);
                leaf->maskTicket[i].store(0, std::memory_order_release);
            }
            leaf->state[i].store(static_cast<std::uint8_t>(state), std::memory_order_release);
            ++touched;
        }
    }
    return touched;
}

void Table::DropCoverage(std::uint64_t begin, std::uint64_t end) noexcept {
    end = std::min(end, AddressLimit);
    if (begin >= end) return;
    const auto first = begin >> BlockShift;
    const auto last = (end - 1) >> BlockShift;
    for (auto block = first; block <= last;) {
        auto* leaf = leafFor(block, false);
        const auto leafEnd = std::min(last + 1, (block / LeafBlocks + 1) * LeafBlocks);
        if (leaf == nullptr) {
            block = leafEnd;
            continue;
        }
        for (; block < leafEnd; ++block) {
            const auto i = static_cast<std::size_t>(block % LeafBlocks);
            leaf->coarseSwept[i / CoarseBlocks].store(0, std::memory_order_release);
            leaf->swept[i].store(0, std::memory_order_release);
            leaf->maskTicket[i].store(0, std::memory_order_release);
        }
    }
}

void Table::RecordSweep(std::uint64_t begin, std::uint64_t end, Ticket ticket, std::uint16_t sweeperId) noexcept {
    end = std::min(end, AddressLimit);
    if (begin >= end || ticket == 0) return;
    const auto first = begin >> BlockShift;
    const auto last = (end - 1) >> BlockShift;
    for (auto block = first; block <= last;) {
        auto* leaf = leafFor(block, true);
        const auto leafEnd = std::min(last + 1, (block / LeafBlocks + 1) * LeafBlocks);
        if (leaf == nullptr) {
            block = leafEnd;
            continue;
        }
        const auto unitFirst = static_cast<std::size_t>(block % LeafBlocks) / CoarseBlocks;
        for (; block < leafEnd; ++block) {
            const auto i = static_cast<std::size_t>(block % LeafBlocks);
            const auto pages = pageMask(block, begin, end);
            leaf->state[i].store(static_cast<std::uint8_t>(BlockState::Tracked), std::memory_order_release);
            leaf->sweeper[i].store(sweeperId, std::memory_order_relaxed);
            if (pages == FullMask) {
                atomicMax(leaf->swept[i], ticket);
                continue;
            }
            // Partial: the mask belongs to maskTicket (mask stored first, ticket after, so a reader
            // that loads the ticket and then the mask never pairs a newer ticket with older pages).
            const auto previous = leaf->maskTicket[i].load(std::memory_order_relaxed);
            std::uint16_t mask = pages;
            if (previous == ticket) mask = static_cast<std::uint16_t>(mask | leaf->mask[i].load(std::memory_order_relaxed));
            else if (previous > ticket) continue;
            leaf->mask[i].store(mask, std::memory_order_release);
            leaf->maskTicket[i].store(ticket, std::memory_order_release);
            if (mask == FullMask) atomicMax(leaf->swept[i], ticket);
        }
        const auto unitLast = static_cast<std::size_t>((leafEnd - 1) % LeafBlocks) / CoarseBlocks;
        for (auto unit = unitFirst; unit <= unitLast; ++unit) {
            Ticket low = NeverCovered;
            for (std::size_t i = unit * CoarseBlocks; i < (unit + 1) * CoarseBlocks; ++i) low = std::min(low, leaf->swept[i].load(std::memory_order_relaxed));
            leaf->coarseSwept[unit].store(low, std::memory_order_release);
        }
    }
}

template<bool Coarse>
Verdict Table::check(std::uint64_t begin, std::uint64_t end, Generation since, Ticket required, std::uint8_t kinds, UnknownReason* reason) const noexcept {
    const auto answer = [&](Verdict verdict, UnknownReason why) {
        if (reason != nullptr) *reason = why;
        return verdict;
    };
    if (since == 0) return answer(Verdict::Unknown, UnknownReason::NoSince);
    if (begin >= end || end > AddressLimit) return answer(Verdict::Unknown, UnknownReason::Outside);
    if (resetGeneration.load(std::memory_order_acquire) > since) return answer(Verdict::Unknown, UnknownReason::Reset);
    const Ticket need = required == 0 ? 0 : std::max(required, sweptFloor.load(std::memory_order_acquire));
    const std::uint8_t changeKinds = kinds & (Data | Cpu | Meta);
    UnknownReason unknown = UnknownReason::None;
    bool pendingMoved = false;
    const auto first = begin >> BlockShift;
    const auto last = (end - 1) >> BlockShift;
    for (auto block = first; block <= last;) {
        const auto* leaf = leafFor(block, false);
        if (leaf == nullptr) return answer(Verdict::Unknown, UnknownReason::Untracked);
        const auto born = leaf->born.load(std::memory_order_acquire);
        if (born == 0 || born > since) return answer(Verdict::Changed, UnknownReason::None);
        const auto leafEnd = std::min(last + 1, (block / LeafBlocks + 1) * LeafBlocks);
        while (block < leafEnd) {
            const auto i = static_cast<std::size_t>(block % LeafBlocks);
            if constexpr (Coarse) {
                if (i % CoarseBlocks == 0 && block + CoarseBlocks <= leafEnd) {
                    const auto unit = i / CoarseBlocks;
                    const auto covered = leaf->coarseSwept[unit].load(std::memory_order_acquire);
                    if (leaf->coarseChanged[unit].load(std::memory_order_acquire) <= since && covered != 0 && covered >= need) {
                        block += CoarseBlocks;
                        continue;
                    }
                }
            }
            const auto state = static_cast<BlockState>(leaf->state[i].load(std::memory_order_acquire));
            if (state != BlockState::Tracked) {
                if (unknown == UnknownReason::None) unknown = reasonOf(state);
                ++block;
                continue;
            }
            if (need != 0) {
                const auto swept = leaf->swept[i].load(std::memory_order_acquire);
                bool covered = swept >= need;
                if (!covered) {
                    const auto ticket = leaf->maskTicket[i].load(std::memory_order_acquire);
                    const auto pages = pageMask(block, begin, end);
                    covered = ticket >= need && (leaf->mask[i].load(std::memory_order_acquire) & pages) == pages;
                }
                if (!covered && unknown == UnknownReason::None) unknown = UnknownReason::NotSwept;
            }
            Generation moved = 0;
            const auto unit = i / CoarseBlocks;
            if ((changeKinds & Data) != 0) moved = std::max({moved, leaf->data[i].load(std::memory_order_acquire), leaf->unitData[unit].load(std::memory_order_acquire)});
            if ((changeKinds & Cpu) != 0) moved = std::max({moved, leaf->cpu[i].load(std::memory_order_acquire), leaf->unitCpu[unit].load(std::memory_order_acquire)});
            if ((changeKinds & Meta) != 0) moved = std::max({moved, leaf->meta[i].load(std::memory_order_acquire), leaf->unitMeta[unit].load(std::memory_order_acquire)});
            if (moved > since) return answer(Verdict::Changed, UnknownReason::None);
            if ((kinds & Pending) != 0 && std::max(leaf->pending[i].load(std::memory_order_acquire), leaf->unitPending[unit].load(std::memory_order_acquire)) > since) pendingMoved = true;
            ++block;
        }
    }
    if (unknown != UnknownReason::None) return answer(Verdict::Unknown, unknown);
    if (pendingMoved) return answer(Verdict::PendingOnly, UnknownReason::None);
    return answer(Verdict::Clean, UnknownReason::None);
}

Verdict Table::Check(std::uint64_t begin, std::uint64_t end, Generation since, Ticket required, std::uint8_t kinds, UnknownReason* reason) const noexcept {
    return check<true>(begin, end, since, required, kinds, reason);
}

Verdict Table::CheckExhaustive(std::uint64_t begin, std::uint64_t end, Generation since, Ticket required, std::uint8_t kinds, UnknownReason* reason) const noexcept {
    return check<false>(begin, end, since, required, kinds, reason);
}

bool Table::Covered(std::uint64_t begin, std::uint64_t end, Ticket required) const noexcept {
    end = std::min(end, AddressLimit);
    if (begin >= end || required == NeverCovered) return false;
    const Ticket need = std::max<Ticket>({required, sweptFloor.load(std::memory_order_acquire), 1});
    const auto last = (end - 1) >> BlockShift;
    for (auto block = begin >> BlockShift; block <= last; ++block) {
        const auto* leaf = leafFor(block, false);
        if (leaf == nullptr) return false;
        const auto i = static_cast<std::size_t>(block % LeafBlocks);
        if (static_cast<BlockState>(leaf->state[i].load(std::memory_order_acquire)) != BlockState::Tracked) return false;
        if (leaf->swept[i].load(std::memory_order_acquire) >= need) continue;
        const auto pages = pageMask(block, begin, end);
        if (leaf->maskTicket[i].load(std::memory_order_acquire) < need || (leaf->mask[i].load(std::memory_order_acquire) & pages) != pages) return false;
    }
    return true;
}

Coverage Table::Model(std::uint64_t begin, std::uint64_t end, Ticket required, std::uint16_t self) const noexcept {
    Coverage result;
    end = std::min(end, AddressLimit);
    if (begin >= end) return result;
    const Ticket need = required == NeverCovered ? NeverCovered : std::max(required, sweptFloor.load(std::memory_order_acquire));
    const auto first = begin >> BlockShift;
    const auto last = (end - 1) >> BlockShift;
    bool inRun = false;
    for (auto block = first; block <= last; ++block) {
        const auto pages = pageMask(block, begin, end);
        ++result.blocks;
        result.pages += static_cast<std::uint64_t>(std::popcount(pages));
        const auto* leaf = leafFor(block, false);
        std::uint16_t uncovered = pages;
        if (leaf != nullptr) {
            const auto i = static_cast<std::size_t>(block % LeafBlocks);
            const auto state = static_cast<BlockState>(leaf->state[i].load(std::memory_order_acquire));
            if (state != BlockState::Tracked && state != BlockState::Untracked) {
                ++result.unknownBlocks;
                inRun = false;
                continue;
            }
            if (state == BlockState::Tracked && need != NeverCovered) {
                if (leaf->swept[i].load(std::memory_order_acquire) >= need) uncovered = 0;
                else if (leaf->maskTicket[i].load(std::memory_order_acquire) >= need) uncovered = static_cast<std::uint16_t>(pages & ~leaf->mask[i].load(std::memory_order_acquire));
                if (uncovered == 0 && leaf->sweeper[i].load(std::memory_order_relaxed) != self) ++result.crossBlocks;
            }
        }
        if (uncovered != 0) {
            ++result.uncoveredBlocks;
            result.uncoveredPages += static_cast<std::uint64_t>(std::popcount(uncovered));
        }
        for (unsigned page = 0; page < PagesPerBlock; ++page) {
            const std::uint16_t bit = static_cast<std::uint16_t>(1u << page);
            if ((pages & bit) == 0) continue;
            if ((uncovered & bit) != 0) {
                if (!inRun) ++result.uncoveredRuns;
                inRun = true;
            } else {
                inRun = false;
            }
        }
    }
    return result;
}

BlockFields Table::Fields(std::uint64_t address) const noexcept {
    BlockFields fields;
    if (address >= AddressLimit) return fields;
    const auto block = address >> BlockShift;
    const auto* leaf = leafFor(block, false);
    if (leaf == nullptr) return fields;
    const auto i = static_cast<std::size_t>(block % LeafBlocks);
    fields.present = true;
    fields.born = leaf->born.load(std::memory_order_acquire);
    const auto unit = i / CoarseBlocks;
    fields.data = std::max(leaf->data[i].load(std::memory_order_acquire), leaf->unitData[unit].load(std::memory_order_acquire));
    fields.cpu = std::max(leaf->cpu[i].load(std::memory_order_acquire), leaf->unitCpu[unit].load(std::memory_order_acquire));
    fields.pending = std::max(leaf->pending[i].load(std::memory_order_acquire), leaf->unitPending[unit].load(std::memory_order_acquire));
    fields.meta = std::max(leaf->meta[i].load(std::memory_order_acquire), leaf->unitMeta[unit].load(std::memory_order_acquire));
    fields.swept = leaf->swept[i].load(std::memory_order_acquire);
    fields.maskTicket = leaf->maskTicket[i].load(std::memory_order_acquire);
    fields.mask = leaf->mask[i].load(std::memory_order_acquire);
    fields.sweeper = leaf->sweeper[i].load(std::memory_order_relaxed);
    fields.state = static_cast<BlockState>(leaf->state[i].load(std::memory_order_acquire));
    fields.coarseChanged = leaf->coarseChanged[i / CoarseBlocks].load(std::memory_order_acquire);
    fields.coarseSwept = leaf->coarseSwept[i / CoarseBlocks].load(std::memory_order_acquire);
    return fields;
}

Generation Table::NewestData(std::uint64_t begin, std::uint64_t end) const noexcept {
    end = std::min(end, AddressLimit);
    Generation newest = 0;
    if (begin >= end) return newest;
    for (auto block = begin >> BlockShift; block <= (end - 1) >> BlockShift; ++block) {
        const auto* leaf = leafFor(block, false);
        if (leaf == nullptr) continue;
        const auto i = static_cast<std::size_t>(block % LeafBlocks);
        newest = std::max({newest, leaf->data[i].load(std::memory_order_acquire), leaf->unitData[i / CoarseBlocks].load(std::memory_order_acquire)});
    }
    return newest;
}

void Table::Refuse(std::uint64_t begin, std::uint64_t end, Generation generation) noexcept {
    end = std::min(end, AddressLimit);
    if (begin >= end || generation == 0) return;
    const auto last = (end - 1) >> BlockShift;
    for (auto block = begin >> BlockShift; block <= last; ++block) {
        auto* leaf = leafFor(block, true);
        if (leaf == nullptr) {
            incomplete.store(true, std::memory_order_seq_cst);
            continue;
        }
        const auto i = static_cast<std::size_t>(block % LeafBlocks);
        atomicMax(leaf->refused[i], generation);
        // The coarse unit covers refusals too, so DataAtMost may skip a unit on it alone.
        atomicMax(leaf->coarseChanged[i / CoarseBlocks], generation);
    }
}

bool Table::DataAtMost(std::uint64_t begin, std::uint64_t end, Generation since) const noexcept {
    if (leaves == nullptr || begin >= end || end > AddressLimit || since == 0) return false;
    if (incomplete.load(std::memory_order_acquire) || resetGeneration.load(std::memory_order_acquire) > since) return false;
    const auto last = (end - 1) >> BlockShift;
    for (auto block = begin >> BlockShift; block <= last;) {
        const auto leafEnd = std::min(last + 1, (block / LeafBlocks + 1) * LeafBlocks);
        const auto* leaf = leaves[block / LeafBlocks].load(std::memory_order_acquire);
        if (leaf == nullptr) {
            block = leafEnd;
            continue;
        }
        while (block < leafEnd) {
            const auto i = static_cast<std::size_t>(block % LeafBlocks);
            const auto unit = i / CoarseBlocks;
            // A whole coarse unit inside the range whose newest event of any kind is old enough.
            if (i % CoarseBlocks == 0 && block + CoarseBlocks <= leafEnd && leaf->coarseChanged[unit].load(std::memory_order_acquire) <= since) {
                block += CoarseBlocks;
                continue;
            }
            if (leaf->data[i].load(std::memory_order_acquire) > since || leaf->unitData[unit].load(std::memory_order_acquire) > since || leaf->refused[i].load(std::memory_order_acquire) > since) return false;
            ++block;
        }
    }
    return true;
}

Generation Table::DataOf(std::uint64_t address) const noexcept {
    if (leaves == nullptr || address >= AddressLimit) return 0;
    const auto block = address >> BlockShift;
    const auto* leaf = leaves[block / LeafBlocks].load(std::memory_order_acquire);
    if (leaf == nullptr) return 0;
    const auto i = static_cast<std::size_t>(block % LeafBlocks);
    return std::max(leaf->data[i].load(std::memory_order_acquire), leaf->unitData[i / CoarseBlocks].load(std::memory_order_acquire));
}

bool Table::Complete() const noexcept {
    return leaves != nullptr && !incomplete.load(std::memory_order_acquire);
}

void Table::Journal(std::uint64_t begin, std::uint64_t end, Generation generation, ProducerId producer) noexcept {
    if (journal == nullptr) return;
    const auto index = journalHead.fetch_add(1, std::memory_order_acq_rel);
    auto& slot = journal[index % JournalEntries];
    slot.sequence.store(2 * index + 1, std::memory_order_release);
    slot.begin.store(begin, std::memory_order_relaxed);
    slot.end.store(end, std::memory_order_relaxed);
    slot.generation.store(generation, std::memory_order_relaxed);
    slot.producer.store(producer, std::memory_order_relaxed);
    slot.sequence.store(2 * index + 2, std::memory_order_release);
}

std::size_t Table::RecentJournal(std::span<JournalEntry> out) const noexcept {
    if (journal == nullptr) return 0;
    const auto head = journalHead.load(std::memory_order_acquire);
    std::size_t written = 0;
    for (std::uint64_t index = head; index > 0 && written < out.size() && head - index < JournalEntries; --index) {
        const auto& slot = journal[(index - 1) % JournalEntries];
        const auto before = slot.sequence.load(std::memory_order_acquire);
        JournalEntry entry{slot.begin.load(std::memory_order_relaxed), slot.end.load(std::memory_order_relaxed), slot.generation.load(std::memory_order_relaxed), slot.producer.load(std::memory_order_relaxed)};
        std::atomic_thread_fence(std::memory_order_acquire);
        if (before != 2 * (index - 1) + 2 || slot.sequence.load(std::memory_order_relaxed) != before) continue;
        out[written++] = entry;
    }
    return written;
}

bool Table::ForeignPendingSince(std::uint64_t begin, std::uint64_t end, Generation since, ProducerId self) const noexcept {
    if (journal == nullptr) return true;
    const auto head = journalHead.load(std::memory_order_acquire);
    // Entries are appended in roughly generation order: stop after a long run of older ones.
    std::uint32_t older = 0;
    for (std::uint64_t index = head; index > 0; --index) {
        if (head - index >= JournalEntries) return true;
        const auto& slot = journal[(index - 1) % JournalEntries];
        const auto before = slot.sequence.load(std::memory_order_acquire);
        const auto entryBegin = slot.begin.load(std::memory_order_relaxed);
        const auto entryEnd = slot.end.load(std::memory_order_relaxed);
        const auto generation = slot.generation.load(std::memory_order_relaxed);
        const auto producer = slot.producer.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (before != 2 * (index - 1) + 2 || slot.sequence.load(std::memory_order_relaxed) != before) return true;
        if (generation <= since) {
            if (++older >= 256) return false;
            continue;
        }
        older = 0;
        if (producer != self && entryBegin < end && begin < entryEnd) return true;
    }
    return false;
}

std::uint64_t Table::JournalAppends() const noexcept {
    return journalHead.load(std::memory_order_relaxed);
}

std::size_t Table::Leaves() const noexcept {
    return leafCount.load(std::memory_order_relaxed);
}

// ----- names -----

const char* VerdictName(Verdict verdict) {
    switch (verdict) {
    case Verdict::Clean: return "clean";
    case Verdict::Changed: return "changed";
    case Verdict::PendingOnly: return "pending";
    case Verdict::Unknown: return "unknown";
    }
    return "?";
}

const char* UnknownReasonName(UnknownReason reason) {
    static constexpr const char* names[] = {"none", "host_region", "excluded", "compared", "uncommitted", "outside", "not_swept", "reset", "no_since", "untracked"};
    static_assert(std::size(names) == static_cast<std::size_t>(UnknownReason::Count));
    const auto index = static_cast<std::size_t>(reason);
    return index < std::size(names) ? names[index] : "?";
}

const char* TicketReasonName(TicketReason reason) {
    static constexpr const char* names[] = {"submission", "packet", "wait_entry", "wait_cpu", "wait_gpu_label", "wait_own", "drain", "reap", "unlocked_wait", "committer", "present", "uncached", "other"};
    static_assert(std::size(names) == static_cast<std::size_t>(TicketReason::Count));
    const auto index = static_cast<std::size_t>(reason);
    return index < std::size(names) ? names[index] : "?";
}

const char* EventName(Event event) {
    static constexpr const char* names[] = {"cpu_sweep", "driver_sweep", "import_window", "fresh_collect", "driver_store", "host_write_begin", "host_write_end", "pending", "forget", "inaccessible", "watch_mapping", "rewatch", "unwatch", "representation", "shadow_publish", "refused", "failed_walk"};
    static_assert(std::size(names) == static_cast<std::size_t>(Event::Count));
    const auto index = static_cast<std::size_t>(event);
    return index < std::size(names) ? names[index] : "?";
}

const char* LockFreeApiName(LockFreeApi api) {
    static constexpr const char* names[] = {"unchanged_since", "unchanged_since_all", "changed_blocks", "written_since", "newest_stamp", "unchanged_committed_since", "stored_over", "tracker_generation"};
    static_assert(std::size(names) == static_cast<std::size_t>(LockFreeApi::Count));
    const auto index = static_cast<std::size_t>(api);
    return index < std::size(names) ? names[index] : "?";
}

const char* ConsumerName(Consumer consumer) {
    static constexpr const char* names[] = {"draw_validate", "dispatch_validate", "dispatch_spans", "resource_proof", "texture_lookup", "storage_refresh", "prelock", "snapshot", "draw_input", "mirror", "presentation", "other"};
    static_assert(std::size(names) == static_cast<std::size_t>(Consumer::Count));
    const auto index = static_cast<std::size_t>(consumer);
    return index < std::size(names) ? names[index] : "?";
}

// ----- counters -----

namespace {

constexpr std::size_t EventCount = static_cast<std::size_t>(Event::Count);
constexpr std::size_t TicketCount = static_cast<std::size_t>(TicketReason::Count);
constexpr std::size_t ConsumerCount = static_cast<std::size_t>(Consumer::Count);
constexpr std::size_t ReasonCount = static_cast<std::size_t>(UnknownReason::Count);
constexpr std::size_t SerialCount = static_cast<std::size_t>(Serial::Count);
constexpr const char* ShadowFieldNames[] = {"agree_clean", "agree_changed", "conservative", "unsafe", "needs_sweep", "equal_restamped", "pending", "refused", "conservative_meta"};
constexpr std::size_t ShadowFixed = std::size(ShadowFieldNames);
constexpr std::size_t ShadowFields = ShadowFixed + ReasonCount - 1;
constexpr const char* VerifyFieldNames[] = {"agree_clean", "agree_changed", "conservative", "unsafe"};
constexpr std::size_t HitBuckets = 6;
constexpr const char* HitBucketNames[HitBuckets] = {"1", "2_3", "4_7", "8_15", "16_31", "32_plus"};
constexpr const char* ModelNames[] = {"calls", "sweeps", "runs", "pages", "blocks", "cross_blocks", "cross_walks", "unknown", "unbumped", "covered"};
constexpr std::size_t ModelCount = std::size(ModelNames);
constexpr const char* SerialNames[SerialCount] = {"proof", "resource_proof", "pending", "publish", "forget", "unwatch", "allocations", "commit", "depth", "tracker"};
constexpr const char* WaitNames[] = {"gpu_label", "inheritable", "inheritable_table", "inheritable_memory"};
constexpr std::size_t LockFreeCount = static_cast<std::size_t>(LockFreeApi::Count);
// Counters a gate may require: their recorder names keep the name as is (no 'gen.' dot prefix;
// queue 0 gets 'gen_q0_'), since the gate accepts only [A-Za-z_]\w* counter names.
constexpr const char* PlainNames[] = {"gen_lockfree_fast", "gen_lockfree_slow", "gen_lockfree_verified", "gen_lockfree_raced", "gen_lockfree_unsafe"};
constexpr std::size_t PlainCount = std::size(PlainNames);
enum PlainCounter : std::size_t { LockFreeFast, LockFreeSlow, LockFreeVerified, LockFreeRaced, LockFreeUnsafe };
constexpr std::size_t WaitCount = std::size(WaitNames);

constexpr std::size_t EventBase = 0;
constexpr std::size_t EventNs = EventBase + 2 * EventCount;
constexpr std::size_t EventSamples = EventNs + 1;
constexpr std::size_t TicketBase = EventSamples + 1;
constexpr std::size_t ShadowBase = TicketBase + TicketCount;
constexpr std::size_t VerifyBase = ShadowBase + ConsumerCount * ShadowFields;
constexpr std::size_t HitBase = VerifyBase + ConsumerCount * 4;
constexpr std::size_t HitLookups = HitBase + 2 * HitBuckets;
constexpr std::size_t HitHits = HitLookups + 1;
constexpr std::size_t HitClean = HitHits + 1;
constexpr std::size_t ModelBase = HitClean + 1;
constexpr std::size_t SerialBase = ModelBase + ModelCount;
constexpr std::size_t SerialFrames = SerialBase + SerialCount;
constexpr std::size_t GlobalOnly = SerialFrames + 1;
constexpr std::size_t WaitBase = GlobalOnly + 1;
constexpr std::size_t UnsafeLogged = WaitBase + WaitCount;
constexpr std::size_t LockFreeBase = UnsafeLogged + 1;
constexpr std::size_t PlainBase = LockFreeBase + 2 * LockFreeCount;
constexpr std::size_t CounterCount = PlainBase + PlainCount;

const std::vector<std::string>& counterNames() {
    static const auto* names = [] {
        auto* list = new std::vector<std::string>(CounterCount);
        auto& n = *list;
        for (std::size_t e = 0; e < EventCount; ++e) {
            n[EventBase + 2 * e] = std::string("gen_events.") + EventName(static_cast<Event>(e)) + ".count";
            n[EventBase + 2 * e + 1] = std::string("gen_events.") + EventName(static_cast<Event>(e)) + ".blocks";
        }
        n[EventNs] = "gen_event_ns";
        n[EventSamples] = "gen_event_samples";
        for (std::size_t t = 0; t < TicketCount; ++t) n[TicketBase + t] = std::string("gen_tickets.") + TicketReasonName(static_cast<TicketReason>(t));
        for (std::size_t c = 0; c < ConsumerCount; ++c) {
            const std::string prefix = std::string("gen_shadow.") + ConsumerName(static_cast<Consumer>(c)) + ".";
            for (std::size_t f = 0; f < ShadowFixed; ++f) n[ShadowBase + c * ShadowFields + f] = prefix + ShadowFieldNames[f];
            for (std::size_t r = 1; r < ReasonCount; ++r) n[ShadowBase + c * ShadowFields + ShadowFixed + r - 1] = prefix + "unknown_" + UnknownReasonName(static_cast<UnknownReason>(r));
            for (std::size_t f = 0; f < 4; ++f) n[VerifyBase + c * 4 + f] = std::string("gen_verify.") + ConsumerName(static_cast<Consumer>(c)) + "." + VerifyFieldNames[f];
        }
        for (std::size_t b = 0; b < HitBuckets; ++b) {
            n[HitBase + 2 * b] = std::string("gen_shadow.hit_runs.") + HitBucketNames[b] + ".runs";
            n[HitBase + 2 * b + 1] = std::string("gen_shadow.hit_runs.") + HitBucketNames[b] + ".draws";
        }
        n[HitLookups] = "gen_shadow.hit_runs.lookups";
        n[HitHits] = "gen_shadow.hit_runs.hits";
        n[HitClean] = "gen_shadow.hit_runs.clean_hits";
        for (std::size_t m = 0; m < ModelCount; ++m) n[ModelBase + m] = std::string("gen_sweep_model.") + ModelNames[m];
        for (std::size_t s = 0; s < SerialCount; ++s) n[SerialBase + s] = std::string("gen_serial_moves.") + SerialNames[s];
        n[SerialFrames] = "gen_serial_moves.frames";
        n[GlobalOnly] = "gen_shadow.resource_proof.global_only_refusals";
        for (std::size_t w = 0; w < WaitCount; ++w) n[WaitBase + w] = std::string("gen_wait_") + WaitNames[w];
        n[UnsafeLogged] = "gen_shadow.unsafe_logged";
        for (std::size_t a = 0; a < LockFreeCount; ++a) {
            n[LockFreeBase + 2 * a] = std::string("gen_lockfree.") + LockFreeApiName(static_cast<LockFreeApi>(a)) + ".fast";
            n[LockFreeBase + 2 * a + 1] = std::string("gen_lockfree.") + LockFreeApiName(static_cast<LockFreeApi>(a)) + ".slow";
        }
        for (std::size_t p = 0; p < PlainCount; ++p) n[PlainBase + p] = PlainNames[p];
        for (const auto& name : n) {
            if (name.empty()) std::abort();
        }
        return list;
    }();
    return *names;
}

using Values = std::array<std::uint64_t, CounterCount>;

// Per thread, single writer (relaxed load + store, no locked RMW on a shared line); folded by
// report and publish. A finished thread's values move to `retired` under its queue tag.
struct ThreadCounters {
    std::array<std::atomic<std::uint64_t>, CounterCount> values{};
    std::atomic<std::uint32_t> tag{0xffffffffu};
    std::uint32_t eventSample = 0;
    std::uint32_t verifySample = 0;
    std::uint64_t hitRun = 0;
    ThreadCounters();
    ~ThreadCounters();
};

struct Registry {
    std::mutex mutex;
    std::vector<ThreadCounters*> threads;
    std::map<std::uint32_t, Values> retired;
    std::map<std::uint32_t, Values> baseline;
    std::array<std::atomic<std::uint64_t>, CounterCount> publishedAll{};
    std::array<std::atomic<std::uint64_t>, CounterCount> publishedQueue0{};
    std::array<bool, CounterCount> registeredAll{};
    std::array<bool, CounterCount> registeredQueue0{};
    std::vector<std::string> queue0Names;
    std::vector<std::string> allNames;
};

Registry& registry() {
    static auto* value = new Registry();
    return *value;
}

ThreadCounters::ThreadCounters() {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    r.threads.push_back(this);
}

struct CountersSlot {};

// Destroyed at thread exit on the exiting thread (ThreadOwned's key destructor): the values move to
// `retired` and the slot is cleared, so a later count on the same thread starts a new block.
ThreadCounters::~ThreadCounters() {
    if (HostThreadSlot<ThreadCounters*, CountersSlot>::Get() == this) HostThreadSlot<ThreadCounters*, CountersSlot>::Set(nullptr);
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    std::erase(r.threads, this);
    auto& into = r.retired[tag.load(std::memory_order_relaxed)];
    for (std::size_t i = 0; i < CounterCount; ++i) into[i] += values[i].load(std::memory_order_relaxed);
}

ThreadCounters& local() noexcept {
    auto* counters = HostThreadSlot<ThreadCounters*, CountersSlot>::Get();
    if (counters == nullptr) {
        ThreadCounters* created = nullptr;
        counters = &ShaderRecompiler::ThreadOwned(created);
        counters->tag.store(GpuLockThreadTag(), std::memory_order_relaxed);
        HostThreadSlot<ThreadCounters*, CountersSlot>::Set(counters);
    }
    return *counters;
}

void add(std::size_t counter, std::uint64_t value = 1) noexcept {
    auto& slot = local().values[counter];
    slot.store(slot.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

std::map<std::uint32_t, Values> foldLocked(Registry& r) {
    std::map<std::uint32_t, Values> totals = r.retired;
    for (auto* thread : r.threads) {
        auto& into = totals[thread->tag.load(std::memory_order_relaxed)];
        for (std::size_t i = 0; i < CounterCount; ++i) into[i] += thread->values[i].load(std::memory_order_relaxed);
    }
    return totals;
}

std::array<std::atomic<bool>, EventCount> suppressed{};

std::mutex& activationMutex() {
    static auto* mutex = new std::mutex;
    return *mutex;
}
std::atomic<bool> live{false};
// Set (seq_cst) before the first lock-free or draw-validation answer: the table then stays
// maintained for the rest of the run even if GEN_TABLE is turned off.
std::atomic<bool> consumerRelied{false};

void noteConsumer() noexcept {
    if (!consumerRelied.load(std::memory_order_relaxed)) consumerRelied.store(true, std::memory_order_seq_cst);
}

struct RequiredSlot {};
struct ConsumerSlot {};
struct SweeperSlot {};

}

Table& Global() {
    static auto* table = new Table();
    return *table;
}

bool Active() noexcept {
    const bool on = ArchB::Switches::Enabled(ArchB::Feature::GenTable) || consumerRelied.load(std::memory_order_acquire);
    if (on == live.load(std::memory_order_acquire)) return on;
    std::lock_guard lock(activationMutex());
    if (on != live.load(std::memory_order_relaxed)) {
        // A margin far above any tracker increment made concurrently on the switch-off path, so no
        // tracker generation handed out before the floor can reach it.
        if (on) static_cast<void>(Global().Reset(detail::CurrentTrackerGeneration() + (std::uint64_t{1} << 24)));
        live.store(on, std::memory_order_release);
    }
    return on;
}

bool ShadowActive() noexcept {
    return ArchB::Switches::Enabled(ArchB::Feature::GenShadow) && Active();
}

bool LockFreeActive() noexcept {
    if (!ArchB::Switches::Enabled(ArchB::Feature::GenLockfree)) return false;
    noteConsumer();
    return Active() && Global().Complete();
}

void CountLockFree(LockFreeApi api, bool fast) noexcept {
    auto& counters = local();
    const auto bump = [&](std::size_t counter) {
        auto& slot = counters.values[counter];
        slot.store(slot.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    };
    bump(LockFreeBase + 2 * static_cast<std::size_t>(api) + (fast ? 0 : 1));
    bump(PlainBase + (fast ? LockFreeFast : LockFreeSlow));
}

void CountLockFreeVerify(LockFreeApi api, LockFreeVerify outcome, std::uint64_t address, std::uint64_t bytes, std::uint64_t since) noexcept {
    add(PlainBase + (outcome == LockFreeVerify::Agree ? LockFreeVerified : outcome == LockFreeVerify::Raced ? LockFreeRaced : LockFreeUnsafe));
    if (outcome != LockFreeVerify::Unsafe) return;
    static std::atomic<std::uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 20) {
        const auto fields = Global().Fields(address);
        std::fprintf(stderr, "[gen-unsafe] lockfree api=%s range=0x%llx+0x%llx since=%llu now=%llu floor=%llu block data=%llu meta=%llu state=%u\n", LockFreeApiName(api), static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(since), static_cast<unsigned long long>(Global().Now()), static_cast<unsigned long long>(Global().ResetFloor()),
            static_cast<unsigned long long>(fields.data), static_cast<unsigned long long>(fields.meta), static_cast<unsigned>(fields.state));
        std::fflush(stderr);
    }
    if (VerifyAbortOnUnsafe()) std::abort();
}

std::uint32_t VerifyPeriod() noexcept {
    static const std::uint32_t period = [] {
        const char* value = std::getenv("APS5_ARCHB_GEN_VERIFY");
        if (value == nullptr || *value == 0) return std::uint32_t{0};
        std::uint64_t parsed = 0;
        for (const char* at = value; *at != 0; ++at) {
            if (*at < '0' || *at > '9' || parsed > 0xffffffffull / 10) {
                std::fprintf(stderr, "[archb] invalid GEN_VERIFY=%s\n", value);
                std::exit(EXIT_FAILURE);
            }
            parsed = parsed * 10 + static_cast<std::uint64_t>(*at - '0');
        }
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(parsed, 0xffffffffull));
    }();
    return period;
}

bool VerifyAbortOnUnsafe() noexcept {
    static const bool abort = [] {
        const char* value = std::getenv("APS5_ARCHB_GEN_VERIFY_ABORT");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return abort;
}

Ticket Issue(TicketReason reason) noexcept {
    const auto ticket = Global().Issue();
    HostThreadSlot<std::uint64_t, RequiredSlot>::Set(ticket);
    auto& counters = local();
    counters.tag.store(GpuLockThreadTag(), std::memory_order_relaxed);
    add(TicketBase + static_cast<std::size_t>(reason));
    return ticket;
}

Ticket IssuePrivate(TicketReason reason) noexcept {
    add(TicketBase + static_cast<std::size_t>(reason));
    return Global().Issue();
}

void NoteTicket(TicketReason reason) noexcept {
    add(TicketBase + static_cast<std::size_t>(reason));
}

Ticket Required() noexcept {
    return HostThreadSlot<std::uint64_t, RequiredSlot>::Get();
}

void Follow(Ticket ticket) noexcept {
    if (ticket == 0 || ticket == HostThreadSlot<std::uint64_t, RequiredSlot>::Get()) return;
    HostThreadSlot<std::uint64_t, RequiredSlot>::Set(ticket);
    add(TicketBase + static_cast<std::size_t>(TicketReason::Committer));
}

void AdoptLatest() noexcept {
    const auto latest = Global().LatestTicket();
    if (latest > HostThreadSlot<std::uint64_t, RequiredSlot>::Get()) HostThreadSlot<std::uint64_t, RequiredSlot>::Set(latest);
    add(TicketBase + static_cast<std::size_t>(TicketReason::Present));
}

std::uint16_t SweeperId() noexcept {
    auto id = HostThreadSlot<std::uint16_t, SweeperSlot>::Get();
    if (id == 0) {
        static std::atomic<std::uint32_t> next{0};
        id = static_cast<std::uint16_t>(next.fetch_add(1, std::memory_order_relaxed) % 0xffffu + 1);
        HostThreadSlot<std::uint16_t, SweeperSlot>::Set(id);
    }
    return id;
}

Generation Note(Event event, std::uint64_t begin, std::uint64_t end, std::uint8_t kinds, Generation generation, bool create) noexcept {
    if (suppressed[static_cast<std::size_t>(event)].load(std::memory_order_relaxed) || begin >= end) return 0;
    auto& counters = local();
    const bool timed = (++counters.eventSample & 63u) == 0;
    const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto& table = Global();
    if (generation == 0) generation = table.Bump();
    const auto blocks = table.Apply(begin, end, kinds, generation, create);
    if (timed) {
        add(EventNs, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()) * 64);
        add(EventSamples);
    }
    add(EventBase + 2 * static_cast<std::size_t>(event));
    add(EventBase + 2 * static_cast<std::size_t>(event) + 1, blocks);
    return generation;
}

Generation NoteState(Event event, std::uint64_t begin, std::uint64_t end, BlockState state, std::uint8_t kinds, Generation generation, bool create) noexcept {
    if (suppressed[static_cast<std::size_t>(event)].load(std::memory_order_relaxed) || begin >= end) return 0;
    auto& table = Global();
    if (generation == 0) generation = table.Bump();
    // The state first (coverage dropped), then the generations a reader descends on.
    static_cast<void>(table.SetState(begin, end, state, create));
    return Note(event, begin, end, static_cast<std::uint8_t>(kinds | Meta), generation, create);
}

void NotePending(std::uint64_t begin, std::uint64_t end, ProducerId producer) noexcept {
    const auto generation = Note(Event::Pending, begin, end, Pending);
    if (generation != 0) Global().Journal(begin, end, generation, producer);
}

void NoteSweep(std::uint64_t begin, std::uint64_t end, Ticket ticket) noexcept {
    Global().RecordSweep(begin, end, ticket, SweeperId());
}

Consumer CurrentConsumer() noexcept {
    const auto value = HostThreadSlot<std::uint8_t, ConsumerSlot>::Get();
    return value == 0 ? Consumer::Other : static_cast<Consumer>(value - 1);
}

// The outermost scope names the decision: a texture lookup made inside a resource proof, or a
// snapshot check made for a draw input, belongs to the outer consumer.
ConsumerScope::ConsumerScope(Consumer consumer) noexcept : previous(HostThreadSlot<std::uint8_t, ConsumerSlot>::Get()) {
    if (previous == 0) HostThreadSlot<std::uint8_t, ConsumerSlot>::Set(static_cast<std::uint8_t>(static_cast<std::uint8_t>(consumer) + 1));
}

ConsumerScope::~ConsumerScope() {
    HostThreadSlot<std::uint8_t, ConsumerSlot>::Set(previous);
}

Outcome Classify(Consumer consumer, Verdict verdict, UnknownReason reason, bool unchanged, bool needsSweep) noexcept {
    const auto base = ShadowBase + static_cast<std::size_t>(consumer) * ShadowFields;
    Outcome outcome;
    if (verdict == Verdict::Clean) outcome = unchanged ? Outcome::AgreeClean : Outcome::Unsafe;
    else outcome = unchanged ? Outcome::Conservative : Outcome::AgreeChanged;
    add(base + static_cast<std::size_t>(outcome));
    if (needsSweep) add(base + 4);
    if (verdict == Verdict::PendingOnly) add(base + 6);
    if (verdict == Verdict::Unknown && reason != UnknownReason::None && reason != UnknownReason::Count) add(base + ShadowFixed + static_cast<std::size_t>(reason) - 1);
    return outcome;
}

void CountRestamp(Consumer consumer) noexcept {
    add(ShadowBase + static_cast<std::size_t>(consumer) * ShadowFields + 5);
}

void CountRefused(Consumer consumer) noexcept {
    add(ShadowBase + static_cast<std::size_t>(consumer) * ShadowFields + 7);
}

void CountConservativeMeta(Consumer consumer) noexcept {
    add(ShadowBase + static_cast<std::size_t>(consumer) * ShadowFields + 8);
}

namespace {
// Call sites of decisions no consumer scope named (Consumer::Other), for the census report.
struct OtherSite {
    std::atomic<const char*> file{nullptr};
    std::atomic<std::uint32_t> line{0};
    std::atomic<std::uint64_t> count{0};
};
std::array<OtherSite, 256>& otherSites() {
    static auto* sites = new std::array<OtherSite, 256>();
    return *sites;
}
}

void NoteOtherSite(const char* file, std::uint32_t line) noexcept {
    auto& sites = otherSites();
    const auto hash = (reinterpret_cast<std::uintptr_t>(file) * 0x9e3779b97f4a7c15ull) ^ (line * 0xc2b2ae3d27d4eb4full);
    for (std::size_t probe = 0; probe < 16; ++probe) {
        auto& site = sites[(hash + probe) % sites.size()];
        const char* expected = nullptr;
        if (site.file.load(std::memory_order_acquire) == nullptr && site.file.compare_exchange_strong(expected, file, std::memory_order_acq_rel)) site.line.store(line, std::memory_order_release);
        if (site.file.load(std::memory_order_acquire) == file && site.line.load(std::memory_order_acquire) == line) {
            site.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void CountVerify(Consumer consumer, Outcome outcome) noexcept {
    add(VerifyBase + static_cast<std::size_t>(consumer) * 4 + static_cast<std::size_t>(outcome));
}

bool VerifySample() noexcept {
    const auto period = VerifyPeriod();
    if (period == 0) return false;
    auto& counters = local();
    if (++counters.verifySample < period) return false;
    counters.verifySample = 0;
    return true;
}

void ReportUnsafe(Consumer consumer, std::uint64_t begin, std::uint64_t end, Generation since, const char* detail) noexcept {
    static std::atomic<std::uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 20) {
        add(UnsafeLogged);
        auto& table = Global();
        // The first block of the range that moved past `since` (or the first block).
        auto address = begin;
        for (auto at = begin & ~((std::uint64_t{1} << Table::BlockShift) - 1); at < end; at += std::uint64_t{1} << Table::BlockShift) {
            const auto fields = table.Fields(at);
            if (fields.data > since || fields.meta > since || fields.state != BlockState::Tracked) {
                address = at;
                break;
            }
        }
        const auto fields = table.Fields(address);
        std::fprintf(stderr, "[gen-unsafe] consumer=%s range=0x%llx+0x%llx since=%llu now=%llu floor=%llu %s block=0x%llx present=%d born=%llu data=%llu cpu=%llu pending=%llu meta=%llu swept=%llu mask=0x%x/%llu state=%u coarse=%llu/%llu\n",
            ConsumerName(consumer), static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin), static_cast<unsigned long long>(since), static_cast<unsigned long long>(table.Now()),
            static_cast<unsigned long long>(table.ResetFloor()), detail != nullptr ? detail : "", static_cast<unsigned long long>(address), fields.present ? 1 : 0, static_cast<unsigned long long>(fields.born),
            static_cast<unsigned long long>(fields.data), static_cast<unsigned long long>(fields.cpu), static_cast<unsigned long long>(fields.pending), static_cast<unsigned long long>(fields.meta),
            static_cast<unsigned long long>(fields.swept), static_cast<unsigned>(fields.mask), static_cast<unsigned long long>(fields.maskTicket), static_cast<unsigned>(fields.state),
            static_cast<unsigned long long>(fields.coarseChanged), static_cast<unsigned long long>(fields.coarseSwept));
        std::array<JournalEntry, 8> recent{};
        const auto count = table.RecentJournal(recent);
        for (std::size_t i = 0; i < count; ++i) {
            std::fprintf(stderr, "[gen-unsafe]   journal 0x%llx+0x%llx gen=%llu producer=%llu\n", static_cast<unsigned long long>(recent[i].begin), static_cast<unsigned long long>(recent[i].end - recent[i].begin),
                static_cast<unsigned long long>(recent[i].generation), static_cast<unsigned long long>(recent[i].producer));
        }
        std::fflush(stderr);
    }
    if (VerifyAbortOnUnsafe()) std::abort();
}

void NoteDrawLookup(bool queue0, bool hit, bool clean) noexcept {
    if (!queue0) return;
    auto& counters = local();
    const auto flush = [&] {
        const auto run = counters.hitRun;
        counters.hitRun = 0;
        if (run == 0) return;
        const std::size_t bucket = run == 1 ? 0 : run < 4 ? 1 : run < 8 ? 2 : run < 16 ? 3 : run < 32 ? 4 : 5;
        add(HitBase + 2 * bucket);
        add(HitBase + 2 * bucket + 1, run);
    };
    add(HitLookups);
    if (hit) add(HitHits);
    if (hit && clean) {
        add(HitClean);
        ++counters.hitRun;
        return;
    }
    flush();
}

void NoteModel(const Coverage& coverage, bool unbumped) noexcept {
    add(ModelBase + 0);
    if (coverage.uncoveredPages != 0) add(ModelBase + 1);
    add(ModelBase + 2, coverage.uncoveredRuns);
    add(ModelBase + 3, coverage.uncoveredPages);
    add(ModelBase + 4, coverage.uncoveredBlocks);
    add(ModelBase + 5, coverage.crossBlocks);
    if (coverage.unknownBlocks != 0) add(ModelBase + 7);
    if (unbumped) add(ModelBase + 8);
    if (coverage.uncoveredPages == 0 && coverage.unknownBlocks == 0) add(ModelBase + 9);
}

void NoteModelCrossWalk() noexcept {
    add(ModelBase + 6);
}

void NoteWaitInheritance(bool fromTable, bool admitted) noexcept {
    add(WaitBase + 0);
    if (!admitted) return;
    add(WaitBase + 1);
    add(WaitBase + (fromTable ? 2 : 3));
}

void NoteGlobalOnlyRefusal() noexcept {
    add(GlobalOnly);
}

void SampleSerials(std::span<const std::uint64_t, static_cast<std::size_t>(Serial::Count)> values) noexcept {
    static std::array<std::uint64_t, SerialCount> previous{};
    static bool primed = false;
    if (primed) {
        for (std::size_t s = 0; s < SerialCount; ++s) {
            if (values[s] > previous[s]) add(SerialBase + s, values[s] - previous[s]);
        }
        add(SerialFrames);
    }
    primed = true;
    std::copy(values.begin(), values.end(), previous.begin());
}

void PublishCounters() noexcept {
    // At most every 200 ms (the recorder reports five-second intervals): the fold reads every
    // registered thread's counters.
    static std::atomic<std::int64_t> last{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto previous = last.load(std::memory_order_relaxed);
    if (now - previous < 200 || !last.compare_exchange_strong(previous, now, std::memory_order_relaxed)) return;
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    if (r.allNames.empty()) {
        const auto& names = counterNames();
        r.allNames.reserve(CounterCount);
        r.queue0Names.reserve(CounterCount);
        for (std::size_t i = 0; i < CounterCount; ++i) {
            const auto& name = names[i];
            const bool plain = i >= PlainBase;
            r.allNames.push_back(plain ? name : "gen." + name.substr(4));
            r.queue0Names.push_back(plain ? "gen_q0_" + name.substr(4) : "gen.q0." + name.substr(4));
        }
    }
    const auto totals = foldLocked(r);
    Values all{};
    Values queue0{};
    for (const auto& [tag, values] : totals) {
        for (std::size_t i = 0; i < CounterCount; ++i) {
            all[i] += values[i];
            if (tag == 0) queue0[i] += values[i];
        }
    }
    for (std::size_t i = 0; i < CounterCount; ++i) {
        // Registered once non-zero (a registered counter is reported every interval), at 0 so the
        // next interval's delta carries the whole value.
        if (all[i] != 0 && !r.registeredAll[i]) {
            r.registeredAll[i] = true;
            ArchB::Recorder::RegisterCounter(r.allNames[i].c_str(), r.publishedAll[i]);
        }
        if (queue0[i] != 0 && !r.registeredQueue0[i]) {
            r.registeredQueue0[i] = true;
            ArchB::Recorder::RegisterCounter(r.queue0Names[i].c_str(), r.publishedQueue0[i]);
        }
        r.publishedAll[i].store(all[i], std::memory_order_relaxed);
        r.publishedQueue0[i].store(queue0[i], std::memory_order_relaxed);
    }
}

void ResetCensus() noexcept {
    for (auto& site : otherSites()) site.count.store(0, std::memory_order_relaxed);
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    r.baseline = foldLocked(r);
}

void ReportCensus(std::uint64_t pass, std::uint64_t frames) noexcept {
    std::map<std::uint32_t, Values> totals;
    std::map<std::uint32_t, Values> baseline;
    {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        totals = foldLocked(r);
        baseline = r.baseline;
        r.baseline = totals;
    }
    const auto& names = counterNames();
    Values all{};
    for (auto& [tag, values] : totals) {
        const auto found = baseline.find(tag);
        std::string line;
        for (std::size_t i = 0; i < CounterCount; ++i) {
            const auto value = values[i] - (found != baseline.end() ? found->second[i] : 0);
            all[i] += value;
            if (value == 0) continue;
            line += " " + names[i] + "=" + std::to_string(value);
        }
        if (!line.empty()) std::fprintf(stderr, "[gen-census] pass=%llu frames=%llu tag=0x%x%s\n", static_cast<unsigned long long>(pass), static_cast<unsigned long long>(frames), tag, line.c_str());
    }
    std::string line;
    for (std::size_t i = 0; i < CounterCount; ++i) {
        if (all[i] != 0) line += " " + names[i] + "=" + std::to_string(all[i]);
    }
    {
        std::vector<std::pair<std::uint64_t, const OtherSite*>> sites;
        for (const auto& site : otherSites()) {
            const auto count = site.count.load(std::memory_order_relaxed);
            if (count != 0) sites.emplace_back(count, &site);
        }
        std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string text;
        for (std::size_t i = 0; i < sites.size() && i < 16; ++i) {
            const char* file = sites[i].second->file.load(std::memory_order_relaxed);
            for (const char* at = file; *at != 0; ++at) if (*at == '/' || *at == '\\') file = at + 1;
            text += " " + std::string(file) + ":" + std::to_string(sites[i].second->line.load(std::memory_order_relaxed)) + "=" + std::to_string(sites[i].first);
            for (auto& site : otherSites()) site.count.store(0, std::memory_order_relaxed);
        }
        if (!text.empty()) std::fprintf(stderr, "[gen-census-other] pass=%llu%s\n", static_cast<unsigned long long>(pass), text.c_str());
    }
    auto& table = Global();
    std::fprintf(stderr, "[gen-census] pass=%llu frames=%llu tag=all leaves=%zu journal=%llu clock=%llu tickets=%llu%s\n", static_cast<unsigned long long>(pass), static_cast<unsigned long long>(frames),
        table.Leaves(), static_cast<unsigned long long>(table.JournalAppends()), static_cast<unsigned long long>(table.Now()), static_cast<unsigned long long>(table.LatestTicket()), line.c_str());
    std::fflush(stderr);
}

std::uint64_t CounterForTest(const char* name) noexcept {
    const auto& names = counterNames();
    std::size_t index = CounterCount;
    for (std::size_t i = 0; i < CounterCount; ++i) {
        if (names[i] == name) index = i;
    }
    if (index == CounterCount) return 0;
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    std::uint64_t total = 0;
    for (const auto& [tag, values] : foldLocked(r)) total += values[index];
    return total;
}

void SuppressEventForTest(Event event, bool value) noexcept {
    suppressed[static_cast<std::size_t>(event)].store(value, std::memory_order_relaxed);
}

void ResetActivationForTest() noexcept {
    std::lock_guard lock(activationMutex());
    live.store(false, std::memory_order_release);
    consumerRelied.store(false, std::memory_order_seq_cst);
}

}
