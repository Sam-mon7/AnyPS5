// Stage 1 write generations, slice S1: unit tests of the generation table (no guest memory), the
// tickets and counters of the driver integration, and the shadow classification. The guest-memory
// shadow tests (tracker answers, injected misses, reset floor) are in WriteTracking.cpp.
#include "prx/libSceAgcDriver/Execution/include/GuestMemory/GenTable.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace AgcDriver::GuestMemory::Gen;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint64_t Block = std::uint64_t{1} << Table::BlockShift;
constexpr std::uint64_t Page = std::uint64_t{1} << Table::PageShift;
// A base well inside one leaf, coarse-unit aligned.
constexpr std::uint64_t Base = 0x0000002000000000ull;

std::uint64_t counter(const char* name) {
    return CounterForTest(name);
}

// 8 threads race atomic max over the same blocks (and their coarse unit) through Apply; a reader
// watches that no field ever decreases (I2), and the leaf is allocated once (CAS).
void monotonic() {
    Table table;
    constexpr int Threads = 8;
    constexpr int Rounds = 20000;
    std::atomic<bool> stop{false};
    std::atomic<bool> decreased{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < Threads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937_64 random(static_cast<std::uint64_t>(t) * 7919u + 1u);
            for (int round = 0; round < Rounds; ++round) {
                const auto generation = random() % 1000000u + 1u;
                const auto block = random() % 4u;
                table.Apply(Base + block * Block, Base + (block + 1) * Block, Data | Cpu | Pending | Meta, generation);
            }
        });
    }
    std::thread reader([&] {
        std::array<Generation, 4> last{};
        Generation lastCoarse = 0;
        while (!stop.load()) {
            for (std::size_t b = 0; b < 4; ++b) {
                const auto fields = table.Fields(Base + b * Block);
                if (fields.data < last[b] || fields.coarseChanged < lastCoarse) decreased = true;
                last[b] = fields.data;
                lastCoarse = std::max(lastCoarse, fields.coarseChanged);
            }
        }
    });
    for (auto& thread : threads) thread.join();
    stop = true;
    reader.join();
    Require(!decreased.load(), "a block or coarse generation decreased under concurrent atomic max");
    Require(table.Leaves() == 1, "concurrent first touches allocated more than one leaf");
    Generation highest = 0;
    for (std::size_t b = 0; b < 4; ++b) {
        const auto fields = table.Fields(Base + b * Block);
        Require(fields.present && fields.data == fields.cpu && fields.data == fields.pending && fields.data == fields.meta, "the kinds of one event diverged");
        highest = std::max(highest, fields.data);
        Require(fields.coarseChanged >= fields.data, "the coarse unit is below one of its blocks");
    }
    Require(highest >= 900000, "the maximum generation was lost");
}

