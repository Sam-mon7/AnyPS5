#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using namespace ShaderRecompiler;

constexpr std::uint32_t Records = 4;
constexpr std::uint32_t Textures = 3;

struct alignas(4096) GuestData {
    std::array<std::array<std::uint32_t, 8>, Records> table{};
    std::array<std::uint32_t, 64> output{};
};

alignas(256) std::array<std::array<std::uint32_t, 64>, Textures> textures{};
std::array<GuestData, 3> guest;

alignas(256) constexpr std::array<std::uint32_t, 46> WholeWave32{
    0x36020083u, 0x2c1c0082u, 0x7e1e0280u, 0x7e140280u, 0xe0382000u, 0x80000201u, 0xe0382010u, 0x80000601u,
    0xbf8c3f70u, 0xbe94037eu, 0xbe961314u, 0xd7600008u, 0x00002d02u, 0xd7600009u, 0x00002d03u, 0xd760000au,
    0x00002d04u, 0xd760000bu, 0x00002d05u, 0xd760000cu, 0x00002d06u, 0xd760000du, 0x00002d07u, 0xd760000eu,
    0x00002d08u, 0xd760000fu, 0x00002d09u, 0xbe98037eu, 0x7da40408u, 0x7da40609u, 0x7da4080au, 0x7da40a0bu,
    0x7da40c0cu, 0x7da40e0du, 0x7da4100eu, 0x7da4120fu, 0xf0000f08u, 0x00020a0eu, 0xbe9a037eu, 0x8a141a14u,
    0xbefe0318u, 0xbf85ffe0u, 0xbf8c3f70u, 0xe0702000u, 0x80010a00u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 50> PartialWave32{
    0x36020083u, 0x2c1c0082u, 0x7e1e0280u, 0x7e140280u, 0x36200087u, 0xbe9c037eu, 0x7daa2087u, 0xe0382000u,
    0x80000201u, 0xe0382010u, 0x80000601u, 0xbf8c3f70u, 0xbe94037eu, 0xbe961314u, 0xd7600008u, 0x00002d02u,
    0xd7600009u, 0x00002d03u, 0xd760000au, 0x00002d04u, 0xd760000bu, 0x00002d05u, 0xd760000cu, 0x00002d06u,
    0xd760000du, 0x00002d07u, 0xd760000eu, 0x00002d08u, 0xd760000fu, 0x00002d09u, 0xbe98037eu, 0x7da40408u,
    0x7da40609u, 0x7da4080au, 0x7da40a0bu, 0x7da40c0cu, 0x7da40e0du, 0x7da4100eu, 0x7da4120fu, 0xf0000f08u,
    0x00020a0eu, 0xbe9a037eu, 0x8a141a14u, 0xbefe0318u, 0xbf85ffe0u, 0xbf8c3f70u, 0xbefe031cu, 0xe0702000u,
    0x80010a00u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 50> PartialWave64{
    0x36020083u, 0x2c1c0082u, 0x7e1e0280u, 0x7e140280u, 0x36200087u, 0xbe9c047eu, 0x7daa2087u, 0xe0382000u,
    0x80000201u, 0xe0382010u, 0x80000601u, 0xbf8c3f70u, 0xbe94047eu, 0xbe961414u, 0xd7600008u, 0x00002d02u,
    0xd7600009u, 0x00002d03u, 0xd760000au, 0x00002d04u, 0xd760000bu, 0x00002d05u, 0xd760000cu, 0x00002d06u,
    0xd760000du, 0x00002d07u, 0xd760000eu, 0x00002d08u, 0xd760000fu, 0x00002d09u, 0xbe98047eu, 0x7da40408u,
    0x7da40609u, 0x7da4080au, 0x7da40a0bu, 0x7da40c0cu, 0x7da40e0du, 0x7da4100eu, 0x7da4120fu, 0xf0000f08u,
    0x00020a0eu, 0xbe9a047eu, 0x8a941a14u, 0xbefe0418u, 0xbf85ffe0u, 0xbf8c3f70u, 0xbefe041cu, 0xe0702000u,
    0x80010a00u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 59> StaleWave32{
    0x36020083u, 0x2c1c0082u, 0x7e1e0280u, 0x7e140280u, 0x7e040208u, 0x7e060209u, 0x7e08020au, 0x7e0a020bu,
    0x7e0c020cu, 0x7e0e020du, 0x7e10020eu, 0x7e12020fu, 0x36200087u, 0xbe9c037eu, 0x7daa2087u, 0xe0382000u,
    0x80000201u, 0xe0382010u, 0x80000601u, 0xbf8c3f70u, 0xbefe031cu, 0xbe94037eu, 0xbe961314u, 0xd7600008u,
    0x00002d02u, 0xd7600009u, 0x00002d03u, 0xd760000au, 0x00002d04u, 0xd760000bu, 0x00002d05u, 0xd760000cu,
    0x00002d06u, 0xd760000du, 0x00002d07u, 0xd760000eu, 0x00002d08u, 0xd760000fu, 0x00002d09u, 0xbe98037eu,
    0x7da40408u, 0x7da40609u, 0x7da4080au, 0x7da40a0bu, 0x7da40c0cu, 0x7da40e0du, 0x7da4100eu, 0x7da4120fu,
    0xf0000f08u, 0x00020a0eu, 0xbe9a037eu, 0x8a141a14u, 0xbefe0318u, 0xbf85ffe0u, 0xbf8c3f70u, 0xbefe031cu,
    0xe0702000u, 0x80010a00u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 37> FirstLaneWave32{
    0x36020083u, 0x2c1c0082u, 0x7e1e0280u, 0x7e140280u, 0xe0382000u, 0x80000201u, 0xe0382010u, 0x80000601u,
    0xbf8c3f70u, 0xbe94037eu, 0x7e100502u, 0x7e120503u, 0x7e140504u, 0x7e160505u, 0x7e180506u, 0x7e1a0507u,
    0x7e1c0508u, 0x7e1e0509u, 0xbe98037eu, 0x7da40408u, 0x7da40609u, 0x7da4080au, 0x7da40a0bu, 0x7da40c0cu,
    0x7da40e0du, 0x7da4100eu, 0x7da4120fu, 0xf0000f08u, 0x00020a0eu, 0xbe9a037eu, 0x8a7e1a18u, 0xbf85ffeau,
    0xbf8c3f70u, 0xbefe0314u, 0xe0702000u, 0x80010a00u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 42> Compare64Wave32{
    0x36020083u, 0x2c1c0082u, 0x7e1e0280u, 0x7e140280u, 0xe0382000u, 0x80000201u, 0xe0382010u, 0x80000601u,
    0xbf8c3f70u, 0xbe94037eu, 0xbe961314u, 0xd7600008u, 0x00002d02u, 0xd7600009u, 0x00002d03u, 0xd760000au,
    0x00002d04u, 0xd760000bu, 0x00002d05u, 0xd760000cu, 0x00002d06u, 0xd760000du, 0x00002d07u, 0xd760000eu,
    0x00002d08u, 0xd760000fu, 0x00002d09u, 0xbe98037eu, 0x7de40408u, 0x7de4080au, 0x7de40c0cu, 0x7de4100eu,
    0xf0000f08u, 0x00020a0eu, 0xbe9a037eu, 0x8a141a14u, 0xbefe0318u, 0xbf85ffe4u, 0xbf8c3f70u, 0xe0702000u,
    0x80010a00u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 68> MaskedStaleWave32{
    0x36020083u, 0x2c1c0082u, 0x7e1e0280u, 0x7e140280u, 0x7e040280u, 0x7e060280u, 0x7e080280u, 0x7e0a0280u,
    0x7e0c0280u, 0x7e0e0280u, 0x7e100280u, 0x7e120280u, 0x36200087u, 0xbe9c037eu, 0x7da42087u, 0x7e040208u,
    0x7e060209u, 0x7e08020au, 0x7e0a020bu, 0x7e0c020cu, 0x7e0e020du, 0x7e10020eu, 0x7e12020fu, 0xbefe031cu,
    0x7daa2087u, 0xe0382000u, 0x80000201u, 0xe0382010u, 0x80000601u, 0xbf8c3f70u, 0xbefe031cu, 0xbe94037eu,
    0xbe961314u, 0xd7600008u, 0x00002d02u, 0xd7600009u, 0x00002d03u, 0xd760000au, 0x00002d04u, 0xd760000bu,
    0x00002d05u, 0xd760000cu, 0x00002d06u, 0xd760000du, 0x00002d07u, 0xd760000eu, 0x00002d08u, 0xd760000fu,
    0x00002d09u, 0xbe98037eu, 0x7da40408u, 0x7da40609u, 0x7da4080au, 0x7da40a0bu, 0x7da40c0cu, 0x7da40e0du,
    0x7da4100eu, 0x7da4120fu, 0xf0000f08u, 0x00020a0eu, 0xbe9a037eu, 0x8a141a14u, 0xbefe0318u, 0xbf85ffe0u,
    0xbf8c3f70u, 0xe0702000u, 0x80010a00u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 45> SevenDwordsWave32{
    0x36020083u, 0x2c1c0082u, 0x7e1e0280u, 0x7e140280u, 0xe0382000u, 0x80000201u, 0xe0382010u, 0x80000601u,
    0xbf8c3f70u, 0xbe94037eu, 0xbe961314u, 0xd7600008u, 0x00002d02u, 0xd7600009u, 0x00002d03u, 0xd760000au,
    0x00002d04u, 0xd760000bu, 0x00002d05u, 0xd760000cu, 0x00002d06u, 0xd760000du, 0x00002d07u, 0xd760000eu,
    0x00002d08u, 0xd760000fu, 0x00002d09u, 0xbe98037eu, 0x7da40408u, 0x7da40609u, 0x7da4080au, 0x7da40a0bu,
    0x7da40c0cu, 0x7da40e0du, 0x7da4100eu, 0xf0000f08u, 0x00020a0eu, 0xbe9a037eu, 0x8a141a14u, 0xbefe0318u,
    0xbf85ffe1u, 0xbf8c3f70u, 0xe0702000u, 0x80010a00u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 46> OutsideWave32{
    0x36020083u, 0x2c1c0082u, 0x7e1e0280u, 0x7e140280u, 0xe0382000u, 0x80000201u, 0xe0382010u, 0x80000601u,
    0xbf8c3f70u, 0xbe94037eu, 0xbe961314u, 0xd7600008u, 0x00002d02u, 0xd7600009u, 0x00002d03u, 0xd760000au,
    0x00002d04u, 0xd760000bu, 0x00002d05u, 0xd760000cu, 0x00002d06u, 0xd760000du, 0x00002d07u, 0xd760000eu,
    0x00002d08u, 0xd760000fu, 0x00002d09u, 0xbe98037eu, 0x7da40408u, 0x7da40609u, 0x7da4080au, 0x7da40a0bu,
    0x7da40c0cu, 0x7da40e0du, 0x7da4100eu, 0x7da4120fu, 0xbe9a037eu, 0x8a141a14u, 0xbefe0318u, 0xf0000f08u,
    0x00020a0eu, 0xbf85ffe0u, 0xbf8c3f70u, 0xe0702000u, 0x80010a00u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 46> StoreWave32{
    0x36020083u, 0x2c1c0082u, 0x7e1e0280u, 0x7e140280u, 0xe0382000u, 0x80000201u, 0xe0382010u, 0x80000601u,
    0xbf8c3f70u, 0xbe94037eu, 0xbe961314u, 0xd7600008u, 0x00002d02u, 0xd7600009u, 0x00002d03u, 0xd760000au,
    0x00002d04u, 0xd760000bu, 0x00002d05u, 0xd760000cu, 0x00002d06u, 0xd760000du, 0x00002d07u, 0xd760000eu,
    0x00002d08u, 0xd760000fu, 0x00002d09u, 0xbe98037eu, 0x7da40408u, 0x7da40609u, 0x7da4080au, 0x7da40a0bu,
    0x7da40c0cu, 0x7da40e0du, 0x7da4100eu, 0x7da4120fu, 0xf0200f08u, 0x00020a0eu, 0xbe9a037eu, 0x8a141a14u,
    0xbefe0318u, 0xbf85ffe0u, 0xbf8c3f70u, 0xe0702000u, 0x80010a00u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 46> LodWave32{
    0x36020083u, 0x2c1c0082u, 0x7e1e0280u, 0x7e140280u, 0xe0382000u, 0x80000201u, 0xe0382010u, 0x80000601u,
    0xbf8c3f70u, 0xbe94037eu, 0xbe961314u, 0xd7600008u, 0x00002d02u, 0xd7600009u, 0x00002d03u, 0xd760000au,
    0x00002d04u, 0xd760000bu, 0x00002d05u, 0xd760000cu, 0x00002d06u, 0xd760000du, 0x00002d07u, 0xd760000eu,
    0x00002d08u, 0xd760000fu, 0x00002d09u, 0xbe98037eu, 0x7da40408u, 0x7da40609u, 0x7da4080au, 0x7da40a0bu,
    0x7da40c0cu, 0x7da40e0du, 0x7da4100eu, 0x7da4120fu, 0xf1800308u, 0x00820a0eu, 0xbe9a037eu, 0x8a141a14u,
    0xbefe0318u, 0xbf85ffe0u, 0xbf8c3f70u, 0xe0702000u, 0x80010a00u, 0xbf810000u,
};

std::array<std::uint32_t, 4> Buffer(const void* pointer, std::uint32_t stride, std::uint32_t records) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (stride << 16u), records, 0x01016facu};
}

std::array<std::uint32_t, 8> Image(std::uint32_t texture) {
    const auto address = reinterpret_cast<std::uintptr_t>(textures[texture].data());
    const std::uint32_t format = texture == 1u ? 22u : 20u;
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u) | (format << 20u) | (3u << 30u), 7u, 0x90000facu, 0u, 0u, 0u, 0u};
}

std::uint32_t TextureOf(std::uint32_t iteration, std::uint32_t key) {
    return key == Records - 1u - iteration ? Textures : (key + iteration) % Textures;
}

bool DecoratesNonUniform(const std::vector<std::uint32_t>& words) {
    for (std::size_t index = 5u; index < words.size();) {
        const auto count = words[index] >> 16u;
        if (count == 0u) return false;
        if ((words[index] & 0xffffu) == spv::OpDecorate && count >= 3u && index + 2u < words.size() && words[index + 2u] == spv::DecorationNonUniform) return true;
        index += count;
    }
    return false;
}

RecompileRequest Request(const AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::uint32_t wave, std::uint32_t lanes, std::span<const std::uint32_t> userData, const std::array<MemoryRegion, 1>& regions) {
    const ShaderComputeStageInfo compute{{lanes, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    return {{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}}, {wave, 0u, userData, compute, std::nullopt, std::nullopt, regions}, device.Target(), {0u, 0u, 0u, 128u}};
}

std::array<std::uint32_t, 20> UserData(GuestData& data, std::uint32_t lanes) {
    const auto table = Buffer(data.table.data(), 32u, Records);
    const auto output = Buffer(data.output.data(), 4u, lanes);
    const auto fixed = Image(0u);
    constexpr std::array<std::uint32_t, 4> sampler{0x00000092u, 0x00fff000u, 0u, 0u};
    std::array<std::uint32_t, 20> userData{};
    std::copy(table.begin(), table.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4u);
    std::copy(fixed.begin(), fixed.end(), userData.begin() + 8u);
    std::copy(sampler.begin(), sampler.end(), userData.begin() + 16u);
    return userData;
}

void Run(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::uint32_t wave, bool partial, const std::string& what) {
    for (std::uint32_t texture = 0u; texture < Textures; ++texture) {
        for (std::uint32_t x = 0u; x < textures[texture].size(); ++x) textures[texture][x] = 0x1000u * (texture + 1u) + x;
    }
    const auto lanes = wave == 64u ? 64u : std::min(32u, device.Target().subgroupSize);
    CompiledShaderArtifact artifact;
    for (std::uint32_t iteration = 0u; iteration < guest.size(); ++iteration) {
        auto& data = guest[iteration];
        for (std::uint32_t key = 0u; key < Records; ++key) {
            const auto texture = TextureOf(iteration, key);
            data.table[key] = texture == Textures ? std::array<std::uint32_t, 8>{} : Image(texture);
        }
        data.output.fill(0xdeadbeefu);
        const auto userData = UserData(data, lanes);
        const std::array regions{MemoryRegion{reinterpret_cast<std::uintptr_t>(&data), std::as_bytes(std::span(&data, 1u))}};
        const auto shader = Recompile(Request(device, code, wave, lanes, userData, regions));
        Require(DecoratesNonUniform(shader.spirv.Words()), what + ": the per-lane table slot is not decorated NonUniform");
        if (artifact.variantId == 0u) artifact = shader;
        else Require(shader.cacheHit && shader.variantId == artifact.variantId, what + ": a new table changed the compiled artifact");
        device.Dispatch(shader, 1u, 1u, 1u);
        device.SubmitRecorded(false);
    }
    device.WaitIdle();
    for (std::uint32_t iteration = 0u; iteration < guest.size(); ++iteration) {
        for (std::uint32_t lane = 0u; lane < lanes; ++lane) {
            const auto texture = TextureOf(iteration, lane % Records);
            const bool skipped = partial && lane % 8u == 7u;
            const auto expected = skipped || texture == Textures ? 0u : textures[texture][lane / 4u];
            const auto actual = guest[iteration].output[lane];
            Require(actual == expected, what + ": iteration " + std::to_string(iteration) + " lane " + std::to_string(lane) + " read " + std::to_string(actual) + ", expected " + std::to_string(expected));
        }
    }
}

void RequireRefused(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::string& what) {
    auto& data = guest[0];
    const auto userData = UserData(data, 32u);
    const std::array regions{MemoryRegion{reinterpret_cast<std::uintptr_t>(&data), std::as_bytes(std::span(&data, 1u))}};
    auto request = Request(device, code, 32u, std::min(32u, device.Target().subgroupSize), userData, regions);
    request.useCache = false;
    bool refused = false;
    try {
        static_cast<void>(Recompile(request));
    } catch (const std::exception& error) {
        refused = std::string_view(error.what()).find("GetImageResource dword 0 is not a valid runtime value") != std::string_view::npos;
    }
    Require(refused, what + " was accepted");
}

void RunLod(AgcDriver::VulkanDevice& device, bool mixed, const std::string& what) {
    auto& data = guest[0];
    for (std::uint32_t key = 0u; key < Records; ++key) data.table[key] = Image(mixed && key % 2u == 0u ? 0u : 1u);
    const auto userData = UserData(data, 32u);
    const std::array regions{MemoryRegion{reinterpret_cast<std::uintptr_t>(&data), std::as_bytes(std::span(&data, 1u))}};
    auto request = Request(device, LodWave32, 32u, std::min(32u, device.Target().subgroupSize), userData, regions);
    request.useCache = false;
    std::string failure;
    try {
        const auto shader = Recompile(request);
        Require(DecoratesNonUniform(shader.spirv.Words()), what + ": the per-lane table slot is not decorated NonUniform");
    } catch (const std::exception& error) {
        failure = error.what();
    }
    if (mixed) Require(failure.find("different runtime modes under an implicit-derivative access") != std::string::npos, what + " was not refused: " + failure.substr(0u, 300u));
    else Require(failure.empty(), what + " failed: " + failure.substr(0u, 300u));
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device, WholeWave32, 32u, false, "wave32 waterfall over the whole wave");
        Run(*device, PartialWave32, 32u, true, "wave32 waterfall under a partial EXEC");
        if (device->Target().subgroupSize >= 32u) Run(*device, PartialWave64, 64u, true, "wave64 waterfall under a partial EXEC");
        Run(*device, FirstLaneWave32, 32u, false, "wave32 waterfall picking lanes with v_readfirstlane");
        Run(*device, Compare64Wave32, 32u, false, "wave32 waterfall comparing the T# with v_cmpx_eq_u64");
        RequireRefused(*device, StaleWave32, "a waterfall over a table T# and a T# left in the VGPRs of lanes the load skipped");
        RequireRefused(*device, MaskedStaleWave32, "a waterfall over a table T# and a T# written under EXEC to the VGPRs of lanes the load skipped");
        RequireRefused(*device, SevenDwordsWave32, "a waterfall that compares seven of the eight T# dwords");
        RequireRefused(*device, OutsideWave32, "an image load after the waterfall restores EXEC");
        RequireRefused(*device, StoreWave32, "an image store in a waterfall");
        RunLod(*device, false, "an LOD query in a waterfall over a table of one image mode");
        RunLod(*device, true, "an LOD query in a waterfall over a table of float and integer images");
        std::cout << "waterfall image tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
