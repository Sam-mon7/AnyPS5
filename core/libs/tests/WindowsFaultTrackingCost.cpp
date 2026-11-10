// Stage 4 M3 premise check (AnyPS5-redesign/specs/stage4-driver-critical-path.md, option 2 WT_FAULT):
// what write tracking by page protection costs on this machine. A region is protected read-only; the
// first store to each page faults, a vectored exception handler stamps the page and makes it writable
// again, and the store retries. Measured, as medians over rounds:
//   store        a store to a writable, resident page (baseline)
//   fault        the faulting first store (handler + VirtualProtect of one unit + retry), sequential
//                and random page order, one unit = 4 KiB or 64 KiB (16 pages opened per fault)
//   second       a store to a page the handler opened (TLB refill after the protection change)
//   reprotect    putting the written pages back to read-only at a "flip": one call over a contiguous
//                run, or one call per scattered page (every 4th page)
//   chain        the fault with 3 more handlers ahead of ours in the vectored chain
//   parallel     W threads faulting disjoint pages at once (per-fault latency on each thread)
// each with B busy threads running on other processors (a protection change must shoot the TLB down
// on every processor running the process). Lines start with "[fault-cost]"; the last ones give the
// pages per frame that fit in 2 ms of guest critical path. Exit 0 unless the handler failed.
// usage: windows_fault_tracking_cost_tests [--quick] [--pages N] [--rounds R]
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t PageBytes = 4096;

std::atomic<std::uintptr_t> regionBase{0};
std::atomic<std::size_t> regionBytes{0};
std::atomic<std::size_t> unitBytes{PageBytes};
std::atomic<std::uint64_t> faults{0};
std::atomic<std::uint64_t> handlerFailures{0};
std::vector<std::uint32_t>* stamps = nullptr;
std::atomic<std::uint32_t> frame{1};

LONG CALLBACK TrackingHandler(PEXCEPTION_POINTERS info) {
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
    const auto base = regionBase.load(std::memory_order_acquire);
    const auto bytes = regionBytes.load(std::memory_order_relaxed);
    if (address < base || address >= base + bytes) return EXCEPTION_CONTINUE_SEARCH;
    const auto unit = unitBytes.load(std::memory_order_relaxed);
    const auto start = base + ((address - base) & ~(static_cast<std::uintptr_t>(unit) - 1));
    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(start), unit, PAGE_READWRITE, &previous)) {
        handlerFailures.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // The tracker's work: stamp every page the unit opened with the current frame.
    const auto first = (start - base) / PageBytes;
    const auto stamp = frame.load(std::memory_order_relaxed);
    for (std::size_t page = first; page < first + unit / PageBytes; ++page) (*stamps)[page] = stamp;
    faults.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_EXECUTION;
}