// Every event kind bumps exactly the fields it names; counters count events and blocks.
void events() {
    auto& table = Global();
    const std::uint64_t begin = Base + 0x100000000ull;
    struct Case {
        Event event;
        std::uint8_t kinds;
    };
    const Case cases[] = {
        {Event::CpuSweep, Data | Cpu},
        {Event::DriverSweep, Data},
        {Event::ImportWindow, Data | Cpu},
        {Event::FreshCollect, Data | Cpu},
        {Event::DriverStore, Data},
        {Event::HostWriteBegin, Meta},
        {Event::HostWriteEnd, Data | Cpu},
        {Event::Forget, Meta | Data},
        {Event::Inaccessible, Meta},
        {Event::Representation, Meta},
    };
    std::uint64_t at = begin;
    for (const auto& [event, kinds] : cases) {
        const auto name = std::string("gen_events.") + EventName(event);
        const auto countBefore = counter((name + ".count").c_str());
        const auto blocksBefore = counter((name + ".blocks").c_str());
        const auto before = table.Now();
        const auto generation = Note(event, at, at + 3 * Block, kinds);
        Require(generation > before, std::string("no fresh generation for ") + EventName(event));
        for (std::uint64_t b = 0; b < 3; ++b) {
            const auto fields = table.Fields(at + b * Block);
            Require(fields.present, std::string("no leaf after ") + EventName(event));
            Require(((kinds & Data) != 0) == (fields.data == generation), std::string("data field wrong for ") + EventName(event));
            Require(((kinds & Cpu) != 0) == (fields.cpu == generation), std::string("cpu field wrong for ") + EventName(event));
            Require(((kinds & Meta) != 0) == (fields.meta == generation), std::string("meta field wrong for ") + EventName(event));
            Require(fields.pending == 0, std::string("pending moved for ") + EventName(event));
        }
        Require(counter((name + ".count").c_str()) == countBefore + 1 && counter((name + ".blocks").c_str()) == blocksBefore + 3, std::string("event counters wrong for ") + EventName(event));
        at += 4 * Block;
    }
    // Pending: field plus a journal entry.
    const auto appended = table.JournalAppends();
    NotePending(at, at + Block, 42);
    Require(table.Fields(at).pending != 0 && table.JournalAppends() == appended + 1, "a pending announcement did not bump pending and the journal");
    // State events.
    const auto unwatched = NoteState(Event::Unwatch, at, at + Block, BlockState::Excluded, Meta);
    Require(table.Fields(at).state == BlockState::Excluded && table.Fields(at).meta == unwatched, "unwatch did not exclude and bump meta");
    // A suppressed event is dropped (the test hook the injected-miss tests use).
    SuppressEventForTest(Event::DriverStore, true);
    Require(Note(Event::DriverStore, at + Block, at + 2 * Block, Data) == 0 && table.Fields(at + Block).data == 0, "a suppressed event was applied");
    SuppressEventForTest(Event::DriverStore, false);
    // create=false leaves a missing leaf missing; a leaf created later is born after the event.
    const std::uint64_t far = Base + 0x300000000ull;
    const auto skipped = Note(Event::Forget, far, far + Block, Meta, 0, false);
    Require(!table.Fields(far).present, "an existing-leaves-only event created a leaf");
    table.RecordSweep(far, far + Block, table.Issue(), 1);
    Require(table.Fields(far).born > skipped, "a leaf created after a skipped event is not born after it");
    Require(table.Check(far, far + Block, skipped, 0, Data | Meta) == Verdict::Changed, "a since before the leaf's birth read clean");
}

// The coarse level: a change in one block of a 64-block unit is found through the unit; units that
// did not change are skipped; the unit's swept is the minimum of its blocks.
void coarse() {
    Table table;
    const auto units = 3 * Table::CoarseBlocks;
    const auto t1 = table.Issue();
    table.RecordSweep(Base, Base + units * Block, t1, 1);
    const auto since = table.Now();
    Require(table.Check(Base, Base + units * Block, since, t1, Any) == Verdict::Clean, "a swept, unchanged range is not clean");
    const auto inside = Base + (Table::CoarseBlocks + 17) * Block;
    const auto g = table.Bump();
    table.Apply(inside, inside + Page, Data, g);
    Require(table.Check(Base, Base + units * Block, since, t1, Any) == Verdict::Changed, "a change inside a coarse unit was skipped");
    Require(table.Check(Base, Base + Table::CoarseBlocks * Block, since, t1, Any) == Verdict::Clean, "an unchanged coarse unit is not clean");
    Require(table.Check(Base + 2 * Table::CoarseBlocks * Block, Base + units * Block, since, t1, Any) == Verdict::Clean, "the unit after the change is not clean");
    Require(table.CheckExhaustive(Base, Base + units * Block, since, t1, Any) == Verdict::Changed, "the exhaustive check missed the change");
    Require(table.Fields(Base + Table::CoarseBlocks * Block).coarseChanged == g, "the coarse unit did not take the block's generation");
    Require(table.Fields(Base).coarseSwept == t1, "the coarse swept is not the blocks' minimum");
    // Dropping one block's coverage drops the unit's.
    table.DropCoverage(Base + 5 * Block, Base + 6 * Block);
    Require(table.Fields(Base).coarseSwept == 0, "dropping a block's coverage left the unit covered");
    Require(table.Check(Base, Base + Table::CoarseBlocks * Block, since, t1, Any) == Verdict::Unknown, "an uncovered block inside a skipped unit was not found");
}

