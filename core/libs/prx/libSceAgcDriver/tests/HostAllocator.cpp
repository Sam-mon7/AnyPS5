#include "prx/libSceAgcDriver/Execution/include/HostAllocator.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <mutex>
#include <semaphore>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#endif

namespace {

int failures = 0;

using AgcDriver::HostAllocator::Event;

std::uint64_t Count(Event event, std::size_t bucket = 0) {
    return AgcDriver::HostAllocator::counters[static_cast<std::size_t>(event)][bucket].load(std::memory_order_relaxed);
}

std::uint64_t Total(Event event) {
    std::uint64_t total = 0;
    for (const auto& bucket : AgcDriver::HostAllocator::counters[static_cast<std::size_t>(event)]) total += bucket.load(std::memory_order_relaxed);
    return total;
}

std::size_t Depth(std::size_t sizeClass) {
    return sizeClass < AgcDriver::HostAllocator::SmallClasses ? 64 : 16;
}

bool Requested(const char* name) {
    const auto* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool DepotFixRequested() {
    return Requested("APS5_ARCHB_FIX_ALLOC_DEPOT");
}

bool Census() {
    return Requested("APS5_ARCHB_ALLOCATOR_CENSUS");
}

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void FillAndVerify(unsigned char* bytes, std::size_t size, unsigned char seed) {
    for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<unsigned char>(seed + i);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != static_cast<unsigned char>(seed + i)) {
            Check(false, "a block holds the bytes written to it");
            return;
        }
    }
}

void EverySizeRoundTrips() {
    for (int round = 0; round < 4; ++round) {
        std::vector<unsigned char*> blocks;
        for (std::size_t size = 1; size <= 2048; ++size) {
            auto* block = new unsigned char[size];
#ifdef _WIN32
            Check(_msize(block) >= size, "a block holds at least the bytes asked for");
#endif
            FillAndVerify(block, size, static_cast<unsigned char>(size));
            blocks.push_back(block);
        }
        for (auto* block : blocks) delete[] block;
    }
}

void SizedDeletesKeepTheirClass() {
    for (int round = 0; round < 1000; ++round) {
        for (std::size_t size = 16; size <= 1024; size += 16) {
            void* block = ::operator new(size);
            std::memset(block, 0x5a, size);
            ::operator delete(block, size);
        }
    }
    for (std::size_t size = 1; size <= 1024; ++size) {
        auto* block = static_cast<unsigned char*>(::operator new(size));
        FillAndVerify(block, size, 3);
        ::operator delete(block, size);
    }
}

void ReleaseBoundaries() {
    for (const std::size_t size : {1, 15, 16, 17, 92, 96, 1008, 1024, 1025, 1040}) {
        for (bool array : {false, true}) {
            for (bool sized : {false, true}) {
                auto* block = static_cast<unsigned char*>(array ? ::operator new[](size) : ::operator new(size));
                FillAndVerify(block, size, 7);
                const auto queries = Count(Event::SizeQuery);
                const auto unsized = Count(Event::UnsizedRelease);
                if (array && sized) ::operator delete[](block, size);
                else if (array) ::operator delete[](block);
                else if (sized) ::operator delete(block, size);
                else ::operator delete(block);
                const bool queried = !sized || size % 16 != 0;
                Check(Count(Event::SizeQuery) - queries == queried, "unsized and unrounded releases query the heap size; rounded sized releases do not");
                Check(Count(Event::UnsizedRelease) - unsized == !sized, "unsized releases are counted");
            }
        }
    }
    void* zero = ::operator new(0);
    ::operator delete(zero, std::size_t{0});
    ::operator delete(nullptr, std::size_t{17});
}

struct Cookie {
    unsigned char data[17]{};
    static inline int alive = 0;
    Cookie() { ++alive; }
    ~Cookie() { --alive; }
};

struct Throws {
    Throws() { throw 1; }
};

struct alignas(128) Aligned {
    unsigned char data[128]{};
};

