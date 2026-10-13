#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::size_t SurfaceBytes = 65536;
alignas(256) std::array<std::uint32_t, 7> StoreCode{0x7e020280u, 0x7e04020cu, 0xf0201108u, 0x00000200u, 0xf0201108u, 0x00010200u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 7> CounterCode{0xf4080200u, 0xfa000000u, 0xbf8cc07fu, 0x7e000281u, 0xe0c80000u, 0x80020000u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 4> Counter{};
alignas(256) std::array<std::uint32_t, 4> CounterDescriptor{};
alignas(256) volatile std::uint32_t Done = 0;

std::uint64_t Address(const volatile void* pointer) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t Low(const volatile void* pointer) { return static_cast<std::uint32_t>(Address(pointer)); }
std::uint32_t High(const volatile void* pointer) { return static_cast<std::uint32_t>(Address(pointer) >> 32u); }

class GuestSurface {
public:
    GuestSurface() {
#ifdef _WIN32
        data = static_cast<std::uint32_t*>(GuestArena::GuestArenaAllocate_nid_postfix(SurfaceBytes, SurfaceBytes));
        if (data != nullptr) GuestArena::GuestArenaCommit_nid_postfix(data, SurfaceBytes, PAGE_READWRITE, SurfaceBytes);
#else
        void* mapped = mmap(nullptr, SurfaceBytes * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + SurfaceBytes - 1) & ~(SurfaceBytes - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + SurfaceBytes != begin + SurfaceBytes * 2) munmap(reinterpret_cast<void*>(aligned + SurfaceBytes), begin + SurfaceBytes - aligned);
            data = reinterpret_cast<std::uint32_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, SurfaceBytes);
        }
#endif
        Require(data != nullptr, "cannot allocate the indirect argument surface");
        std::fill_n(data, SurfaceBytes / sizeof(*data), 0u);
        data[0] = 1;
        data[1] = 2;
        data[2] = 2;
        GuestAllocations::Mutation().Add(data, SurfaceBytes, true, true, true);
    }

    ~GuestSurface() {
        GuestAllocations::Mutation().Remove(data);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(data, SurfaceBytes);
        GuestArena::GuestArenaRelease_nid_postfix(data, SurfaceBytes);
#else
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, SurfaceBytes);
        munmap(data, SurfaceBytes);
#endif
    }

    GuestSurface(const GuestSurface&) = delete;
    GuestSurface& operator=(const GuestSurface&) = delete;
    std::uint32_t* Data() const { return data; }

private:
    std::uint32_t* data = nullptr;
};

void Register(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

void Program(std::vector<std::uint32_t>& words, const void* code, std::span<const std::uint32_t> userData) {
    const auto address = Address(code);
    Register(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    Register(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const auto offset : {0x207u, 0x208u, 0x209u}) Register(words, offset, 1);
    Register(words, 0x212, 0);
    Register(words, 0x213, static_cast<std::uint32_t>(userData.size()) << 1u);
    std::uint32_t offset = 0x240;
    for (const auto value : userData) Register(words, offset++, value);
}

}

int main(int argc, char** argv) {
    try {
        const bool threadDimensions = argc == 2 && std::string_view(argv[1]) == "--thread-dimensions";
        Require(argc == 1 || threadDimensions, "unknown image indirect test option");
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        GuestSurface surface;
        const auto address = Address(surface.Data());
        const std::array<std::uint32_t, 8> texture{
            static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (20u << 20u) | (3u << 30u),
            31u | (127u << 14u), 0xfacu | (0x1bu << 20u) | (9u << 28u), 0u, 0u, 0u, 0u
        };
        CounterDescriptor = {Low(Counter.data()), High(Counter.data()) & 0xffffu, sizeof(Counter), 0x31016facu};
        const std::array<std::uint32_t, 2> counterData{Low(CounterDescriptor.data()), High(CounterDescriptor.data())};
        std::vector<std::uint32_t> warmUp;
        Program(warmUp, CounterCode.data(), counterData);
        warmUp.insert(warmUp.end(), {0xc0031500u, 1u, 1u, 1u, 0x8041u});
        warmUp.insert(warmUp.end(), {0xc0033700u, 0x00100200u, Low(&Done), High(&Done), 2u});
        Packet first{warmUp.data(), static_cast<std::uint32_t>(warmUp.size()), 0, {}};
        Require(sceAgcDriverSubmitAcb(0x20, &first) == 0, "warm-up submit failed");
        AgcDriverWaitIdle_nid_postfix();
        Require(Done == 2 && Counter[0] == 1, "the warm-up counter did not run");
        Counter[0] = 0;
        std::array<std::uint32_t, 13> imageData{};
        std::copy(texture.begin(), texture.end(), imageData.begin() + 4);
        imageData[12] = 3;
        std::vector<std::uint32_t> words;
        Program(words, StoreCode.data(), imageData);
        words.insert(words.end(), {0xc0031500u, 1u, 1u, 1u, 0x8041u});
        Program(words, CounterCode.data(), counterData);
        words.insert(words.end(), {0xc0021102u, 1u, Low(surface.Data()), High(surface.Data())});
        words.insert(words.end(), {0xc0011600u, 0u, threadDimensions ? 0x8061u : 0x8041u});
        words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(&Done), High(&Done), 1u});
        Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
        Require(sceAgcDriverSubmitAcb(0x20, &packet) == 0, "image argument submit failed");
        AgcDriverWaitIdle_nid_postfix();
        Require(Done == 1, "the image argument submission did not finish");
        Require(surface.Data()[0] == 3 && surface.Data()[1] == 2 && surface.Data()[2] == 2, "image write-back arguments " + std::to_string(surface.Data()[0]) + "x" + std::to_string(surface.Data()[1]) + "x" + std::to_string(surface.Data()[2]) + ", counter " + std::to_string(Counter[0]));
        Require(Counter[0] == 12, "image-based indirect dispatch ran " + std::to_string(Counter[0]) + " groups, expected 12");
        std::cout << "image indirect arguments written ahead test passed\n";
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (const std::exception& shutdownError) { std::cerr << shutdownError.what() << '\n'; }
        return 1;
    }
}
