#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/RecordProof.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace AgcDriver::PageTracker;
using namespace AgcDriver::DriverDetail;
using Runs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
constexpr std::uint64_t Base = 0x200000000ull, Forget = 2;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

RecordProofResult check(const Tracker& tracker, const RecordProof* proof, const Runs& runs, std::uint64_t forget = Forget, const Observation* observation = nullptr) {
    const auto result = RecordProof::Check(tracker, proof, runs, forget, observation);
    RecordProof::Account(result);
    return result;
}

void epochs() {
    Tracker tracker;
    const Range range{Base + RegionBytes - PageBytes, 2 * PageBytes};
    const Runs runs{{range.address + 16, range.address + range.bytes - 16}, {Base + 3 * RegionBytes, Base + 3 * RegionBytes + 4}};
    tracker.RegisterRange(range, Kind::Section, Bit(Reason::ProtectedSection));
    tracker.RegisterRange({runs[1].first, PageBytes}, Kind::Section, Bit(Reason::Immutable));
    require(check(tracker, nullptr, runs) == RecordProofResult::Missing, "covered cold lookup did not fall back");
    auto proof = RecordProof::Capture(tracker, runs, Forget);
    require(proof != nullptr && proof->epochs.size() == 2 && proof->epochs[0].regions.size() == 2, "captured ranges lost region tokens");
    require(check(tracker, proof.get(), runs) == RecordProofResult::Accepted, "unchanged multi-region proof rejected");
    require(check(tracker, proof.get(), runs, Forget + 1) == RecordProofResult::Forget, "open forget interval accepted");
    require(check(tracker, proof.get(), runs, Forget + 2) == RecordProofResult::Forget, "mapping forget accepted");
    auto moved = runs;
    moved[0].first += 4;
    require(check(tracker, proof.get(), moved) == RecordProofResult::Ranges, "relocated range reused old proof");
    moved.pop_back();
    require(check(tracker, proof.get(), moved) == RecordProofResult::Ranges, "missing captured range accepted");
    auto partial = *proof;
    partial.epochs[0].regions.pop_back();
    require(check(tracker, &partial, runs) == RecordProofResult::Coverage, "missing region token accepted");
    tracker.MarkCpuWritten({Base + RegionBytes, 4}, Source::DriverStore);
    require(check(tracker, proof.get(), runs) == RecordProofResult::Cpu, "CPU write in second region accepted");
    proof = RecordProof::Capture(tracker, runs, Forget);
    require(check(tracker, proof.get(), runs) == RecordProofResult::Accepted, "re-record after validation did not recover");
    tracker.MarkCpuWritten({runs[1].first, 4}, Source::DriverStore);
    require(check(tracker, proof.get(), runs) == RecordProofResult::Cpu, "second captured range was not checked");
    proof = RecordProof::Capture(tracker, runs, Forget);
    tracker.MarkGpuWritten({Base + RegionBytes, 4}, Source::GpuDeclaration);
    require(check(tracker, proof.get(), runs) == RecordProofResult::Gpu, "declared GPU write accepted before execution");
    proof = RecordProof::Capture(tracker, runs, Forget);
    require(check(tracker, proof.get(), runs) == RecordProofResult::Coverage, "GPU-pending range acquired new clean proof");
    tracker.RegisterRange(range, Kind::Section, Bit(Reason::ProtectedSection));
    proof = RecordProof::Capture(tracker, runs, Forget);
    tracker.InvalidateRange(range, true);
    tracker.RegisterRange(range, Kind::Section, Bit(Reason::ProtectedSection));
    require(check(tracker, proof.get(), runs) == RecordProofResult::Mapping, "remap resurrected proof");
    proof = RecordProof::Capture(tracker, runs, Forget);
    tracker.MarkCpuWritten(range, Source::DriverStore);
    require(check(tracker, proof.get(), runs) != RecordProofResult::Accepted, "capture/read/capture race recorded clean");
    RecordProof::Recorded(false);
}