void AllocationForms() {
    for (std::size_t size = 1; size <= 1040; ++size) {
        auto* bytes = new (std::nothrow) unsigned char[size];
        Check(bytes != nullptr, "nothrow array allocation succeeds");
        if (bytes != nullptr) FillAndVerify(bytes, size, 11);
        delete[] bytes;
        std::allocator<unsigned char> allocator;
        bytes = allocator.allocate(size);
        FillAndVerify(bytes, size, 19);
        allocator.deallocate(bytes, size);
    }
    for (const std::size_t size : {1, 15, 16, 17, 60, 64}) {
        auto* array = new Cookie[size];
        Check(Cookie::alive == static_cast<int>(size), "array cookie covers every constructed element");
        for (std::size_t i = 0; i < size; ++i) FillAndVerify(array[i].data, sizeof(Cookie), 23);
        delete[] array;
        Check(Cookie::alive == 0, "sized array deletion includes its cookie");
        auto shared = std::make_shared<Cookie>();
        auto sharedArray = std::make_shared<Cookie[]>(size);
        auto separate = std::shared_ptr<Cookie>(new Cookie);
        Check(Cookie::alive == static_cast<int>(size) + 2, "shared control blocks and objects coexist");
    }
    Check(Cookie::alive == 0, "shared objects are destroyed");
    auto* scalar = new (std::nothrow) Cookie;
    delete scalar;
    for (bool array : {false, true}) {
        try {
            if (array) delete[] new (std::nothrow) Throws[3];
            else delete new (std::nothrow) Throws;
            Check(false, "throwing constructor must propagate through nothrow new");
        } catch (int) {
        }
    }
    const auto queries = Total(Event::SizeQuery);
    const auto mallocs = Total(Event::Malloc);
    const auto hits = Total(Event::CacheHit);
    auto* aligned = new Aligned[17];
    Check(reinterpret_cast<std::uintptr_t>(aligned) % alignof(Aligned) == 0, "aligned new keeps its alignment");
    delete[] aligned;
    std::allocator<Aligned> allocator;
    aligned = allocator.allocate(3);
    allocator.deallocate(aligned, 3);
    Check(Total(Event::SizeQuery) == queries && Total(Event::Malloc) == mallocs && Total(Event::CacheHit) == hits, "aligned new and delete stay outside the host allocator");
}

void ForeignSizedRelease() {
#ifdef _WIN32
    const auto runtime = GetModuleHandleA("libstdc++-6.dll");
    const auto allocate = reinterpret_cast<void* (*)(std::size_t)>(GetProcAddress(runtime, "_Znwy"));
    Check(allocate != nullptr, "the shared C++ runtime exports operator new");
    if (allocate == nullptr) return;
    for (const std::size_t size : {1, 15, 17, 31, 92, 1009, 1023}) {
        auto* block = allocate(size);
        const auto queries = Count(Event::SizeQuery);
        const auto usable = _msize(block);
        Check(usable >= size, "foreign new supplies the requested bytes");
        ::operator delete(block, size);
        Check(Count(Event::SizeQuery) == queries + 1, "foreign sized release queries actual capacity");
        const auto rounded = (size + 15) / 16 * 16;
        block = ::operator new(rounded);
        Check(_msize(block) >= rounded, "foreign unrounded allocation cannot poison a rounded cache class");
        std::memset(block, 0x5a, rounded);
        ::operator delete(block, rounded);
    }
    {
        const auto path = std::filesystem::current_path();
        Check(!path.empty(), "filesystem returns a path allocated by the shared runtime");
        const auto bytes = (path.native().capacity() + 1) * sizeof(wchar_t);
        const auto usable = _msize(const_cast<wchar_t*>(path.c_str()));
        std::printf("foreign filesystem buffer: requested=%zu usable=%zu rounded=%zu\n", bytes, usable, (bytes + 15) / 16 * 16);
    }
#endif
}

void CensusBuckets() {
    for (std::size_t bucket = 0; bucket < 64; ++bucket) {
        const auto size = bucket * 16 + 1;
        const auto full = Count(Event::DepotFullFree, bucket);
        const auto takes = Count(Event::DepotTake, bucket);
        const auto keeps = Count(Event::DepotKeep, bucket);
        std::vector<void*> blocks((AgcDriver::HostAllocator::DepotCapacity(bucket) + 2) * Depth(bucket));
        for (auto& block : blocks) block = ::operator new(size);
        for (auto* block : blocks) ::operator delete(block, size);
        for (auto& block : blocks) block = ::operator new(size);
        for (auto* block : blocks) ::operator delete(block);
        Check(Count(Event::CacheHit, bucket) > 0, "every class records cache hits");
        Check(Count(Event::Malloc, bucket) > 0, "every class records heap fall-throughs");
        Check(Count(Event::DepotFullFree, bucket) > full, "full depots record heap frees");
        Check(Count(Event::DepotTake, bucket) > takes, "empty caches record depot takes");
        Check(Count(Event::DepotKeep, bucket) > keeps, "full caches record depot keeps");
    }
    Check(Count(Event::Malloc, 64) > 0, "large allocations have a separate heap bucket");
}

