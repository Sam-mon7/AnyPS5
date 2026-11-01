#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWWRITERANGES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWWRITERANGES_HPP

#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

// `narrow` (APS5_ARCHB_PIPE_NARROW_DRAW_WRITES, draws only): a program on the BDA ABI whose build has
// no BDA writes is bounded by its written bindings, and a single-sample, single-slice depth target with
// HTILE by its planes and HTILE metadata, instead of making the write set unbounded.
std::vector<std::pair<std::uint64_t, std::uint64_t>> DrawWriteRanges(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages, bool exact, bool narrow = false);
std::vector<std::pair<std::uint64_t, std::uint64_t>> DispatchWriteRanges(const ShaderRecompiler::RecompileResult& compiled, bool& bounded, bool readOnlyBda = false);
// Whether a write set is the unbounded range (0, max).
bool UnboundedWrites(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges);
bool ExactDrawWrites();
std::vector<std::pair<std::uint64_t, std::uint64_t>> QueuedDrawReadRanges(const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> stages);

// Diagnostics (APS5_PIPE_WRITE_DIAG=1, replay only): which part of a queued draw's write set a range
// came from, to see why indirect arguments overlap queued draws' writes.
enum class DrawWriteKind : std::uint8_t { BufferWritten, BufferAssumed, StorageImage, StorageImageDcc, Color, ColorDcc, ColorCmask, Depth, Stencil, Count };
struct DrawWriteTag {
    DrawWriteKind kind = DrawWriteKind::BufferWritten;
    std::uint8_t stage = 0;     // ShaderStage, 0xff for targets
    std::uint32_t element = 0;  // binding element or color slot
    std::uint32_t a = 0;        // buffer stride or target width
    std::uint32_t b = 0;        // buffer records or target height
    std::uint64_t variant = 0;
};
bool PipeWriteDiag();
const char* DrawWriteKindName(DrawWriteKind kind);
void NoteDrawWriteTags(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages);
// `changed`: whether the arguments read after the drain differ from those read before it (if read).
void NoteIndirectWriterOverlap(std::uint64_t arguments, std::uint64_t argumentBytes, std::pair<std::uint64_t, std::uint64_t> writer, std::optional<bool> changed);
std::string DrawWriteDiagText();

}

#endif