// Clocks and tickets (V1-V4 truth table on the model): a sweep covers tickets up to the one read
// before it; a later ticket needs a later sweep; partial sweeps cover their pages; the floor.
void tickets() {
    Table table;
    const auto g1 = table.Bump();
    const auto g2 = table.Bump();
    Require(g2 == g1 + 1 && table.Now() == g2, "GenClock is not a counter");
    Require(table.BumpAbove(g2 + 100) > g2 + 100, "BumpAbove did not move past the floor");
    const auto t1 = table.Issue();
    const auto t2 = table.Issue();
    Require(t2 > t1 && table.LatestTicket() == t2, "tickets are not increasing");
    // A sweep that read t2 before its OS query covers t1 and t2 but not a later ticket.
    table.RecordSweep(Base, Base + 2 * Block, t2, 1);
    Require(table.Covered(Base, Base + 2 * Block, t1) && table.Covered(Base, Base + 2 * Block, t2), "a sweep does not cover the tickets before it");
    const auto t3 = table.Issue();
    Require(!table.Covered(Base, Base + 2 * Block, t3), "a stale sweep covers a later ticket");
    UnknownReason reason{};
    Require(table.Check(Base, Base + Block, table.Now(), t3, Any, &reason) == Verdict::Unknown && reason == UnknownReason::NotSwept, "a check at a later ticket is not NotSwept");
    // A partial sweep (pages 2-5 of a block) covers those pages only.
    table.RecordSweep(Base + 2 * Page, Base + 6 * Page, t3, 2);
    Require(table.Covered(Base + 3 * Page, Base + 5 * Page, t3), "a partial sweep does not cover its pages");
    Require(!table.Covered(Base, Base + 3 * Page, t3), "a partial sweep covers pages it did not reach");
    // The other pages swept at the same ticket complete the block.
    table.RecordSweep(Base, Base + 2 * Page, t3, 2);
    table.RecordSweep(Base + 6 * Page, Base + Block, t3, 2);
    Require(table.Fields(Base).swept == t3 && table.Covered(Base, Base + Block, t3), "a block swept piece by piece at one ticket is not covered");
    // The sweep model: runs of uncovered pages and cross-sweeper coverage.
    const auto model = table.Model(Base, Base + 2 * Block, t3, 2);
    Require(model.blocks == 2 && model.pages == 32 && model.uncoveredBlocks == 1 && model.uncoveredPages == 16 && model.uncoveredRuns == 1 && model.crossBlocks == 0, "the sweep model miscounted");
    Require(table.Model(Base, Base + Block, t3, 7).crossBlocks == 1, "coverage by another sweeper is not counted as cross-thread reuse");
    Require(table.Model(Base, Base + Block, Table::NeverCovered, 2).uncoveredPages == 16, "an unbumped thread is covered");
    // The reset floor: earlier sinces read Unknown(Reset), earlier sweeps stop covering.
    const auto since = table.Now();
    const auto floor = table.Reset(since + 10);
    Require(floor > since + 10 && table.Now() >= floor, "the reset floor is not above the clock");
    Require(table.Check(Base, Base + Block, since, 0, Any, &reason) == Verdict::Unknown && reason == UnknownReason::Reset, "a since before the floor did not read Reset");
    Require(!table.Covered(Base, Base + Block, t1), "a sweep before the floor still covers");
    Require(table.Check(Base, Base + Block, floor, 0, Any) == Verdict::Clean, "a since at the floor is not clean");

    // Driver tickets: Issue sets the thread's ticket, IssuePrivate does not, Follow adopts.
    const auto issued = Issue(TicketReason::Submission);
    Require(Required() == issued, "Issue did not set the thread's ticket");
    const auto submissions = counter("gen_tickets.submission");
    const auto privateTicket = IssuePrivate(TicketReason::Uncached);
    Require(privateTicket > issued && Required() == issued, "a private ticket changed the thread's ticket");
    Require(counter("gen_tickets.submission") == submissions && counter("gen_tickets.uncached") >= 1, "ticket reasons were not counted");
    std::thread committer([&] {
        Require(Required() == 0, "a new thread has a ticket");
        Follow(issued);
        Require(Required() == issued, "a committer did not adopt the carried ticket");
        AdoptLatest();
        Require(Required() >= privateTicket, "the presenter did not adopt the newest ticket");
    });
    committer.join();
}