void coverage() {
    Tracker tracker;
    const Runs runs{{Base, Base + 2 * PageBytes}};
    require(check(tracker, nullptr, runs) == RecordProofResult::Coverage, "unknown lookup did not fall back");
    tracker.RegisterRange({Base, PageBytes}, Kind::Section, Bit(Reason::ProtectedSection));
    auto proof = RecordProof::Capture(tracker, runs, Forget);
    require(check(tracker, proof.get(), runs) == RecordProofResult::Coverage, "partial coverage accepted");
    tracker.RegisterRange({Base, 2 * PageBytes}, Kind::Private, Bit(Reason::PrivateWatch));
    proof = RecordProof::Capture(tracker, runs, Forget);
    require(check(tracker, proof.get(), runs) == RecordProofResult::Coverage, "private memory without proof observation API accepted");
    tracker.RegisterRange({Base, 2 * PageBytes}, Kind::Section, Bit(Reason::ProtectedSection) | Bit(Reason::OpenWriter));
    proof = RecordProof::Capture(tracker, runs, Forget);
    require(check(tracker, proof.get(), runs) == RecordProofResult::Coverage, "open section writer accepted");
    tracker.SetCoverage({Base, 2 * PageBytes}, Bit(Reason::ProtectedSection));
    proof = RecordProof::Capture(tracker, runs, Forget);
    tracker.ExcludeImport({Base, PageBytes});
    require(check(tracker, proof.get(), runs) != RecordProofResult::Accepted, "excluded import accepted");
    tracker.Poison();
    require(check(tracker, RecordProof::Capture(tracker, runs, Forget).get(), runs) == RecordProofResult::Coverage, "poisoned tracker accepted");
    const Runs invalid{{Base, Base}, {UINT64_MAX - 1, 0}};
    require(check(tracker, RecordProof::Capture(tracker, invalid, Forget).get(), invalid) != RecordProofResult::Accepted, "invalid range accepted");
}

#ifdef _WIN32
void privateMemory() {
    Initialize();
    constexpr std::size_t bytes = RegionBytes + 0x10000;
    auto* memory = GuestArena::GuestArenaAllocate_nid_postfix(bytes, RegionBytes);
    GuestArena::GuestArenaCommit_nid_postfix(memory, bytes, PAGE_READWRITE, bytes);
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    const Range range{address, 4};
    const Runs runs{{address, address + 4}, {address + 32, address + 36}};
    auto* tracker = Get();
    *static_cast<volatile std::uint32_t*>(memory) = 7;
    const auto before = tracker->Counts();
    auto first = RecordProof::Observe(runs);
    require(tracker->Current(range, &first) && tracker->Counts().pollCalls == before.pollCalls + 1 && tracker->Counts().polledPages == before.polledPages + 1, "private boundary did not make one resetting poll for overlapping pages");
    auto proof = RecordProof::Capture(*tracker, runs, Forget, &first);
    require(check(*tracker, proof.get(), runs, Forget, &first) == RecordProofResult::Accepted, "fresh private proof rejected");
    require(check(*tracker, nullptr, runs, Forget, &first) == RecordProofResult::Missing, "fresh private cold lookup did not fall back");
    require(check(*tracker, proof.get(), runs) == RecordProofResult::Coverage, "private proof without boundary accepted");
    Observation empty;
    require(check(*tracker, proof.get(), runs, Forget, &empty) == RecordProofResult::Coverage, "default boundary accepted");
    Tracker other;
    other.RegisterRange({address, bytes}, Kind::Private, Bit(Reason::PrivateWatch));
    require(check(other, proof.get(), runs, Forget, &first) == RecordProofResult::Coverage, "foreign tracker boundary accepted");
    auto current = RecordProof::Observe(runs);
    require(tracker->Counts().polledPages == before.polledPages + 1, "private boundary did not reset the previous write watch");
    require(!tracker->Current(range, &first) && check(*tracker, proof.get(), runs, Forget, &first) == RecordProofResult::Coverage, "old boundary survived the next poll");
    const auto polls = tracker->Counts().pollCalls;
    DWORD previous = 0;
    require(VirtualProtect(memory, bytes, PAGE_NOACCESS, &previous), "private proof noaccess protection failed");
    const auto accepted = check(*tracker, proof.get(), runs, Forget, &current);
    MEMORY_BASIC_INFORMATION state{};
    const auto queried = VirtualQuery(memory, &state, sizeof(state));
    require(VirtualProtect(memory, bytes, previous, &previous), "private proof protection restore failed");
    require(accepted == RecordProofResult::Accepted && queried == sizeof(state) && state.Protect == PAGE_NOACCESS && tracker->Counts().pollCalls == polls, "unchanged private proof read guest bytes, changed protection, or polled in the query");
    RecordProofResult crossThread = RecordProofResult::Accepted;
    std::thread worker([&] { crossThread = check(*tracker, proof.get(), runs, Forget, &current); });
    worker.join();
    require(crossThread == RecordProofResult::Coverage, "boundary crossed its caller thread");
    auto narrow = RecordProof::Observe(RecordProofRuns(runs).first(1));
    require(check(*tracker, proof.get(), runs, Forget, &narrow) == RecordProofResult::Coverage, "partial boundary covered another run");
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto legacy = AgcDriver::GuestMemory::CollectWrites(address, bytes);
    require(legacy != 0, "legacy private baseline failed");
    *static_cast<volatile std::uint32_t*>(memory) = 11;
    current = RecordProof::Observe(runs);
    require(check(*tracker, proof.get(), runs, Forget, &current) == RecordProofResult::Cpu, "private write between boundaries accepted");
    require(!AgcDriver::GuestMemory::UnchangedSince(address, 4, legacy), "private proof poll consumed a write without updating legacy stamps");
    proof = RecordProof::Capture(*tracker, runs, Forget, &current);
    require(check(*tracker, proof.get(), runs, Forget, &current) == RecordProofResult::Accepted, "private re-record did not recover");
    const auto copied = *static_cast<volatile std::uint32_t*>(memory);
    require(copied == 11, "private capture fixture read failed");
    *static_cast<volatile std::uint32_t*>(memory) = 13;
    current = RecordProof::Observe(runs);
    require(check(*tracker, proof.get(), runs, Forget, &current) == RecordProofResult::Cpu, "private capture/read/capture race recorded clean");
    const Runs invalid{{address, address}};
    auto invalidBoundary = RecordProof::Observe(invalid);
    require(!tracker->Current(range, &invalidBoundary) && check(*tracker, proof.get(), runs, Forget, &current) == RecordProofResult::Coverage, "invalid boundary did not expire the previous boundary");
    current = RecordProof::Observe(runs);
    const auto beforeRemap = RecordProof::Capture(*tracker, runs, Forget, &current);
    tracker->RegisterRange({address, bytes}, Kind::Private, Bit(Reason::PrivateWatch));
    require(!tracker->CaptureEpoch(range, &current).covered && check(*tracker, beforeRemap.get(), runs, Forget, &current) != RecordProofResult::Accepted, "boundary crossed a mapping incarnation");
    const Runs crossing{{address + RegionBytes - 4, address + RegionBytes + 4}};
    current = RecordProof::Observe(crossing);
    const auto multi = RecordProof::Capture(*tracker, crossing, Forget, &current);
    require(multi->epochs[0].regions.size() == 2 && check(*tracker, multi.get(), crossing, Forget, &current) == RecordProofResult::Accepted, "multi-region private boundary rejected");
    reinterpret_cast<volatile std::uint32_t*>(memory)[RegionBytes / sizeof(std::uint32_t)] = 19;
    current = RecordProof::Observe(crossing);
    require(check(*tracker, multi.get(), crossing, Forget, &current) == RecordProofResult::Cpu, "private write in the second region accepted");
    current = RecordProof::Observe(runs);
    proof = RecordProof::Capture(*tracker, runs, Forget, &current);
    AgcDriver::GuestMemory::Unwatch(address, bytes);
    auto excluded = RecordProof::Observe(runs);
    require(!tracker->Current(range, &excluded) && check(*tracker, proof.get(), runs, Forget, &excluded) == RecordProofResult::Coverage, "excluded private import acquired a boundary");
    GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
    GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
}

