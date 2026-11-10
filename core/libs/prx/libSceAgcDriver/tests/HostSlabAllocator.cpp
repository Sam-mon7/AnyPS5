#include "prx/libSceAgcDriver/Execution/include/HostAllocator.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Classes() {
    const AgcDriver::HostAllocator::Scope scope(true);
    for (std::size_t size = 1; size <= 1024; ++size) {
        auto* block = static_cast<unsigned char*>(::operator new(size));
        Require(AgcDriver::HostAllocator::Owns(block), "small allocation did not use a slab");
        block[0] = static_cast<unsigned char>(size);
        block[size - 1] = static_cast<unsigned char>(size >> 8u);
        ::operator delete(block);
    }
}

void CrossThread() {
    void* block;
    {
        const AgcDriver::HostAllocator::Scope scope(true);
        block = ::operator new(200);
    }
    const auto before = AgcDriver::HostAllocator::GetCounters().remoteFrees.load();
    std::thread([block] { ::operator delete(block); }).join();
    Require(AgcDriver::HostAllocator::GetCounters().remoteFrees.load() == before + 1, "cross-thread free was not counted");
    const AgcDriver::HostAllocator::Scope scope(true);
    void* reused = ::operator new(200);
    Require(AgcDriver::HostAllocator::Owns(reused), "remote free was not reusable");
    ::operator delete(reused);
}

void Foreign() {
    const AgcDriver::HostAllocator::Scope scope(true);
    const auto before = AgcDriver::HostAllocator::GetCounters().releaseForeign.load();
    void* unsized = std::malloc(73);
    void* sized = std::malloc(64);
    Require(unsized != nullptr && sized != nullptr, "malloc failed");
    ::operator delete(unsized);
    ::operator delete(sized, std::size_t{64});
    std::filesystem::path path(std::string(512, 'a'));
    path /= std::string(512, 'b');
    Require(AgcDriver::HostAllocator::GetCounters().releaseForeign.load() >= before + 2, "foreign frees took the slab path");
}

void Toggle() {
    void* owned;
    {
        const AgcDriver::HostAllocator::Scope scope(true);
        owned = ::operator new(64);
    }
    Require(AgcDriver::HostAllocator::Owns(owned), "enabled allocation was foreign");
    ::operator delete(owned);
    void* foreign = ::operator new(64);
    Require(!AgcDriver::HostAllocator::Owns(foreign), "disabled allocation used a slab");
    {
        const AgcDriver::HostAllocator::Scope scope(true);
        ::operator delete(foreign);
    }
}

void Exhaustion() {
    std::vector<void*> blocks;
    blocks.reserve(70000);
    const auto before = AgcDriver::HostAllocator::GetCounters().fallbackExhausted.load();
    {
        const AgcDriver::HostAllocator::Scope scope(true);
        while (AgcDriver::HostAllocator::GetCounters().fallbackExhausted.load() == before) blocks.push_back(::operator new(1024));
    }
    Require(!blocks.empty() && !AgcDriver::HostAllocator::Owns(blocks.back()), "exhaustion did not fall back");
    for (void* block : blocks) ::operator delete(block);
}

void Stress() {
    constexpr std::size_t Threads = 8;
    constexpr std::size_t Iterations = 50000;
    std::vector<void*> remote(Threads * 4096);
    {
        const AgcDriver::HostAllocator::Scope scope(true);
        for (auto& block : remote) block = ::operator new(64);
    }
    std::array<std::thread, Threads> threads;
    for (std::size_t thread = 0; thread < Threads; ++thread) {
        threads[thread] = std::thread([thread, &remote] {
            for (std::size_t i = thread * 4096; i < (thread + 1) * 4096; ++i) ::operator delete(remote[i]);
            const AgcDriver::HostAllocator::Scope scope(true);
            std::uint64_t state = thread + 1;
            for (std::size_t i = 0; i < Iterations; ++i) {
                state = state * 6364136223846793005ull + 1;
                const auto size = static_cast<std::size_t>((state >> 32u) % 1024u) + 1;
                auto* block = static_cast<unsigned char*>(::operator new(size));
                std::memset(block, static_cast<int>(state), size);
                ::operator delete(block);
            }
        });
    }
    for (auto& thread : threads) thread.join();
}

double Bench(std::size_t size, bool on) {
    constexpr std::size_t Iterations = 300000;
    const AgcDriver::HostAllocator::Scope scope(on);
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < Iterations; ++i) {
        void* block = ::operator new(size);
        static_cast<volatile unsigned char*>(block)[0] = static_cast<unsigned char>(i);
        ::operator delete(block);
    }
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / Iterations;
}

void Benchmark() {
    for (const auto size : {24u, 64u, 200u}) {
        const auto off = Bench(size, false);
        const auto on = Bench(size, true);
        std::printf("host_allocator size=%u off_ns=%.2f on_ns=%.2f\n", size, off, on);
    }
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected one test case");
        const std::string_view name = argv[1];
        if (name == "classes") Classes();
        else if (name == "cross_thread") CrossThread();
        else if (name == "foreign") Foreign();
        else if (name == "toggle") Toggle();
        else if (name == "exhaustion") Exhaustion();
        else if (name == "stress") Stress();
        else if (name == "benchmark") Benchmark();
        else throw std::runtime_error("unknown test case");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