// The journal: newest first, foreign vs own producer, overflow is conservative.
void journal() {
    Table table;
    for (std::uint64_t i = 1; i <= 10; ++i) table.Journal(Base + i * Block, Base + (i + 1) * Block, 100 + i, i % 2);
    std::array<JournalEntry, 8> recent{};
    Require(table.RecentJournal(recent) == 8 && recent[0].generation == 110 && recent[7].generation == 103, "the journal is not newest first");
    Require(table.ForeignPendingSince(Base + 10 * Block, Base + 11 * Block, 105, 1), "a foreign pending write since was missed");
    Require(!table.ForeignPendingSince(Base + 9 * Block, Base + 10 * Block, 105, 1), "an own pending write counted as foreign");
    Require(!table.ForeignPendingSince(Base + 2 * Block, Base + 3 * Block, 105, 0), "a pending write before since counted");
    for (std::size_t i = 0; i < Table::JournalEntries + 5; ++i) table.Journal(Base, Base + Page, 1000 + i, 3);
    Require(table.ForeignPendingSince(Base + 50 * Block, Base + 51 * Block, 999, 3), "an overflowed journal is not conservative");
}

// Check classification: Clean, Changed, PendingOnly and every Unknown reason.
void check() {
    Table table;
    UnknownReason reason{};
    Require(table.Check(Base, Base + Block, table.Now(), 0, Any, &reason) == Verdict::Unknown && reason == UnknownReason::Untracked, "a never-swept block is not Untracked");
    table.RecordSweep(Base, Base + 8 * Block, table.Issue(), 1);
    const auto since = table.Now();
    Require(table.Check(Base, Base + 8 * Block, since, 0, Any) == Verdict::Clean, "clean");
    Require(table.Check(Base, Base + Block, 0, 0, Any, &reason) == Verdict::Unknown && reason == UnknownReason::NoSince, "since 0 is not NoSince");
    Require(table.Check(Base, (std::uint64_t{1} << Table::AddressBits) + Block, since, 0, Any, &reason) == Verdict::Unknown && reason == UnknownReason::Outside, "outside the address space is not Outside");
    table.Apply(Base + Block, Base + 2 * Block, Pending, table.Bump());
    Require(table.Check(Base, Base + 2 * Block, since, 0, Any) == Verdict::PendingOnly, "pending only");
    Require(table.Check(Base, Base + 2 * Block, since, 0, Data | Meta) == Verdict::Clean, "pending counted without the kind");
    table.Apply(Base + Block, Base + 2 * Block, Cpu | Data, table.Bump());
    Require(table.Check(Base, Base + 2 * Block, since, 0, Any) == Verdict::Changed, "changed dominates pending");
    Require(table.Check(Base, Base + 2 * Block, since, 0, Pending) == Verdict::PendingOnly, "kinds not selected");
    const struct {
        BlockState state;
        UnknownReason expected;
    } states[] = {{BlockState::Excluded, UnknownReason::Excluded}, {BlockState::Compared, UnknownReason::Compared}, {BlockState::Uncommitted, UnknownReason::Uncommitted}};
    std::uint64_t at = Base + 2 * Block;
    for (const auto& [state, expected] : states) {
        table.SetState(at, at + Block, state, false);
        Require(table.Check(at, at + Block, since, 0, Any, &reason) == Verdict::Unknown && reason == expected, "an untracked state does not read its reason");
        Require(table.Fields(at).swept == 0, "a state change kept coverage");
        at += Block;
    }
    // A sweep tracks the block again.
    table.RecordSweep(Base + 2 * Block, Base + 3 * Block, table.Issue(), 1);
    Require(table.Check(Base + 2 * Block, Base + 3 * Block, since, 0, Any) == Verdict::Clean, "a sweep did not track an excluded block again");
}