LONG CALLBACK PassingHandler(PEXCEPTION_POINTERS info) {
    // A handler ahead of ours that looks at the exception and passes (the emulator's own handlers).
    return info->ExceptionRecord->ExceptionCode == 0xE0000001u ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

[[noreturn]] void Fail(const char* what) {
    std::fprintf(stderr, "[fault-cost] FAILED: %s (error %lu)\n", what, GetLastError());
    std::exit(1);
}

double NowNs() {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values.empty() ? 0.0 : values[values.size() / 2];
}

void Protect(unsigned char* memory, std::size_t bytes, DWORD protection) {
    DWORD previous = 0;
    if (!VirtualProtect(memory, bytes, protection, &previous)) Fail("VirtualProtect");
}

// Threads spinning over their own memory on other processors (the process stays live there).
class BusyThreads {
public:
    explicit BusyThreads(unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([this] {
                std::vector<unsigned char> memory(1 << 20);
                std::size_t at = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    memory[at] = static_cast<unsigned char>(memory[at] + 1);
                    at = (at + 4096 + 64) & ((1 << 20) - 1);
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ~BusyThreads() {
        stop = true;
        for (auto& thread : threads) thread.join();
    }

private:
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
};

struct Results {
    double storeNs = 0;
    double faultSeqNs = 0;
    double faultRandNs = 0;
    double secondNs = 0;
    double reprotectRunNsPerPage = 0;
    double reprotectScatterNsPerCall = 0;
    double fault64kNsPerFault = 0;
    double fault64kNsPerPage = 0;
    double chainNs = 0;
};

class Bench {
public:
    Bench(std::size_t pages, int rounds) : pages(pages), rounds(rounds) {
        bytes = pages * PageBytes;
        memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (memory == nullptr) Fail("VirtualAlloc");
        std::memset(memory, 1, bytes);
        pageStamps.assign(pages, 0);
        stamps = &pageStamps;
        regionBytes = bytes;
        regionBase = reinterpret_cast<std::uintptr_t>(memory);
        order.resize(pages);
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::mt19937_64 random(12345);
        std::shuffle(order.begin(), order.end(), random);
    }
    ~Bench() {
        regionBase = 0;
        VirtualFree(memory, 0, MEM_RELEASE);
    }

    // One flip: every page read-only again (a contiguous run, one call), frame bumped.
    void Arm() {
        Protect(memory, bytes, PAGE_READONLY);
        frame.fetch_add(1, std::memory_order_relaxed);
    }

    // Stores one byte to every page in `sequence` (indices), timed; volatile so each store happens.
    double Touch(const std::vector<std::size_t>& sequence) {
        volatile unsigned char* target = memory;
        const auto start = NowNs();
        for (const auto page : sequence) target[page * PageBytes + 8] = static_cast<unsigned char>(page);
        return (NowNs() - start) / static_cast<double>(sequence.size());
    }

    Results Run() {
        Results r;
        std::vector<std::size_t> sequential(pages);
        std::iota(sequential.begin(), sequential.end(), std::size_t{0});
        std::vector<double> store, faultSeq, faultRand, second, reprotectRun, reprotectScatter, fault64k, fault64kPage, chain;
        for (int round = 0; round < rounds; ++round) {
            Protect(memory, bytes, PAGE_READWRITE);
            store.push_back(Touch(order));

            unitBytes = PageBytes;
            Arm();
            auto before = faults.load();
            faultSeq.push_back(Touch(sequential));
            if (faults.load() - before != pages) Fail("sequential faults != pages");
            second.push_back(Touch(order));

            // Re-protect the written pages as one run (what a flip-time batch over a contiguous
            // tracked range does), timed per page.
            auto start = NowNs();
            Protect(memory, bytes, PAGE_READONLY);
            reprotectRun.push_back((NowNs() - start) / static_cast<double>(pages));
            frame.fetch_add(1, std::memory_order_relaxed);
            before = faults.load();
            faultRand.push_back(Touch(order));
            if (faults.load() - before != pages) Fail("random faults != pages");

            // Scattered re-protection: every 4th page, one call each.
            start = NowNs();
            std::size_t calls = 0;
            for (std::size_t page = 0; page < pages; page += 4, ++calls) Protect(memory + page * PageBytes, PageBytes, PAGE_READONLY);
            reprotectScatter.push_back((NowNs() - start) / static_cast<double>(calls));

            // 64 KiB units: one fault opens 16 pages.
            Protect(memory, bytes, PAGE_READWRITE);
            unitBytes = 16 * PageBytes;
            Arm();
            before = faults.load();
            const auto perPage = Touch(sequential);
            const auto unitFaults = faults.load() - before;
            if (unitFaults != pages / 16) Fail("64 KiB faults != pages / 16");
            fault64kPage.push_back(perPage);
            fault64k.push_back(perPage * static_cast<double>(pages) / static_cast<double>(unitFaults));
            unitBytes = PageBytes;

            // Three passing handlers ahead of ours.
            std::vector<void*> passing;
            for (int i = 0; i < 3; ++i) passing.push_back(AddVectoredExceptionHandler(1, PassingHandler));
            Arm();
            chain.push_back(Touch(order));
            for (auto* handler : passing) RemoveVectoredExceptionHandler(handler);
        }
        Protect(memory, bytes, PAGE_READWRITE);
        r.storeNs = Median(store);
        r.faultSeqNs = Median(faultSeq);
        r.faultRandNs = Median(faultRand);
        r.secondNs = Median(second);
        r.reprotectRunNsPerPage = Median(reprotectRun);
        r.reprotectScatterNsPerCall = Median(reprotectScatter);
        r.fault64kNsPerFault = Median(fault64k);
        r.fault64kNsPerPage = Median(fault64kPage);
        r.chainNs = Median(chain);
        return r;
    }

    // W threads, each faulting its own slice of pages at once: the median per-fault latency.
    double Parallel(unsigned workers) {
        std::vector<double> perFault;
        for (int round = 0; round < rounds; ++round) {
            unitBytes = PageBytes;
            Arm();
            std::vector<double> latency(workers);
            std::atomic<unsigned> ready{0};
            std::atomic<bool> go{false};
            std::vector<std::thread> threads;
            for (unsigned w = 0; w < workers; ++w) {
                threads.emplace_back([&, w] {
                    std::vector<std::size_t> mine;
                    for (std::size_t i = w; i < pages; i += workers) mine.push_back(order[i]);
                    ++ready;
                    while (!go.load(std::memory_order_acquire)) {}
                    latency[w] = Touch(mine);
                });
            }
            while (ready.load() != workers) {}
            go = true;
            for (auto& thread : threads) thread.join();
            perFault.push_back(Median(latency));
        }
        Protect(memory, bytes, PAGE_READWRITE);
        return Median(perFault);
    }

private:
    std::size_t pages;
    int rounds;
    std::size_t bytes = 0;
    unsigned char* memory = nullptr;
    std::vector<std::uint32_t> pageStamps;
    std::vector<std::size_t> order;
};

}

int main(int argc, char** argv) {
    std::size_t pages = 4096;
    int rounds = 7;
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--quick") quick = true;
        else if (arg == "--pages" && i + 1 < argc) pages = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (arg == "--rounds" && i + 1 < argc) rounds = std::atoi(argv[++i]);
    }
    if (quick) {
        pages = std::min<std::size_t>(pages, 1024);
        rounds = std::min(rounds, 3);
    }
    pages = std::max<std::size_t>(16, pages / 16 * 16);
    auto* handler = AddVectoredExceptionHandler(1, TrackingHandler);
    if (handler == nullptr) Fail("AddVectoredExceptionHandler");
    const unsigned processors = std::max(2u, std::thread::hardware_concurrency());
    std::vector<unsigned> busyCounts{0, 4, processors / 2, processors - 2};
    if (quick) busyCounts = {0, processors / 2};
    std::sort(busyCounts.begin(), busyCounts.end());
    busyCounts.erase(std::unique(busyCounts.begin(), busyCounts.end()), busyCounts.end());
    std::printf("[fault-cost] machine: %u logical processors, %zu pages (%zu KiB) per round, %d rounds; ns medians\n", processors, pages, pages * PageBytes / 1024, rounds);
    double worstFault = 0;
    double worstReprotect = 0;
    for (const auto busy : busyCounts) {
        Results r;
        {
            BusyThreads load(busy);
            Bench bench(pages, rounds);
            r = bench.Run();
        }
        worstFault = std::max({worstFault, r.faultSeqNs, r.faultRandNs});
        worstReprotect = std::max(worstReprotect, r.reprotectRunNsPerPage);
        std::printf("[fault-cost] busy=%u store=%.1f fault_seq=%.0f fault_rand=%.0f second_touch=%.1f reprotect_run_per_page=%.1f reprotect_scattered_per_call=%.0f fault64k_per_fault=%.0f fault64k_per_page=%.0f chain3=%.0f\n",
                    busy, r.storeNs, r.faultSeqNs, r.faultRandNs, r.secondNs, r.reprotectRunNsPerPage, r.reprotectScatterNsPerCall, r.fault64kNsPerFault, r.fault64kNsPerPage, r.chainNs);
        std::fflush(stdout);
    }
    std::vector<unsigned> workerCounts{2, 4, 8};
    if (quick) workerCounts = {4};
    for (const auto workers : workerCounts) {
        if (workers >= processors) continue;
        Bench bench(pages, rounds);
        const auto latency = bench.Parallel(workers);
        worstFault = std::max(worstFault, latency);
        std::printf("[fault-cost] parallel workers=%u fault_latency=%.0f\n", workers, latency);
        std::fflush(stdout);
    }
    RemoveVectoredExceptionHandler(handler);
    if (handlerFailures.load() != 0) Fail("the handler's VirtualProtect failed");
    // Threshold: 2 ms per frame of faults on the guest's critical path.
    const double budgetNs = 2'000'000.0;
    std::printf("[fault-cost] threshold: worst fault %.0f ns -> %.0f pages/f fit in 2 ms of guest critical path (re-protect at flips %.1f ns/page off that path: %.0f pages cost 1 ms)\n",
                worstFault, budgetNs / std::max(1.0, worstFault), worstReprotect, 1'000'000.0 / std::max(0.1, worstReprotect));
    return 0;
}
