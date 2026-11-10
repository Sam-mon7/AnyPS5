#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_STAGE4DIAGMODEL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_STAGE4DIAGMODEL_HPP

// Pure data structures behind the Stage 4 measurements (Stage4Diag.hpp): no driver dependencies, so
// tests/Stage4Diag.cpp checks them directly. Times are steady-clock nanoseconds passed in by the
// caller (deterministic in tests).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>

namespace AgcDriver::Stage4Model {

// A thread's state over time: one writer (the thread itself) sets the state, any thread reads the
// cumulative time spent in each state up to a given instant (a seqlock, so the snapshot is
// consistent: the state, its start and every total belong to the same transition).
template<std::size_t States>
class StateClock {
public:
    using Totals = std::array<std::int64_t, States>;

    // Writer only. The first call starts the clock; time before it counts nowhere.
    void Set(std::size_t state, std::int64_t now) noexcept {
        const auto sequence = seq.load(std::memory_order_relaxed);
        seq.store(sequence + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        const auto old = current.load(std::memory_order_relaxed);
        const auto start = since.load(std::memory_order_relaxed);
        if (start != 0 && now > start) totals[old].store(totals[old].load(std::memory_order_relaxed) + (now - start), std::memory_order_relaxed);
        current.store(static_cast<std::uint32_t>(state), std::memory_order_relaxed);
        since.store(now, std::memory_order_relaxed);
        seq.store(sequence + 2, std::memory_order_release);
    }

    // Any thread: the time spent in each state up to `now` (the open state counted up to `now`).
    Totals Read(std::int64_t now) const noexcept {
        Totals out{};
        std::uint32_t state = 0;
        std::int64_t start = 0;
        for (;;) {
            const auto before = seq.load(std::memory_order_acquire);
            if ((before & 1u) != 0) continue;
            state = current.load(std::memory_order_relaxed);
            start = since.load(std::memory_order_relaxed);
            for (std::size_t i = 0; i < States; ++i) out[i] = totals[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == before) break;
        }
        if (start != 0 && now > start && state < States) out[state] += now - start;
        return out;
    }

    std::size_t State() const noexcept { return current.load(std::memory_order_relaxed); }
    bool Started() const noexcept { return since.load(std::memory_order_relaxed) != 0; }

private:
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint32_t> current{0};
    std::atomic<std::int64_t> since{0};
    std::array<std::atomic<std::int64_t>, States> totals{};
};

// Time the other thread spent in each state while this thread's interval [before, after] ran: the
// difference of two StateClock snapshots.
template<std::size_t States>
std::array<std::int64_t, States> Overlap(const std::array<std::int64_t, States>& before, const std::array<std::int64_t, States>& after) noexcept {
    std::array<std::int64_t, States> out{};
    for (std::size_t i = 0; i < States; ++i) out[i] = std::max<std::int64_t>(0, after[i] - before[i]);
    return out;
}

// A frame stamp per 4 KiB page of the host address space, allocated a GiB of pages at a time on
// first touch (the guest arena reserves ~1 TiB, of which a few GiB are ever written). Exchange is
// lock-free; a leaf is published with a CAS (a losing allocation is freed).
class PageStampTable {
public:
    static constexpr unsigned PageShift = 12;
    static constexpr unsigned LeafShift = 18;  // 2^18 pages = 1 GiB per leaf
    static constexpr std::size_t LeafPages = std::size_t{1} << LeafShift;
    static constexpr std::size_t TopEntries = std::size_t{1} << (47 - PageShift - LeafShift);

    PageStampTable() : top(new std::atomic<std::uint32_t*>[TopEntries]) {
        for (std::size_t i = 0; i < TopEntries; ++i) top[i].store(nullptr, std::memory_order_relaxed);
    }
    ~PageStampTable() {
        for (std::size_t i = 0; i < TopEntries; ++i) std::free(top[i].load(std::memory_order_relaxed));
    }
    PageStampTable(const PageStampTable&) = delete;
    PageStampTable& operator=(const PageStampTable&) = delete;