void inaccessible() {
    auto* memory = VirtualAlloc(nullptr, 2 * PageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    require(memory != nullptr, "test allocation failed");
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    const Runs runs{{address, address + 4}, {address + PageBytes, address + PageBytes + 4}};
    Tracker tracker;
    tracker.RegisterRange({address, 2 * PageBytes}, Kind::Section, Bit(Reason::Immutable));
    auto proof = RecordProof::Capture(tracker, runs, Forget);
    std::array<std::byte, 4> captured{};
    std::memcpy(captured.data(), memory, captured.size());
    require(check(tracker, proof.get(), runs) == RecordProofResult::Accepted, "capture/read/capture failed");
    RecordProof::Recorded(true);
    DWORD previous = 0;
    require(VirtualProtect(memory, 2 * PageBytes, PAGE_NOACCESS, &previous), "noaccess protection failed");
    const auto accepted = check(tracker, proof.get(), runs) == RecordProofResult::Accepted;
    MEMORY_BASIC_INFORMATION state{};
    const auto queried = VirtualQuery(memory, &state, sizeof(state));
    require(VirtualProtect(memory, 2 * PageBytes, PAGE_READWRITE, &previous), "restoring test protection failed");
    *static_cast<std::uint32_t*>(memory) = 17;
    tracker.MarkCpuWritten({address, 4}, Source::DriverStore);
    const auto changed = check(tracker, proof.get(), runs);
    VirtualFree(memory, 0, MEM_RELEASE);
    require(accepted && queried == sizeof(state) && state.Protect == PAGE_NOACCESS, "proof lookup touched guest bytes or protections");
    require(changed == RecordProofResult::Cpu, "CPU store accepted");
}

LONG CALLBACK writeFault(EXCEPTION_POINTERS* exception) {
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 && record->ExceptionInformation[0] == 1 && GuestArena::GuestArenaHandleWrite_nid_postfix(record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

void observedCpuWrite() {
    Initialize();
    constexpr auto bytes = 0x4000;
    auto* memory = GuestArena::GuestArenaAllocate_nid_postfix(bytes, RegionBytes);
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
    require(section != nullptr, "section allocation failed");
    GuestArena::GuestArenaMap_nid_postfix(memory, bytes, section, 0, PAGE_READWRITE);
    CloseHandle(section);
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    const Runs runs{{address, address + 4}};
    std::array<void*, 16> pages{};
    std::size_t count = pages.size();
    require(GuestArena::GuestArenaCollectWrites_nid_postfix(address, bytes, pages.data(), &count, true), "section observation boundary failed");
    auto* tracker = Get();
    const auto proof = RecordProof::Capture(*tracker, runs, Forget);
    require(check(*tracker, proof.get(), runs) == RecordProofResult::Accepted, "armed section proof failed");
    const auto handler = AddVectoredExceptionHandler(1, writeFault);
    require(handler != nullptr, "write fault handler failed");
    *static_cast<volatile std::uint32_t*>(memory) = 23;
    RemoveVectoredExceptionHandler(handler);
    require(check(*tracker, proof.get(), runs) != RecordProofResult::Accepted, "observed CPU write accepted");
    count = pages.size();
    require(GuestArena::GuestArenaCollectWrites_nid_postfix(address, bytes, pages.data(), &count, true), "section rearm failed");
    const auto refreshed = RecordProof::Capture(*tracker, runs, Forget);
    require(check(*tracker, refreshed.get(), runs) == RecordProofResult::Accepted, "rearmed section proof failed");
    GpuWrite({address, 4});
    require(check(*tracker, refreshed.get(), runs) == RecordProofResult::Gpu, "GPU write notification accepted");
    GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
    GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
}
#endif
}

int main() {
    try {
        const auto exact = [](const char* name) { const auto* value = std::getenv(name); return value != nullptr && std::string_view(value) == "1"; };
        const bool enabled = exact("APS5_EPOCH_PROOFS") && exact("APS5_PAGE_TRACKER");
        require(RecordProof::Enabled() == enabled, "switch semantics changed");
        require(RecordProof::Counts() == RecordProofCounts{}, "counters changed before use");
        if (enabled) {
            epochs();
            coverage();
#ifdef _WIN32
            inaccessible();
            observedCpuWrite();
            privateMemory();
#endif
            const auto counts = RecordProof::Counts();
            require(counts.results[static_cast<std::size_t>(RecordProofResult::Accepted)] != 0 && counts.results[static_cast<std::size_t>(RecordProofResult::Coverage)] != 0 && counts.unstable == 1, "proof counters missing");
            const auto line = RecordProof::ProfileLine();
            require(line.starts_with("[record-proof] cumulative accepted=") && line.find("rejected-cpu=") != std::string::npos && line.find("rejected-gpu=") != std::string::npos && line.find("fallbacks=") != std::string::npos, "proof profile fields missing");
        } else {
            Tracker tracker;
            const Runs runs{{Base, Base + PageBytes}};
            require(RecordProof::Capture(tracker, runs, Forget) == nullptr, "disabled proof allocated metadata");
            const auto observation = RecordProof::Observe(runs);
            const std::array ranges{Range{Base, PageBytes}};
            const auto direct = PollCpuWrites(ranges);
            require(!tracker.Current(ranges[0], &observation) && !tracker.Current(ranges[0], &direct), "disabled proof issued a boundary");
            if (auto* global = Get()) require(global->Counts().pollCalls == 0 && global->Counts().regions == 0, "disabled proof polled or created region metadata");
            for (std::size_t i = 0; i < static_cast<std::size_t>(RecordProofResult::Count); ++i) RecordProof::Account(static_cast<RecordProofResult>(i));
            RecordProof::Recorded(true);
            RecordProof::Recorded(false);
            RecordProof::ReportIfDue();
            require(RecordProof::Counts() == RecordProofCounts{} && RecordProof::ProfileLine().empty(), "disabled proof counters or output changed");
        }
        std::cout << "record proof " << (enabled ? "on" : "off") << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
