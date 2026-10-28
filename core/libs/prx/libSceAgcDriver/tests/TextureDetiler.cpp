#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include <cstring>
#include <bit>
#include <string>
#include <string_view>

namespace {

using namespace AgcDriver::Graphics;

struct Push {
    std::uint32_t srcBase;
    std::uint32_t dstBase;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitchBytes;
    std::uint32_t blocksPerRow;
    std::uint32_t tail;
    std::uint32_t tailX;
    std::uint32_t tailY;
    std::uint32_t elementBytes;
    std::uint32_t slice;
    std::uint32_t rangeBegin;
    std::uint32_t rangeEnd;
    std::uint32_t tiledBase;
    std::uint32_t linearBase;
    std::uint32_t columnBegin;
    std::uint32_t rowBegin;
    std::uint32_t pipeBankXor;
};

Push decodePush(const std::vector<std::byte>& bytes) {
    Push push{};
    Require(bytes.size() == sizeof(Push), "unexpected texture detiling push constant size");
    std::memcpy(&push, bytes.data(), sizeof(Push));
    return push;
}

TileMipLayout makeLayout(std::uint32_t width, std::uint32_t height, std::uint32_t pitchBytes, std::uint32_t blocksPerRow, std::uint64_t tiledSize, std::uint64_t linearSize) {
    TileMipLayout layout{};
    layout.width = width;
    layout.height = height;
    layout.pitchBytes = pitchBytes;
    layout.blocksPerRow = blocksPerRow;
    layout.tiledSize = tiledSize;
    layout.linearSize = linearSize;
    layout.tail = false;
    layout.tailX = 0;
    layout.tailY = 0;
    return layout;
}

template<typename TAction>
void reject(TAction action, std::string_view reason) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        Require(std::string_view(error.what()).find(reason) != std::string_view::npos, std::string("unexpected texture detiler test error: ") + error.what());
        return;
    }
    throw std::runtime_error(std::string("expected texture detiler rejection: ") + std::string(reason));
}

}

