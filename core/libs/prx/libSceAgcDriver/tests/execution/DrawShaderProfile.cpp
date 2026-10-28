#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

alignas(256) constexpr std::array<std::uint32_t, 8> VertexCode{0x7e000208u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> OtherPixelCode{0x7e000280u, 0x7e020200u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) std::array<std::byte, 1024> ColorMemory{};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};
alignas(8) Header OtherPixelHeader{};

void Register(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

std::uint64_t Address(const void* pointer) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

void Draw(const std::uint32_t* pixelCode, std::size_t draws) {
    const auto color = reinterpret_cast<std::uintptr_t>(ColorMemory.data());
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(pixelCode);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 1u << 1u}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 1u << 1u},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    for (std::size_t draw = 0; draw < draws; ++draw) {
        Append(commands, 0x76, {0x00c, std::bit_cast<std::uint32_t>(1.0f)});
        Append(commands, 0x2d, {3, 2});
    }
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

std::vector<AgcDriver::DrawShaderProfileRow> Profile() {
    std::vector<AgcDriver::DrawShaderProfileRow> rows(AgcDriverDrawShaderProfile_nid_postfix(nullptr, 0));
    rows.resize(std::min(rows.size(), AgcDriverDrawShaderProfile_nid_postfix(rows.data(), rows.size())));
    return rows;
}

const AgcDriver::DrawShaderProfileRow* Find(const std::vector<AgcDriver::DrawShaderProfileRow>& rows, const std::uint32_t* pixelCode) {
    for (const auto& row : rows) {
        const auto codes = std::span(row.code, row.programs);
        const bool vertex = std::find(codes.begin(), codes.end(), Address(VertexCode.data())) != codes.end();
        const bool pixel = std::find(codes.begin(), codes.end(), Address(pixelCode)) != codes.end();
        if (row.programs == 2 && vertex && pixel) return &row;
    }
    return nullptr;
}

void ProgramSetsGetTheirOwnRows(bool pipelined) {
    AgcDriverResetDrawShaderProfile_nid_postfix();
    Require(Profile().empty(), "the reset profile still holds rows");
    Draw(PixelCode.data(), 3);
    Draw(OtherPixelCode.data(), 2);
    const auto rows = Profile();
    const auto* first = Find(rows, PixelCode.data());
    const auto* second = Find(rows, OtherPixelCode.data());
    Require(first != nullptr && second != nullptr, "the profile has " + std::to_string(rows.size()) + " rows, without one for each vertex and pixel program pair drawn");
    Require(first->draws == 3 && first->drawn == 3, "the first program pair counted " + std::to_string(first->draws) + " packets and " + std::to_string(first->drawn) + " drawn, expected 3 and 3");
    Require(second->draws == 2 && second->drawn == 2, "the second program pair counted " + std::to_string(second->draws) + " packets and " + std::to_string(second->drawn) + " drawn, expected 2 and 2");
    for (const auto* row : {first, second}) {
        Require(std::accumulate(std::begin(row->ms), std::end(row->ms), 0.0) > 0.0, "a profiled program pair recorded no worker time");
        if (pipelined) Require(row->commits == row->drawn && row->commitMs > 0.0, "a pipelined program pair counted " + std::to_string(row->commits) + " commits for " + std::to_string(row->drawn) + " drawn packets, or no commit time");
        else Require(row->commits == 0 && row->commitMs == 0.0, "an inline program pair counted " + std::to_string(row->commits) + " committer commits");
        const auto stages = std::span(row->stage, row->programs);
        Require(std::find(stages.begin(), stages.end(), 1) != stages.end() && std::find(stages.begin(), stages.end(), 5) != stages.end(), "a program pair did not record its vertex and fragment stages");
    }
    AgcDriverResetDrawShaderProfile_nid_postfix();
    Require(Profile().empty(), "the reset did not clear the profile");
}

void SwitchOffRecordsNothing() {
    Draw(PixelCode.data(), 2);
    Require(Profile().empty(), "without APS5_PROFILE_DRAW_SHADERS the driver kept per-program rows");
}

}

int main(int argc, char** argv) {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        Register(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
        Register(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
        Register(OtherPixelHeader, OtherPixelCode.data(), sizeof(OtherPixelCode), 1);
        const std::string_view mode = argc > 1 ? argv[1] : "";
        if (mode == "--off") SwitchOffRecordsNothing();
        else ProgramSetsGetTheirOwnRows(mode == "--pipelined");
        AgcDriverShutdown_nid_postfix();
        std::cout << "draw shader profile test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