// I1 under concurrency: writers store a version ("the bytes"), then take a generation and publish
// it (O2); checkers take since before reading the versions (V6), wait, check. A Clean verdict with a
// version published before the check started that is newer than the one the checker read fails.
void stress() {
    Table table;
    constexpr std::size_t Blocks = 256;
    table.RecordSweep(Base, Base + Blocks * Block, table.Issue(), 1);
    std::vector<std::atomic<std::uint64_t>> versions(Blocks);
    std::vector<std::atomic<std::uint64_t>> published(Blocks);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> violations{0};
    std::atomic<std::uint64_t> cleans{0};
    std::atomic<std::uint64_t> changes{0};
    std::vector<std::thread> threads;
    for (int w = 0; w < 4; ++w) {
        threads.emplace_back([&, w] {
            std::mt19937_64 random(static_cast<std::uint64_t>(w) * 31u + 5u);
            while (!stop.load(std::memory_order_relaxed)) {
                const auto block = random() % Blocks;
                const auto version = versions[block].fetch_add(1, std::memory_order_acq_rel) + 1;
                const auto generation = table.Bump();
                table.Apply(Base + block * Block, Base + block * Block + Page, Data | Cpu, generation);
                auto current = published[block].load(std::memory_order_relaxed);
                while (current < version && !published[block].compare_exchange_weak(current, version, std::memory_order_release, std::memory_order_relaxed)) {}
                if ((random() & 63u) == 0) std::this_thread::yield();
            }
        });
    }
    for (int c = 0; c < 4; ++c) {
        threads.emplace_back([&, c] {
            std::mt19937_64 random(static_cast<std::uint64_t>(c) * 131u + 7u);
            std::vector<std::uint64_t> seen;
            while (!stop.load(std::memory_order_relaxed)) {
                const auto first = random() % (Blocks - 8);
                const auto count = random() % 8 + 1;
                const auto since = table.Now();
                seen.assign(count, 0);
                for (std::size_t i = 0; i < count; ++i) seen[i] = versions[first + i].load(std::memory_order_acquire);
                if ((random() & 3u) == 0) std::this_thread::yield();
                std::vector<std::uint64_t> before(count);
                for (std::size_t i = 0; i < count; ++i) before[i] = published[first + i].load(std::memory_order_acquire);
                const auto verdict = table.Check(Base + first * Block, Base + (first + count) * Block, since, 0, Data | Meta);
                if (verdict == Verdict::Clean) {
                    ++cleans;
                    for (std::size_t i = 0; i < count; ++i) {
                        if (before[i] > seen[i]) ++violations;
                    }
                } else {
                    ++changes;
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    stop = true;
    for (auto& thread : threads) thread.join();
    std::cout << "stress: clean " << cleans.load() << " changed " << changes.load() << " violations " << violations.load() << "\n";
    Require(violations.load() == 0, "a clean verdict hid a published write (I1)");
    Require(cleans.load() != 0 && changes.load() != 0, "the stress test did not exercise both verdicts");
}

// Shadow classification: the outcome table, and an injected decision that the table calls Clean
// while the byte compare found a change is counted unsafe.
void shadowClassify() {
    const auto unsafeBefore = counter("gen_shadow.draw_validate.unsafe");
    Require(Classify(Consumer::Snapshot, Verdict::Clean, UnknownReason::None, true, false) == Outcome::AgreeClean, "clean + unchanged");
    Require(Classify(Consumer::Snapshot, Verdict::Changed, UnknownReason::None, false, false) == Outcome::AgreeChanged, "changed + changed");
    Require(Classify(Consumer::Snapshot, Verdict::Unknown, UnknownReason::Excluded, true, true) == Outcome::Conservative, "unknown + unchanged");
    Require(Classify(Consumer::Snapshot, Verdict::Clean, UnknownReason::None, false, false) == Outcome::Unsafe, "clean + changed");
    Require(counter("gen_shadow.snapshot.agree_clean") >= 1 && counter("gen_shadow.snapshot.agree_changed") >= 1 && counter("gen_shadow.snapshot.conservative") >= 1 && counter("gen_shadow.snapshot.unsafe") >= 1 && counter("gen_shadow.snapshot.unknown_excluded") >= 1 && counter("gen_shadow.snapshot.needs_sweep") >= 1, "the shadow counters did not move");

    // A draw variant captured at `since` over a tracked range; the guest bytes then change through
    // a path the table misses (no event): the validation's byte compare finds the change (here
    // EqualData: bytes differ in data words), the table says Clean, the census counts unsafe.
    auto& table = Global();
    const std::uint64_t at = Base + 0x500000000ull;
    // A sweep after the thread's ticket covers the runs (an uncovered clean reads unknown_not_swept).
    static_cast<void>(Issue(TicketReason::Submission));
    table.RecordSweep(at, at + 2 * Block, table.Issue(), SweeperId());
    AgcDriver::DriverDetail::DispatchVariant variant;
    variant.runs.emplace_back(at + 0x100, at + 0x180);
    variant.genSince.store(table.Now());
    const auto verdict = AgcDriver::DriverDetail::ShadowValidatedVariant(Consumer::DrawValidate, variant, AgcDriver::DriverDetail::EntryOutcome::EqualData, table.Now());
    Require(verdict == Verdict::Clean, "the fixture is not clean in the table");
    Require(counter("gen_shadow.draw_validate.unsafe") == unsafeBefore + 1, "an injected unsafe draw validation was not counted");
    // The same variant after a real event: Changed, and an equal compare restamps its since.
    Note(Event::CpuSweep, at, at + Page, Data | Cpu);
    const auto restamps = counter("gen_shadow.draw_validate.equal_restamped");
    const auto before = table.Now();
    Require(AgcDriver::DriverDetail::ShadowValidatedVariant(Consumer::DrawValidate, variant, AgcDriver::DriverDetail::EntryOutcome::Equal, before) == Verdict::Changed, "a written variant is not changed");
    Require(counter("gen_shadow.draw_validate.equal_restamped") == restamps + 1 && variant.genSince.load() == before, "an equal compare after a change did not restamp");
    Require(AgcDriver::DriverDetail::ShadowValidatedVariant(Consumer::DrawValidate, variant, AgcDriver::DriverDetail::EntryOutcome::Equal, table.Now()) == Verdict::Clean, "the restamped variant is not clean");
    // A refusal by a gate is not classified.
    const auto refused = counter("gen_shadow.draw_validate.refused");
    AgcDriver::DriverDetail::ShadowValidatedVariant(Consumer::DrawValidate, variant, AgcDriver::DriverDetail::EntryOutcome::PublishMoved, table.Now());
    Require(counter("gen_shadow.draw_validate.refused") == refused + 1, "a refused validation was classified");
    // Hit runs: 9 clean queue-0 hits then a miss land in the 8-15 bucket.
    for (int i = 0; i < 9; ++i) NoteDrawLookup(true, true, true);
    NoteDrawLookup(true, false, false);
    Require(counter("gen_shadow.hit_runs.8_15.runs") == 1 && counter("gen_shadow.hit_runs.8_15.draws") == 9, "the hit-run histogram miscounted");
}

}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (mode == "--monotonic") monotonic();
        else if (mode == "--events") events();
        else if (mode == "--coarse") coarse();
        else if (mode == "--tickets") tickets();
        else if (mode == "--journal") journal();
        else if (mode == "--check") check();
        else if (mode == "--stress") stress();
        else if (mode == "--shadow-classify") shadowClassify();
        else {
            std::cerr << "unknown mode\n";
            return 2;
        }
        std::cout << "gen table test " << mode << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << "\n";
        return 1;
    }
}