void DepotCapacities(bool enabled) {
    using AgcDriver::HostAllocator::DepotCapacity;
    for (std::size_t sizeClass = 0; sizeClass < AgcDriver::HostAllocator::Classes; ++sizeClass) {
        const std::size_t expected = sizeClass < 16 && enabled ? 128 : 8;
        Check(DepotCapacity(sizeClass) == expected, "depot capacity is 128 lists for classes up to 256 bytes with the fix, otherwise 8");
    }
    Check(DepotCapacity(15) == (enabled ? 128u : 8u), "the 256-byte class is small");
    Check(DepotCapacity(16) == 8, "the 272-byte class keeps 8 lists");
}

void DepotsStayBounded(bool enabled) {
    using AgcDriver::HostAllocator::DepotCapacity;
    using AgcDriver::HostAllocator::DepotLists;
    for (const std::size_t sizeClass : {0, 1, 5, 15, 16, 40, 63}) {
        std::thread([=] {
            const auto size = (sizeClass + 1) * 16;
            const auto capacity = DepotCapacity(sizeClass);
            Check(capacity == (sizeClass < 16 && enabled ? 128u : 8u), "flooded class has the expected capacity");
            std::vector<void*> blocks((capacity + 3) * Depth(sizeClass));
            for (auto& block : blocks) block = ::operator new(size);
            Check(DepotLists(sizeClass) == 0, "allocating more than the depot holds drains it");
            const auto keeps = Count(Event::DepotKeep, sizeClass);
            const auto full = Count(Event::DepotFullFree, sizeClass);
            for (std::size_t i = 0; i < blocks.size(); ++i) {
                ::operator delete(blocks[i], size);
                if (i % Depth(sizeClass) == 0) Check(DepotLists(sizeClass) <= capacity, "a depot never holds more than its capacity");
            }
            Check(DepotLists(sizeClass) == capacity, "a flooded depot holds exactly its capacity");
            if (Census()) {
                Check(Count(Event::DepotKeep, sizeClass) - keeps == capacity, "a flooded depot keeps exactly its capacity in lists");
                Check(Count(Event::DepotFullFree, sizeClass) - full == 2 * Depth(sizeClass), "blocks beyond the depot and the cache go back to the heap");
            }
            const auto retaken = (capacity + 1) * Depth(sizeClass);
            for (std::size_t i = 0; i < retaken; ++i) blocks[i] = ::operator new(size);
            Check(DepotLists(sizeClass) == 0, "every kept list can be taken again");
            for (std::size_t i = 0; i < retaken; ++i) ::operator delete(blocks[i], size);
        }).join();
    }
}

double ProducerConsumer() {
    constexpr std::size_t sizes[] = {16, 32, 96};
    constexpr int rounds = 12;
    constexpr int warmup = 2;
    constexpr std::size_t perSize = 4096;
    std::vector<void*> batch;
    std::binary_semaphore produced(0);
    std::binary_semaphore consumed(0);
    std::uint64_t measuredMallocs = 0;
    std::uint64_t measuredAllocations = 0;
    std::thread consumer([&] {
        for (int round = 0; round < rounds; ++round) {
            produced.acquire();
            for (auto* block : batch) {
                auto* bytes = static_cast<unsigned char*>(block);
                Check(bytes[1] == 0xa5, "a block handed across threads keeps its bytes");
                ::operator delete(block, static_cast<std::size_t>(bytes[0]));
            }
            batch.clear();
            consumed.release();
        }
    });
    std::thread producer([&] {
        for (int round = 0; round < rounds; ++round) {
            std::uint64_t before = 0;
            for (const auto size : sizes) before += Count(Event::Malloc, size / 16 - 1);
            for (std::size_t i = 0; i < perSize; ++i) {
                for (const auto size : sizes) {
                    auto* block = static_cast<unsigned char*>(::operator new(size));
                    block[0] = static_cast<unsigned char>(size);
                    block[1] = 0xa5;
                    batch.push_back(block);
                }
            }
            std::uint64_t after = 0;
            for (const auto size : sizes) after += Count(Event::Malloc, size / 16 - 1);
            if (round >= warmup) {
                measuredMallocs += after - before;
                measuredAllocations += perSize * std::size(sizes);
            }
            produced.release();
            consumed.acquire();
        }
    });
    producer.join();
    consumer.join();
    const double rate = static_cast<double>(measuredMallocs) / static_cast<double>(measuredAllocations);
    std::printf("producer/consumer heap fall-throughs: %llu of %llu small allocations (%.2f%%)\n",
        static_cast<unsigned long long>(measuredMallocs), static_cast<unsigned long long>(measuredAllocations), rate * 100.0);
    return rate;
}