void RunTextureDetilerTests(const Context& context, const TextureDetilerTestAccess& access) {
    for (const auto bytes : {1u, 2u, 4u, 8u, 16u}) {
        const auto equations = TextureDetiler::AliasEquations(bytes);
        Require(equations.has_value(), "R_X alias equations are not invertible");
        const auto block = ThinBlockLayout(TextureTileMode::kR64KBX, bytes);
        const auto byteBits = std::countr_zero(bytes);
        const auto xBits = std::countr_zero(block[1]);
        for (std::uint32_t bit = 0; bit < 16; ++bit) {
            const auto input = 1u << bit;
            const auto x = (input >> byteBits) & (block[1] - 1u);
            const auto y = (input >> (byteBits + xBits)) & (block[2] - 1u);
            std::uint32_t address = input & (bytes - 1u);
            for (std::uint32_t out = 0; out < 16; ++out) {
                const auto mask = (*equations)[out];
                address ^= static_cast<std::uint32_t>(std::popcount((x & (mask & 0xfffu)) ^ ((y << 12u) & (mask & 0xfff000u))) & 1) << out;
            }
            std::uint32_t decoded = 0;
            for (std::uint32_t out = 0; out < 16; ++out) decoded |= static_cast<std::uint32_t>(std::popcount(address & (*equations)[16 + out]) & 1) << out;
            Require(decoded == input, "R_X alias inverse does not recover an address basis vector");
        }
    }
    Require(!TextureDetiler::AliasEquations(3).has_value() && !TextureDetiler::AliasEquations(32).has_value(), "invalid alias element sizes were accepted");
    {
        GuestTextureResource source{};
        source.dimension = TextureDimension::k2DArray;
        source.tileMode = TextureTileMode::kR64KBX;
        source.format = 71;
        source.width = 134;
        source.height = 75;
        source.depthOrLastArray = 71;
        source.mipCount = source.allocatedMipCount = 1;
        source.samples = 1;
        auto target = source;
        target.dimension = TextureDimension::k2D;
        target.depthOrLastArray = 0;
        target.width = 800;
        target.height = 450;
        for (const auto offset : {6u, 62u, 118u}) {
            target.baseAddress = std::uint64_t{offset} * 65536;
            std::array<std::uint64_t, 4> totals{};
            for (const auto unit : {6u, 13u, 20u, 27u, 34u, 41u, 48u, 49u, 50u, 51u, 52u, 53u, 54u, 55u}) {
                const auto coverage = TextureDetiler::ClassifyAliasUnit(source, target, unit);
                Require(coverage.consumer + coverage.sourceOnly + coverage.padding == 65536 && coverage.shared <= coverage.consumer, "edge coverage is not a byte partition");
                totals[0] += coverage.consumer;
                totals[1] += coverage.sourceOnly;
                totals[2] += coverage.padding;
                totals[3] += coverage.shared;
            }
            Require(totals == (offset == 62 ? std::array<std::uint64_t, 4>{127488, 261904, 528112, 45360} : std::array<std::uint64_t, 4>{127488, 263952, 526064, 43312}), "R_X family edge bytes lost their layer XOR");
        }
        target.baseAddress = 6 * 65536;
        target.pipeBankXor = 2048;
        const auto shifted = TextureDetiler::ClassifyAliasUnit(source, target, 6);
        Require(shifted.consumer == 16384 && shifted.consumer + shifted.sourceOnly + shifted.padding == 65536, "pipe XOR changed the consumer extent");
        const auto* equation = FindTextureSwizzleEquation(27, 8);
        const auto encode = [&](std::uint32_t x, std::uint32_t y, std::uint32_t layer) {
            const auto input = x | (y << 12u) | (layer << 24u);
            std::uint32_t address = 0;
            for (std::uint32_t bit = 0; bit < 16; ++bit) address |= (std::popcount(input & equation->bits[bit]) & 1u) << bit;
            return address;
        };
        std::array<bool, 8192> consumer{}, valid{};
        for (std::uint32_t y = 64; y < 128; ++y) for (std::uint32_t x = 768; x < 800; ++x) consumer[(encode(x, y, 0) ^ target.pipeBankXor) / 8] = true;
        for (std::uint32_t y = 64; y < 75; ++y) for (std::uint32_t x = 128; x < 134; ++x) valid[(encode(x, y, 4) ^ source.pipeBankXor) / 8] = true;
        TextureDetiler::AliasCoverage forward;
        for (std::size_t index = 0; index < consumer.size(); ++index) {
            if (consumer[index]) forward.consumer += 8;
            else if (valid[index]) forward.sourceOnly += 8;
            else forward.padding += 8;
            if (consumer[index] && valid[index]) forward.shared += 8;
        }
        const auto inverse = TextureDetiler::ClassifyAliasUnit(source, target, 13);
        Require(forward.consumer == inverse.consumer && forward.sourceOnly == inverse.sourceOnly && forward.padding == inverse.padding && forward.shared == inverse.shared, "inverse alias coverage differs from forward-address enumeration");
        reject([&] { TextureDetiler::ClassifyAliasUnit(source, target, 56); }, "outside");
        target.baseAddress += 1;
        reject([&] { TextureDetiler::ClassifyAliasUnit(source, target, 0); }, "aligned");
    }
    TextureDetiler detiler(context);
    const auto commands = reinterpret_cast<VkCommandBuffer>(static_cast<std::uintptr_t>(1));
    const auto source = access.makeBuffer(4096);
    const auto destination = access.makeBuffer(4096);

    const auto layout = makeLayout(20, 12, 96, 5, 64, 48);
    detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 20, destination, 40, layout, 0);
    auto capture = access.lastDispatch();
    Require(capture.groupsX == 3 && capture.groupsY == 2 && capture.groupsZ == 1, "dispatch group counts were computed incorrectly");
    auto push = decodePush(capture.pushConstants);
    Require(push.srcBase == 4 && push.dstBase == 8, "push constant buffer bases must account for storage buffer offset alignment");
    Require(push.width == 20 && push.height == 12, "push constant dimensions changed");
    Require(push.pitchBytes == 96 && push.blocksPerRow == 5, "push constant row layout changed");
    Require(push.tail == 0 && push.tailX == 0 && push.tailY == 0, "push constant tail fields must reflect a non-tail mip");
    Require(push.elementBytes == 4, "push constant element size changed");
    Require(push.rangeBegin == 0 && push.rangeEnd == 0xffffffffu && push.tiledBase == 0 && push.linearBase == 0, "a whole-mip dispatch must move every element out of whole buffers");
    Require(push.columnBegin == 0 && push.rowBegin == 0, "a whole-mip dispatch starts at the mip's origin");
    Require(capture.sourceBuffer == source && capture.destinationBuffer == destination, "dispatch bound the wrong source or destination buffer");
    Require(capture.sourceOffset == 16 && capture.sourceRange == 68, "source descriptor offset or range computed incorrectly");
    Require(capture.destinationOffset == 32 && capture.destinationRange == 56, "destination descriptor offset or range computed incorrectly");

    const auto pipelinesAfterFirst = access.pipelineCount();
    Require(pipelinesAfterFirst == 1, "the first dispatch must create exactly one compute pipeline");

    // A window inside the mip: tiled bytes [16, 48) of a mip whose tiled buffer starts at the
    // mip's byte 16 and whose linear buffer holds one 96-byte row from linear byte 96; the grid
    // covers only the window's rectangle (columns 8-16 of row 1: one workgroup) and the buffer
    // ranges only the window's bytes, in both directions.
    {
        DetileWindow window{16, 48, 16, 96, 96};
        window.columnBegin = 8;
        window.columnEnd = 16;
        window.rowBegin = 1;
        window.rowEnd = 2;
        detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 20, destination, 40, layout, false, 0, false, window);
        capture = access.lastDispatch();
        Require(capture.groupsX == 1 && capture.groupsY == 1 && capture.groupsZ == 1, "a windowed detile must dispatch only the window's workgroups");
        push = decodePush(capture.pushConstants);
        Require(push.rangeBegin == 16 && push.rangeEnd == 48 && push.tiledBase == 16 && push.linearBase == 96, "windowed detile push constants must carry the window");
        Require(push.columnBegin == 8 && push.rowBegin == 1, "windowed detile push constants must carry the grid origin");
        Require(push.width == 20 && push.height == 12, "a window must not change the mip's dimensions");
        Require(capture.sourceOffset == 16 && capture.sourceRange == 4 + 32, "a windowed detile reads the window's tiled bytes only");
        Require(capture.destinationOffset == 32 && capture.destinationRange == 8 + 96, "a windowed detile writes the window's linear rows only");

        detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 20, destination, 40, layout, true, 0, false, window);
        capture = access.lastDispatch();
        Require(capture.groupsX == 1 && capture.groupsY == 1 && capture.groupsZ == 1, "a windowed retile must dispatch only the window's workgroups");
        push = decodePush(capture.pushConstants);
        Require(push.rangeBegin == 16 && push.rangeEnd == 48 && push.tiledBase == 16 && push.linearBase == 96, "windowed retile push constants must carry the window");
        Require(push.columnBegin == 8 && push.rowBegin == 1, "windowed retile push constants must carry the grid origin");
        Require(capture.sourceOffset == 16 && capture.sourceRange == 4 + 96, "a windowed retile reads the window's linear rows only");
        Require(capture.destinationOffset == 32 && capture.destinationRange == 8 + 32, "a windowed retile writes the window's tiled bytes only");
        Require(access.pipelineCount() == pipelinesAfterFirst + 1, "a retile pipeline is separate from the detile pipeline");

        // The unit-shadow form of the same window (StorageTexture::uploadWindows): the tiled bytes
        // come from a sub-buffer holding the window from its first byte (a slab at offset 1000),
        // so the tiled base is the range's begin and the descriptor covers the window's bytes
        // only; the push constants and the grid are the import form's.
        detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 1000, destination, 40, layout, false, 0, false, window);
        capture = access.lastDispatch();
        Require(capture.groupsX == 1 && capture.groupsY == 1 && capture.groupsZ == 1, "a slab-sourced detile must dispatch only the window's workgroups");
        push = decodePush(capture.pushConstants);
        Require(push.rangeBegin == 16 && push.rangeEnd == 48 && push.tiledBase == 16 && push.linearBase == 96, "slab-sourced detile push constants must carry the window");
        Require(push.columnBegin == 8 && push.rowBegin == 1, "slab-sourced detile push constants must carry the grid origin");
        Require(push.srcBase == 8 && capture.sourceOffset == 992 && capture.sourceRange == 8 + 32, "a slab-sourced detile reads the window's tiled bytes at the slab offset");
        Require(capture.destinationOffset == 32 && capture.destinationRange == 8 + 96, "a slab-sourced detile writes the same linear rows as the import form");

        // Whole block rows: the columns default to the mip's width, the rows to the window's.
        DetileWindow rows{0, 64, 0, 0, 48};
        rows.rowBegin = 8;
        detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 0, destination, 0, layout, false, 0, false, rows);
        capture = access.lastDispatch();
        Require(capture.groupsX == 3 && capture.groupsY == 1, "a row window must dispatch whole rows from its first row");
        reject([&] {
            DetileWindow outside{0, 64, 0, 0, 0};
            outside.rowBegin = 12;
            detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 0, destination, 0, layout, false, 0, false, outside);
        }, "window lies outside the mip");
    }
    const auto pipelinesAfterWindow = access.pipelineCount();

    detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 0, destination, 0, makeLayout(8, 8, 32, 2, 32, 32), 0);
    Require(access.pipelineCount() == pipelinesAfterWindow, "dispatching with the same tile mode and element size must reuse the cached pipeline");

    detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 8, source, 0, destination, 0, makeLayout(8, 8, 32, 2, 32, 32), 0);
    Require(access.pipelineCount() == pipelinesAfterWindow + 1, "a different element size must create a new compute pipeline");

    detiler.Dispatch(commands, TextureTileMode::kLinear, 4, source, 0, destination, 0, makeLayout(8, 8, 32, 2, 32, 32), 0);
    Require(access.pipelineCount() == pipelinesAfterWindow + 2, "a different tile mode must create a new compute pipeline");

    detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 0, destination, 0, makeLayout(8, 8, 32, 2, 32, 32), 0);
    Require(access.pipelineCount() == pipelinesAfterWindow + 2, "reusing an earlier tile mode and element size must not create another pipeline");

    detiler.Dispatch(commands, TextureTileMode::RenderTarget64KB, 4, source, 0, destination, 0, layout, false, 13);
    capture = access.lastDispatch();
    Require(decodePush(capture.pushConstants).slice == 13, "render target detiling must preserve the absolute array layer for XOR addressing");
    Require(access.pipelineCount() == pipelinesAfterWindow + 3, "render target detiling must use a separate pipeline");
    const auto specialization = access.lastSpecialization();
    Require(specialization[0] == 4 && specialization[1] == 65536 && specialization[2] == 2, "render target detiling must select its own swizzle family");

    detiler.Dispatch(commands, TextureTileMode::kD4KBX, 4, source, 0, destination, 0, layout, false, 0);
    const auto equationSpecialization = access.lastSpecialization();
    Require(decodePush(access.lastDispatch().pushConstants).pipeBankXor == 0u, "a dispatch without a window passed a pipe/bank XOR");
    detiler.Dispatch(commands, TextureTileMode::kD4KBX, 4, source, 0, destination, 0, layout, false, 0, false, {.pipeBankXor = 0xa00u});
    Require(decodePush(access.lastDispatch().pushConstants).pipeBankXor == 0xa00u, "the window's pipe/bank XOR did not reach the detiling shader");
    Require(equationSpecialization[0] == 4 && equationSpecialization[1] == 4096 && equationSpecialization[2] == 2, "SW_4KB_D_X detiling must select the equation family over 4 KiB blocks");

    {
        const auto mip = ComputeColorTargetMipLayout(ColorTileMode::RenderTarget, 4, 1024, 1024, 11, 4);
        Require(mip.width == 64 && mip.height == 64 && mip.tail, "the first packed render-target mip has the wrong extent");
        Require(mip.tiledOffset == 0 && mip.tiledSize == 65536 && mip.tailX == 64 && mip.tailY == 0, "the first packed render-target mip lost its shared-block origin");
        Require(mip.pitchBytes == 256 && mip.linearSize == 16384, "packed render-target mips must use tightly packed transfer rows");
        const auto next = ComputeColorTargetMipLayout(ColorTileMode::RenderTarget, 4, 1024, 1024, 11, 5);
        Require(next.width == 32 && next.height == 32 && next.tail && next.tiledOffset == 0 && next.tiledSize == 65536 && next.tailX == 0 && next.tailY == 64, "the next packed render-target mip lost its shared-block origin");
        const auto regular = ComputeColorTargetMipLayout(ColorTileMode::RenderTarget, 4, 1024, 1024, 11, 3);
        Require(!regular.tail && regular.width == 128 && regular.height == 128 && regular.tiledOffset == 0 && regular.tiledSize == 65536, "a regular render-target mip must be relative to its own target buffer");
        reject([] { ComputeColorTargetMipLayout(ColorTileMode::RenderTarget, 4, 1024, 1024, 11, 11); }, "outside its surface");
    }

    {
        const auto pools = access.descriptorPoolCount();
        const auto resets = access.descriptorPoolResets();
        for (int dispatch = 0; dispatch < 3 * 64; ++dispatch) detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 0, destination, 0, makeLayout(8, 8, 32, 2, 32, 32), 0);
        Require(access.descriptorPoolResets() == resets, "a descriptor pool was reset with no recorder to tell that its sets were no longer recorded");
        Require(access.descriptorPoolCount() >= pools + 2, "full descriptor pools with no recorder must be joined by new pools");
    }

    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(context);
        recorder.Activate();
        std::size_t pools = 0;
        for (int batch = 0; batch < 4; ++batch) {
            const auto batchCommands = recorder.Commands();
            for (int dispatch = 0; dispatch < 64; ++dispatch) detiler.Dispatch(batchCommands, TextureTileMode::kStandard4KB, 4, source, 0, destination, 0, makeLayout(8, 8, 32, 2, 32, 32), 0);
            recorder.Sync();
            if (batch == 0) pools = access.descriptorPoolCount();
        }
        Require(access.descriptorPoolCount() == pools, "a full descriptor pool whose batches completed must be reset and reused, not joined by a new pool");
    }

    reject([&] { detiler.Dispatch(VK_NULL_HANDLE, TextureTileMode::kStandard4KB, 4, source, 0, destination, 0, layout, 0); }, "active command buffer");
    reject([&] { detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, VK_NULL_HANDLE, 0, destination, 0, layout, 0); }, "source and destination buffers");
    reject([&] { detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 0, VK_NULL_HANDLE, 0, layout, 0); }, "source and destination buffers");
    reject([&] { detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 0, destination, 0, makeLayout(0, 12, 96, 5, 64, 48), 0); }, "non-empty mip layout");
    reject([&] { detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 0, destination, 0, makeLayout(20, 0, 96, 5, 64, 48), 0); }, "non-empty mip layout");
    reject([&] { detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 0, destination, 0, makeLayout(20, 12, 96, 5, 0, 48), 0); }, "non-empty mip layout");
    reject([&] { detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 0, destination, 0, makeLayout(20, 12, 96, 5, 64, 0), 0); }, "non-empty mip layout");
    reject([&] { detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 3, source, 0, destination, 0, layout, 0); }, "unsupported element size");
    reject([&] { detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 20, destination, 40, makeLayout(20, 12, 96, 5, 1024, 48), 0); }, "buffer range exceeds device limits");
    reject([&] { detiler.Dispatch(commands, TextureTileMode::kStandard4KB, 4, source, 0, destination, 0, layout, false, 0, false, DetileWindow{48, 48, 0, 0, 0}); }, "window is empty");
}
