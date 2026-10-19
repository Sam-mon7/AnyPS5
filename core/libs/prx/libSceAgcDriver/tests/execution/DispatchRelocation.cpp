#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

alignas(256) std::array<std::uint32_t, 12> ReaderCode{0xf4040100u, 0xfa000000u, 0xf4080201u, 0xfa000000u, 0xbf8cc07fu, 0xf4000182u, 0xfa000000u, 0xbf8cc07fu, 0x7e000206u, 0xe0700000u, 0x80020000u, 0xbf810000u};

struct Slots {
    alignas(256) std::array<std::array<std::uint32_t, 4>, 4> tables{};
    alignas(256) std::array<std::array<std::uint32_t, 4>, 4> descriptors{};
    alignas(256) std::array<std::uint32_t, 4> first{};
    alignas(256) std::array<std::uint32_t, 4> second{};
    alignas(256) std::array<std::uint32_t, 4> output{};
};

Slots Memory;

std::uint32_t Low(const void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

void SetShaderRegister(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

std::uint32_t Read(std::size_t slot) {
    Memory.output = {};
    std::vector<std::uint32_t> words;
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ReaderCode.data()));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const std::uint32_t offset : {0x207u, 0x208u, 0x209u}) SetShaderRegister(words, offset, 1);
    SetShaderRegister(words, 0x212, 0);
    SetShaderRegister(words, 0x213, 4u << 1u);
    const std::array<std::uint32_t, 4> userData{Low(Memory.tables[slot].data()), High(Memory.tables[slot].data()), Low(Memory.descriptors[slot].data()), High(Memory.descriptors[slot].data())};
    std::uint32_t offset = 0x240;
    for (const auto value : userData) SetShaderRegister(words, offset++, value);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitAcb(0x20, &packet) == 0, "compute submit failed");
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Memory.output.data()), sizeof(Memory.output));
    return Memory.output[0];
}

void Point(std::size_t slot, const std::array<std::uint32_t, 4>& value) {
    Memory.tables[slot] = {Low(value.data()), High(value.data()), 0, 0};
    Memory.descriptors[slot] = {Low(Memory.output.data()), High(Memory.output.data()) & 0xffffu, sizeof(Memory.output), 0x31016facu};
}

void MovedTablesReuseTheDispatch(bool relocating) {
    const auto counts = [] { return AgcDriver::DriverDetail::Driver::Get().DispatchRelocationCounts(); };
    Memory.first[0] = 0x11111111u;
    Memory.second[0] = 0x22222222u;
    Point(0, Memory.first);
    Require(Read(0) == 0x11111111u, "the first dispatch did not store the value its table points at");
    Require(counts().hits == 0, "the first dispatch was a relocated hit");

    Point(1, Memory.first);
    Require(Read(1) == 0x11111111u, "a dispatch whose table moved stored the wrong value");
    Require(counts().hits == (relocating ? 1u : 0u), relocating ? "a dispatch whose table moved with the same contents did not reuse the cached dispatch" : "a dispatch was relocated with APS5_DISPATCH_RELOCATION not exactly 1");

    Point(2, Memory.second);
    Require(Read(2) == 0x22222222u, "a dispatch whose moved table points elsewhere stored the wrong value");
    Require(counts().hits == (relocating ? 1u : 0u), "a dispatch whose moved table holds other contents was relocated");

    Require(Read(1) == 0x11111111u, "a repeated dispatch at a relocated address stored the wrong value");
    const auto repeated = counts();
    Require(repeated.hits == (relocating ? 1u : 0u) && repeated.inserted == (relocating ? 1u : 0u), "a repeated dispatch at a relocated address did not hit the entry kept under its own key");

    Memory.first[0] = 0x33333333u;
    Point(3, Memory.first);
    Require(Read(3) == 0x33333333u, "a relocated dispatch whose pointed-at value changed stored a stale value");
    const auto changed = counts();
    if (relocating) {
        Require(changed.donors == 2u && changed.hits + changed.differing == 2u, "a relocated dispatch whose pointed-at value changed neither missed nor refreshed its data words (donors " + std::to_string(changed.donors) + ", hits " + std::to_string(changed.hits) + ", differing " + std::to_string(changed.differing) + ")");
    } else {
        Require(changed.lookups == 0u && changed.donors == 0u && changed.hits == 0u, "relocation lookups ran with APS5_DISPATCH_RELOCATION not exactly 1");
    }
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        const auto* value = std::getenv("APS5_DISPATCH_RELOCATION");
        const bool relocating = value != nullptr && std::string_view(value) == "1";
        MovedTablesReuseTheDispatch(relocating);
        AgcDriverShutdown_nid_postfix();
        std::cout << "dispatch relocation test passed (" << (relocating ? "relocating" : "not relocating") << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