struct StaticLifetime {
    void* block = ::operator new(17);
    ~StaticLifetime() {
        ::operator delete(block, std::size_t{17});
        ::operator delete(::operator new(1025), std::size_t{1025});
    }
} staticLifetime;

#ifdef _WIN32
void WINAPI LateThreadRelease(void* data) {
    auto* lifetime = static_cast<StaticLifetime*>(data);
    lifetime->~StaticLifetime();
    std::free(lifetime);
}
#endif

void BlocksCrossIntoLibstdcxx() {
    for (int round = 0; round < 2000; ++round) {
        std::string grown(static_cast<std::size_t>(round % 300) + 20, 'a');
        grown.reserve(grown.size() * 3 + 1);
        grown.append(static_cast<std::size_t>(round % 500), 'b');
        std::istringstream input(std::string(static_cast<std::size_t>(round % 700) + 1, 'c') + "\nnext");
        std::string line(40, 'd');
        std::getline(input, line);
        Check(line.size() == static_cast<std::size_t>(round % 700) + 1, "getline replaces the buffer made here");
        Check(grown.size() == static_cast<std::size_t>(round % 300) + 20 + static_cast<std::size_t>(round % 500), "a grown string keeps its bytes");
    }
}

void BlocksMoveBetweenThreads() {
    constexpr int perThread = 20000;
    std::vector<std::unique_ptr<std::uint64_t[]>> made(perThread);
    std::thread maker([&] {
        for (int i = 0; i < perThread; ++i) {
            made[i] = std::make_unique<std::uint64_t[]>(static_cast<std::size_t>(i % 64) + 1);
            made[i][0] = static_cast<std::uint64_t>(i);
        }
    });
    maker.join();
    std::thread freer([&] {
        for (int i = 0; i < perThread; ++i) {
            Check(made[i][0] == static_cast<std::uint64_t>(i), "a block made on another thread keeps its bytes");
            made[i].reset();
        }
    });
    freer.join();
}

std::atomic<int> exitedThreads{0};

std::size_t PrivateBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return 0;
    return counters.PrivateUsage;
#else
    return 0;
#endif
}

void BlocksFlowToAnotherThread() {
    constexpr int rounds = 12;
    constexpr int perRound = 200000;
    std::size_t afterEarlyRounds = 0;
    for (int round = 0; round < rounds; ++round) {
        std::mutex lock;
        std::vector<unsigned char*> handed;
        std::atomic<bool> done{false};
        std::thread freer([&] {
            std::vector<unsigned char*> taken;
            for (;;) {
                const bool finished = done.load(std::memory_order_acquire);
                {
                    std::lock_guard guard(lock);
                    taken.swap(handed);
                }
                for (auto* block : taken) {
                    const auto size = static_cast<std::size_t>(block[0]) * 4 + 1;
                    Check(block[size - 1] == block[0], "a block freed on another thread keeps its bytes until then");
                    ::operator delete(block);
                }
                taken.clear();
                if (finished) break;
                std::this_thread::yield();
            }
        });
        for (int i = 0; i < perRound; ++i) {
            const auto seed = static_cast<unsigned char>(i % 256);
            const auto size = static_cast<std::size_t>(seed) * 4 + 1;
            auto* block = static_cast<unsigned char*>(::operator new(size));
            block[0] = seed;
            block[size - 1] = seed;
            std::lock_guard guard(lock);
            handed.push_back(block);
        }
        done.store(true, std::memory_order_release);
        freer.join();
        if (round == 3) afterEarlyRounds = PrivateBytes();
    }
    const auto afterLastRound = PrivateBytes();
    std::printf("private bytes after 4 and after %d rounds of blocks freed on another thread: %zu, %zu\n", rounds, afterEarlyRounds, afterLastRound);
    Check(afterLastRound < afterEarlyRounds + (64u << 20), "blocks freed on another thread do not pile up");
}