    // The page's previous stamp (0: never stamped); pages outside the 47-bit space are ignored (0
    // returned, nothing stored).
    std::uint32_t Exchange(std::uint64_t page, std::uint32_t stamp) noexcept {
        auto* slot = find(page, true);
        return slot != nullptr ? std::atomic_ref<std::uint32_t>(*slot).exchange(stamp, std::memory_order_relaxed) : 0;
    }
    std::uint32_t Load(std::uint64_t page) const noexcept {
        auto* slot = const_cast<PageStampTable*>(this)->find(page, false);
        return slot != nullptr ? std::atomic_ref<std::uint32_t>(*slot).load(std::memory_order_relaxed) : 0;
    }
    std::size_t Leaves() const noexcept {
        std::size_t count = 0;
        for (std::size_t i = 0; i < TopEntries; ++i) count += top[i].load(std::memory_order_relaxed) != nullptr;
        return count;
    }

private:
    std::uint32_t* find(std::uint64_t page, bool create) noexcept {
        const auto index = page >> LeafShift;
        if (index >= TopEntries) return nullptr;
        auto* leaf = top[index].load(std::memory_order_acquire);
        if (leaf == nullptr) {
            if (!create) return nullptr;
            auto* fresh = static_cast<std::uint32_t*>(std::calloc(LeafPages, sizeof(std::uint32_t)));
            if (fresh == nullptr) return nullptr;
            std::uint32_t* expected = nullptr;
            if (top[index].compare_exchange_strong(expected, fresh, std::memory_order_acq_rel)) leaf = fresh;
            else {
                std::free(fresh);
                leaf = expected;
            }
        }
        return leaf + (page & (LeafPages - 1));
    }
    std::unique_ptr<std::atomic<std::uint32_t*>[]> top;
};

// What stamping a page with the current frame found: first write of the page this frame (`fresh`)
// and, of those, pages also stamped the frame before (`repeat`, a hot page a fault tracker would
// keep unprotected).
struct StampResult {
    bool fresh = false;
    bool repeat = false;
};
inline StampResult StampPage(PageStampTable& table, std::uint64_t page, std::uint32_t frame) noexcept {
    if (table.Load(page) == frame) return {};
    const auto old = table.Exchange(page, frame);
    if (old == frame) return {};
    return {true, frame > 1 && old == frame - 1};
}

// A small sorted set of disjoint, non-adjacent [begin, end) ranges with a capacity: past it, the two
// ranges with the smallest gap merge (the set only grows, so an overlap test stays conservative)
// and the set notes that it overflowed.
class RangeSet {
public:
    static constexpr std::size_t Capacity = 32;
    struct Range {
        std::uint64_t begin;
        std::uint64_t end;
    };

