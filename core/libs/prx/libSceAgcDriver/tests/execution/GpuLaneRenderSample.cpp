#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Lanes/SubmissionCursor.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include <array>
#include <bit>
#include <iostream>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{0xe0382000u, 0x80000005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 10> SampleCode{
    0x7e3c02ffu, 0x3f000000u, 0x7e3e02ffu, 0x3f000000u, 0xf09c0108u,
    0x00610a1eu, 0x34060082u, 0xe0701000u, 0x80000a03u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::array<float, 4>, 3> Triangle{{{-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}}};
alignas(256) std::array<std::byte, 65536> Color{};
alignas(256) std::array<std::uint32_t, 64> Output{};

struct Header { Shader shader{}; ShaderUserData userData{}; };
alignas(8) Header VertexHeader{}, PixelHeader{};

std::uint64_t Address(const void* value) { return reinterpret_cast<std::uintptr_t>(value); }

void Register(Header& header, std::span<const std::uint32_t> code, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = code.size_bytes();
    header.shader.code = code.data();
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void RegisterWord(std::vector<std::uint32_t>& words, std::uint32_t opcode, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0010000u | (opcode << 8u), offset, value});
}

std::vector<std::uint32_t> Frame() {
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        std::pair{0x2d5u, 0x2000u}, {0x1b6u, 0u}, {0x207u, 0u}, {0x200u, 0u}, {0x203u, 0x800u},
        {0x2dcu, 0xaa00u}, {0x2f8u, 0u}, {0x292u, 2u}, {0x293u, 0u},
        {0x80u, 0u}, {0x8du, 0u}, {0x83u, 0xffffu}, {0x8cu, 0xau}, {0x2f9u, 0x2du},
        {0x313u, 0x6000u}, {0x30eu, 0xffffffffu}, {0x30fu, 0xffffffffu},
        {0x206u, 0x43fu}, {0x204u, 0x80000u}, {0x205u, 0x240u}, {0x8eu, 0xfu}, {0x8fu, 0xfu}, {0x202u, 0xcc0010u},
        {0x1c4u, 0u}, {0x1c5u, 9u}, {0x1c3u, 4u}, {0x31cu, 0x710u}, {0x31bu, 0u}, {0x31du, 0u},
        {0x3b0u, (63u << 14u) | 3u}, {0x3b8u, 0x9000000u}, {0x1e0u, 0u},
        {0xcu, 0u}, {0xdu, 0x40040u}, {0x81u, 0x80000000u}, {0x82u, 0x40040u},
        {0x90u, 0x80000000u}, {0x91u, 0x40040u}, {0x94u, 0x80000000u}, {0x95u, 0x40040u},
        {0x318u, static_cast<std::uint32_t>(Address(Color.data()) >> 8u)}, {0x390u, static_cast<std::uint32_t>(Address(Color.data()) >> 40u)},
        {0x10fu, std::bit_cast<std::uint32_t>(32.0f)}, {0x110u, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111u, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112u, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113u, std::bit_cast<std::uint32_t>(1.0f)}, {0x114u, 0u}, {0xb4u, 0u}, {0xb5u, std::bit_cast<std::uint32_t>(1.0f)}
    };
    std::vector<std::uint32_t> words;
    RegisterWord(words, 0x79, 0x242, 4);
    for (const auto& [offset, value] : context) RegisterWord(words, 0x69, offset, value);
    const auto program = [&](std::uint32_t low, std::uint64_t address) {
        RegisterWord(words, 0x76, low, static_cast<std::uint32_t>(address >> 8u));
        RegisterWord(words, 0x76, low + 1, static_cast<std::uint32_t>(address >> 40u));
    };
    program(0xc8, Address(VertexCode.data()));
    program(0x8, Address(PixelCode.data()));
    RegisterWord(words, 0x76, 0x8b, 4u << 1u);
    RegisterWord(words, 0x76, 0xb, 1u << 1u);
    const std::array<std::uint32_t, 4> vertex{static_cast<std::uint32_t>(Address(Triangle.data())), static_cast<std::uint32_t>(Address(Triangle.data()) >> 32u) | (16u << 16u), 3, 0x01016facu};
    for (std::uint32_t index = 0; index < vertex.size(); ++index) RegisterWord(words, 0x76, 0x8c + index, vertex[index]);
    for (std::uint32_t pass = 0; pass < 2; ++pass) {
        RegisterWord(words, 0x76, 0xc, std::bit_cast<std::uint32_t>(pass == 0 ? 0.25f : 0.75f));
        for (std::size_t draw = 0; draw < 128; ++draw) words.insert(words.end(), {0xc0012d00u, 3u, 2u});
        program(0x20c, Address(SampleCode.data()));
        for (const auto offset : {0x207u, 0x208u, 0x209u}) RegisterWord(words, 0x76, offset, offset == 0x207 ? 32 : 1);
        RegisterWord(words, 0x76, 0x212, 0);
        RegisterWord(words, 0x76, 0x213, 16u << 1u);
        const auto output = Address(Output.data() + pass * 32);
        const auto color = Address(Color.data());
        const std::array<std::uint32_t, 16> data{
            static_cast<std::uint32_t>(output), static_cast<std::uint32_t>(output >> 32u), 128, 0x31016facu,
            static_cast<std::uint32_t>(color >> 8u), static_cast<std::uint32_t>(color >> 40u) | (22u << 20u) | (3u << 30u),
            (63u >> 2u) | (3u << 14u), 0x90000facu, 0u, 0u, 0u, 0u, 0x92u, 0u, 0u, 0u
        };
        for (std::uint32_t index = 0; index < data.size(); ++index) RegisterWord(words, 0x76, 0x240 + index, data[index]);
        words.insert(words.end(), {0xc0031500u, 1u, 1u, 1u, 0x8041u});
    }
    return words;
}

}

int main() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        Register(VertexHeader, VertexCode, 2);
        Register(PixelHeader, PixelCode, 1);
        Output.fill(0xdeadbeefu);
        auto words = Frame();
        AgcDriver::DriverDetail::Lanes::SubmissionCursor cursor;
        cursor.submission.commands = words;
        const auto boundary = cursor.End();
        Require(((words.at(boundary - 3) >> 8u) & 0xffu) == 0x2du && boundary < words.size(), "render/sample fixture did not force a draw chunk boundary");
        Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "render/sample PM4 submit failed");
        AgcDriverWaitIdle_nid_postfix();
        for (std::size_t index = 0; index < Output.size(); ++index) Require(Output[index] == std::bit_cast<std::uint32_t>(index < 32 ? 0.25f : 0.75f), "a sampled render target was stale across chunks at lane " + std::to_string(index));
        AgcDriverShutdown_nid_postfix();
        std::cout << "GPU lane render-target sampling across chunks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