void ThreadsEndWithCachedBlocks(bool lateRelease = false) {
    constexpr int waves = 24;
    std::size_t afterEarlyWaves = 0;
#ifdef _WIN32
    const auto lateSlot = lateRelease ? FlsAlloc(LateThreadRelease) : FLS_OUT_OF_INDEXES;
    if (lateRelease && lateSlot == FLS_OUT_OF_INDEXES) {
        Check(false, "late thread-release callback is registered");
        return;
    }
#endif
    for (int wave = 0; wave < waves; ++wave) {
        std::vector<std::thread> threads;
        for (int t = 0; t < 32; ++t) {
            threads.emplace_back([=] {
#ifdef _WIN32
                if (lateSlot != FLS_OUT_OF_INDEXES) {
                    auto* storage = std::malloc(sizeof(StaticLifetime));
                    if (storage == nullptr) std::terminate();
                    auto* lifetime = new (storage) StaticLifetime;
                    if (!FlsSetValue(lateSlot, lifetime)) std::terminate();
                }
#endif
                std::vector<void*> blocks;
                for (int i = 0; i < 4000; ++i) blocks.push_back(::operator new(static_cast<std::size_t>(i % 1024) + 1));
                for (auto* block : blocks) ::operator delete(block);
                ++exitedThreads;
            });
        }
        for (auto& thread : threads) thread.join();
        if (wave == 7) afterEarlyWaves = PrivateBytes();
    }
#ifdef _WIN32
    if (lateSlot != FLS_OUT_OF_INDEXES) FlsFree(lateSlot);
#endif
    Check(exitedThreads.load() == waves * 32, "every thread ran to its end");
    const auto afterLastWave = PrivateBytes();
    std::printf("private bytes after 8 and after %d waves of ending threads: %zu, %zu\n", waves, afterEarlyWaves, afterLastWave);
    Check(afterLastWave < afterEarlyWaves + (128u << 20), "threads that end give their cached blocks back to the heap");
    auto* after = new std::uint64_t[128];
    FillAndVerify(reinterpret_cast<unsigned char*>(after), 128 * sizeof(std::uint64_t), 9);
    delete[] after;
}

}

int main(int argc, char** argv) {
    const bool depotFix = DepotFixRequested();
    if (argc > 1 && std::strcmp(argv[1], "--thread-exit") == 0) {
        ThreadsEndWithCachedBlocks(true);
        Check(Count(Event::NoCacheFree) >= static_cast<std::uint64_t>(exitedThreads.load()), "late releases work after the thread cache closes");
        DepotCapacities(depotFix);
        DepotsStayBounded(depotFix);
        return failures != 0;
    }
    const bool disabled = argc > 1 && std::strcmp(argv[1], "--disabled") == 0;
    if (argc > 1 && !disabled) {
        DepotCapacities(depotFix);
        const auto rate = ProducerConsumer();
        if (depotFix) Check(rate < 0.05, "with the depot fix, blocks freed on another thread are reused");
        else Check(rate > 0.5, "without the depot fix, most blocks freed on another thread miss the depot");
        DepotsStayBounded(depotFix);
        ReleaseBoundaries();
        AllocationForms();
        ForeignSizedRelease();
        CensusBuckets();
    }
    EverySizeRoundTrips();
    SizedDeletesKeepTheirClass();
    BlocksCrossIntoLibstdcxx();
    BlocksMoveBetweenThreads();
    BlocksFlowToAnotherThread();
    ThreadsEndWithCachedBlocks();
    if (disabled) {
        DepotCapacities(depotFix);
        DepotsStayBounded(depotFix);
        ProducerConsumer();
        for (const auto& event : AgcDriver::HostAllocator::counters) {
            for (const auto& bucket : event) Check(bucket.load(std::memory_order_relaxed) == 0, "disabled census performs no counter updates");
        }
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d host allocator checks failed\n", failures);
        return 1;
    }
    std::printf("Host allocator tests passed\n");
    return 0;
}