    void Clear() noexcept {
        count = 0;
        overflowed = false;
    }
    void Add(std::uint64_t begin, std::uint64_t end) noexcept {
        if (end <= begin) return;
        std::size_t first = 0;
        while (first < count && ranges[first].end < begin) ++first;
        std::size_t last = first;
        while (last < count && ranges[last].begin <= end) {
            begin = std::min(begin, ranges[last].begin);
            end = std::max(end, ranges[last].end);
            ++last;
        }
        if (last > first) {
            ranges[first] = {begin, end};
            if (last > first + 1) std::copy(ranges.begin() + static_cast<std::ptrdiff_t>(last), ranges.begin() + static_cast<std::ptrdiff_t>(count), ranges.begin() + static_cast<std::ptrdiff_t>(first + 1));
            count -= last - first - 1;
            return;
        }
        if (count == Capacity) {
            overflowed = true;
            std::size_t merge = 0;
            std::uint64_t smallest = ~std::uint64_t{0};
            for (std::size_t i = 0; i + 1 < count; ++i) {
                const auto gap = ranges[i + 1].begin - ranges[i].end;
                if (gap < smallest) {
                    smallest = gap;
                    merge = i;
                }
            }
            ranges[merge].end = ranges[merge + 1].end;
            std::copy(ranges.begin() + static_cast<std::ptrdiff_t>(merge + 2), ranges.begin() + static_cast<std::ptrdiff_t>(count), ranges.begin() + static_cast<std::ptrdiff_t>(merge + 1));
            --count;
            Add(begin, end);
            return;
        }
        std::copy_backward(ranges.begin() + static_cast<std::ptrdiff_t>(first), ranges.begin() + static_cast<std::ptrdiff_t>(count), ranges.begin() + static_cast<std::ptrdiff_t>(count + 1));
        ranges[first] = {begin, end};
        ++count;
    }
    void Merge(const RangeSet& other) noexcept {
        for (std::size_t i = 0; i < other.count; ++i) Add(other.ranges[i].begin, other.ranges[i].end);
        overflowed = overflowed || other.overflowed;
    }
    bool Overlaps(const RangeSet& other) const noexcept {
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < count && j < other.count) {
            if (ranges[i].end <= other.ranges[j].begin) ++i;
            else if (other.ranges[j].end <= ranges[i].begin) ++j;
            else return true;
        }
        return false;
    }
    // The ranges of this set that overlap none of `other`'s.
    RangeSet Without(const RangeSet& other) const noexcept {
        RangeSet out;
        for (std::size_t i = 0; i < count; ++i) {
            RangeSet one;
            one.Add(ranges[i].begin, ranges[i].end);
            if (!one.Overlaps(other)) out.Add(ranges[i].begin, ranges[i].end);
        }
        out.overflowed = overflowed;
        return out;
    }
    bool Empty() const noexcept { return count == 0; }
    std::size_t Size() const noexcept { return count; }
    bool Overflowed() const noexcept { return overflowed; }
    const Range& operator[](std::size_t index) const noexcept { return ranges[index]; }

private:
    std::array<Range, Capacity> ranges{};
    std::size_t count = 0;
    bool overflowed = false;
};

// One committed item as the resolve/apply census sees it: what its resolve read (its declared reads
// and the guest ranges it asked the write tracker about during resolve-class phases; `allReads` adds
// the queries of apply-class phases, an upper bound), the template object it used, and what its apply
// changed (its declared writes, ranges it stamped, whether it built the template, moved the pending
// registry's serial or broke every proof memo).
struct ItemFootprint {
    RangeSet reads;
    RangeSet allReads;
    // `reads` less every range that overlaps the item's own writes (its targets: bookkeeping of what it
    // writes, apply-class), and the item's declared device reads alone.
    RangeSet foreignReads;
    RangeSet declaredReads;
    RangeSet writes;
    const void* templateObject = nullptr;
    bool builtTemplate = false;
    bool pendingChanged = false;
    bool breaksProofs = false;
    bool resolves = false;  // a draw or dispatch (an item with a resolve phase)
};

// Whether a later item's resolve, run ahead in a window of K items (in parallel with up to K-1 earlier
// items not applied yet), would read something one of those earlier items changes. Components:
// memory (read ranges vs written ranges), template built by an earlier item, template shared with an
// earlier item (upper bound: the same object, changed or not), the global pending serial (a
// revalidate's fast path reads it) and a proof break (every non-draw item invalidates the proofs).
struct Conflict {
    bool memory = false;
    bool memoryAll = false;
    bool memoryForeign = false;
    bool memoryDeclared = false;
    bool templateBuilt = false;
    bool templateShared = false;
    bool pending = false;
    bool proofs = false;
    bool Fine() const noexcept { return memory || templateBuilt; }
    bool Global() const noexcept { return memory || templateBuilt || pending || proofs; }
};

class ConflictWindow {
public:
    static constexpr std::size_t Depth = 8;
    // Against the window's newest k-1 items (k = 1 checks nothing).
    Conflict Check(const ItemFootprint& item, std::size_t k) const noexcept {
        Conflict result;
        const auto earlier = std::min(k == 0 ? 0 : k - 1, held);
        for (std::size_t back = 1; back <= earlier; ++back) {
            const auto& other = ring[(next + Depth - back) % Depth];
            if (!result.memory && other.writes.Overlaps(item.reads)) result.memory = true;
            if (!result.memoryAll && other.writes.Overlaps(item.allReads)) result.memoryAll = true;
            if (!result.memoryForeign && other.writes.Overlaps(item.foreignReads)) result.memoryForeign = true;
            if (!result.memoryDeclared && other.writes.Overlaps(item.declaredReads)) result.memoryDeclared = true;
            if (item.templateObject != nullptr && other.templateObject == item.templateObject) {
                result.templateShared = true;
                if (other.builtTemplate) result.templateBuilt = true;
            }
            if (other.pendingChanged) result.pending = true;
            if (other.breaksProofs) result.proofs = true;
        }
        return result;
    }
    void Push(const ItemFootprint& item) noexcept {
        ring[next] = item;
        next = (next + 1) % Depth;
        held = std::min(held + 1, Depth - 1);
    }

private:
    std::array<ItemFootprint, Depth> ring{};
    std::size_t next = 0;
    std::size_t held = 0;
};

// Per-item phase timing on one thread: Mark(bucket, now) charges the time since the previous mark,
// less the nested scopes that ended meanwhile, to `bucket`; a nested scope (write tracking, waits)
// charges its own duration to its own bucket. Only the outermost nested scope counts.
template<std::size_t Buckets>
class ItemTimer {
public:
    void Begin(std::int64_t now) noexcept {
        ns.fill(0);
        last = now;
        nested = 0;
        depth = 0;
        active = true;
    }
    void Mark(std::size_t bucket, std::int64_t now) noexcept {
        if (!active) return;
        ns[bucket] += std::max<std::int64_t>(0, now - last - nested);
        last = now;
        nested = 0;
    }
    // True when this scope is the outermost one (the caller then passes its start to NestedEnd).
    bool NestedBegin() noexcept { return active && depth++ == 0; }
    void NestedEnd(std::size_t bucket, std::int64_t start, std::int64_t now, bool outermost) noexcept {
        if (!active) return;
        if (depth > 0) --depth;
        if (!outermost) return;
        const auto spent = std::max<std::int64_t>(0, now - start);
        ns[bucket] += spent;
        nested += spent;
    }
    void End(std::size_t tailBucket, std::int64_t now) noexcept {
        Mark(tailBucket, now);
        active = false;
    }
    bool Active() const noexcept { return active; }
    const std::array<std::int64_t, Buckets>& Totals() const noexcept { return ns; }

private:
    std::array<std::int64_t, Buckets> ns{};
    std::int64_t last = 0;
    std::int64_t nested = 0;
    std::uint32_t depth = 0;
    bool active = false;
};

// Percentile of a small sample (nearest rank, the sample is reordered).
template<typename TValue, std::size_t N>
TValue Percentile(std::array<TValue, N>& values, std::size_t count, double fraction) noexcept {
    if (count == 0) return TValue{};
    count = std::min(count, N);
    auto rank = static_cast<std::size_t>(fraction * static_cast<double>(count - 1) + 0.5);
    rank = std::min(rank, count - 1);
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(rank), values.begin() + static_cast<std::ptrdiff_t>(count));
    return values[rank];
}

}

#endif
