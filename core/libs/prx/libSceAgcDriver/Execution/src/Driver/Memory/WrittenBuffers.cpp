#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DrawWriteRanges.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

namespace {

template<typename TVisit>
void visitWrittenBuffers(const ShaderRecompiler::RecompileResult& compiled, TVisit&& visit) {
    for (const auto& binding : compiled.bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const bool written = element >= binding.Flags().bufferWritten.size() || binding.Flags().bufferWritten[element];
            if (!written || binding.guestDescriptor.size() < (static_cast<std::size_t>(element) + 1) * 4) continue;
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 4, 4);
            const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1], words[2], words[3]}};
            const auto base = descriptor.Base48();
            const auto size = descriptor.GetSize();
            if (base == 0 || size == 0) continue;
            visit(element, base, base + size, element < binding.Flags().bufferAtomic.size() && binding.Flags().bufferAtomic[element]);
        }
    }
}

}

template<typename TVisit>
void Driver::forEachWrittenBuffer(const ShaderRecompiler::RecompileResult& compiled, TVisit&& visit) {
    visitWrittenBuffers(compiled, std::forward<TVisit>(visit));
}

void Driver::appendWrittenRanges(const ShaderRecompiler::RecompileResult& compiled, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) {
    forEachWrittenBuffer(compiled, [&](std::uint32_t, std::uint64_t begin, std::uint64_t end, bool) { ranges.emplace_back(begin, end); });
}

void Driver::noteWrittenBuffers(std::uint64_t program, std::uint32_t queue, const ShaderRecompiler::RecompileResult& compiled) {
    std::lock_guard lock(writtenBuffersMutex);
    const auto serial = ++writtenBufferSerial;
    forEachWrittenBuffer(compiled, [&](std::uint32_t, std::uint64_t begin, std::uint64_t end, bool atomic) {
        writtenBuffers.push_back({program, begin, end, serial, queue, atomic});
    });
    while (writtenBuffers.size() > WrittenBufferRing) writtenBuffers.pop_front();
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> DispatchWriteRanges(const ShaderRecompiler::RecompileResult& compiled, bool& bounded, bool readOnlyBda) {
    // A program on the BDA ABI accesses guest memory by address through its page table, so its writes
    // are unbounded, unless (readOnlyBda) its build has no BDA writes: the recompiler sets bdaWrites
    // for every address store or atomic and every buffer store or atomic it could not bind to a V#,
    // and the SPIR-V emitter defines the BDA write and atomic lookups only when it is set (EmitBdaWrite
    // and the atomic emitters fail without them). Such a program reads through the page table and
    // writes only its fault buffer, which is driver memory (BdaResources), not guest memory, so its
    // guest writes are its written bindings, listed below as for a program without the BDA ABI.
    const bool addressWrites = !readOnlyBda || compiled.bdaWrites;
    bounded = compiled.bdaAbiVersion == 0 || !addressWrites;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    const auto add = [&](std::uint64_t address, std::uint64_t bytes) {
        if (address == 0) return;
        if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) bounded = false;
        else ranges.emplace_back(address, address + bytes);
    };
    for (const auto& binding : compiled.bindings) {
        if (binding.role == ShaderRecompiler::DescriptorRole::Gds) bounded = false;
        if ((binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable || binding.role == ShaderRecompiler::DescriptorRole::FaultBuffer) && addressWrites) bounded = false;
        if (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
            static const bool allWritten = std::getenv("APS5_ALL_BUFFERS_WRITTEN") != nullptr;
            if (allWritten) bounded = false;
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                if (element < binding.Flags().bufferWritten.size() && !binding.Flags().bufferWritten[element]) continue;
                if (binding.guestDescriptor.size() < (static_cast<std::size_t>(element) + 1) * 4 || (binding.guestDescriptor[static_cast<std::size_t>(element) * 4 + 3] >> 30u) != 0) bounded = false;
            }
        }
        if (binding.role != ShaderRecompiler::DescriptorRole::ImageStorageTable && binding.role != ShaderRecompiler::DescriptorRole::ImageTable && (binding.role != ShaderRecompiler::DescriptorRole::GuestImages || binding.kind != ShaderRecompiler::DescriptorKind::StorageImage)) continue;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const bool written = element >= binding.Flags().imageWritten.size() || binding.Flags().imageWritten[element];
            if (!written) continue;
            if (binding.guestDescriptor.size() < (static_cast<std::size_t>(element) + 1) * 8) {
                bounded = false;
                continue;
            }
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8, 8);
            if (Graphics::IsNullTextureResource(words)) continue;
            try {
                const auto resource = Graphics::DecodeTextureResource(words);
                const auto surfaceBytes = Graphics::SurfaceGuestBytes(resource);
                if (surfaceBytes == 0) bounded = false;
                add(resource.baseAddress, surfaceBytes);
                if (resource.dccAddress != 0 && surfaceBytes != 0) add(resource.dccAddress, Graphics::DccKeyCount(resource, surfaceBytes));
            } catch (const std::runtime_error&) {
                bounded = false;
            }
        }
    }
    visitWrittenBuffers(compiled, [&](std::uint32_t, std::uint64_t begin, std::uint64_t end, bool) { add(begin, end - begin); });
    return ranges;
}

void Driver::noteForeignWriter(std::uint64_t begin, std::uint64_t end, std::uint32_t queue) {
    if (!foreignWriters() || !(writeEvidenceEnabled() || traceCapSync()) || end <= begin) return;
    std::lock_guard lock(writtenBuffersMutex);
    writtenBuffers.push_back({0, begin, end, ++writtenBufferSerial, queue, false});
    while (writtenBuffers.size() > WrittenBufferRing) writtenBuffers.pop_front();
}

void Driver::noteDrawWriters(std::span<const Graphics::CompiledShader> stages, std::uint32_t queue) {
    for (const auto& stage : stages) {
        forEachWrittenBuffer(*stage.program, [&](std::uint32_t, std::uint64_t begin, std::uint64_t end, bool) { noteForeignWriter(begin, end, queue); });
    }
}

bool ExactDrawWrites() {
    static const bool exact = [] { const char* text = std::getenv("APS5_EXACT_DRAW_WRITES"); return text == nullptr || std::strcmp(text, "0") != 0; }();
    return exact;
}

bool UnboundedWrites(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) {
    return ranges.size() == 1 && ranges.front().first == 0 && ranges.front().second == std::numeric_limits<std::uint64_t>::max();
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> DrawWriteRanges(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages, bool exact, bool narrow, bool slicedDepth) {
    constexpr std::uint64_t Slack = 64 * 1024;
    constexpr std::pair<std::uint64_t, std::uint64_t> Unbounded{0, std::numeric_limits<std::uint64_t>::max()};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    bool bounded = true;
    const auto add = [&](std::uint64_t begin, std::uint64_t bytes) {
        if (begin == 0) return;
        if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - begin) bounded = false;
        else ranges.emplace_back(begin, begin + bytes);
    };
    for (const auto& stage : stages) {
        if (stage.program == nullptr) return {Unbounded};
        auto writes = DispatchWriteRanges(*stage.program, bounded, narrow);
        if (!bounded) return {Unbounded};
        ranges.insert(ranges.end(), writes.begin(), writes.end());
    }
    const auto addColor = [&](const Graphics::ColorTarget& color) {
        if (color.address == 0 && color.dccAddress == 0 && color.cmaskAddress == 0) return;
        if (color.bytes == 0 || color.extent.width == 0 || color.extent.height == 0 || color.extent.width > 16384 || color.extent.height > 16384 || color.samples != 1 || color.depth != 1 || color.layers != 1 || color.arrayAddress != 0 || color.mipCount != 1 || color.mip != 0 || color.depthSlice != 0 || color.arraySlice != 0 || color.arrayLastSlice != 0) {
            bounded = false;
            return;
        }
        try {
            if (color.bytes < Graphics::ColorTargetLayout(color.extent.width, color.extent.height, color.tileMode, color.elementBytes).Bytes()) {
                bounded = false;
                return;
            }
        } catch (const std::runtime_error&) {
            bounded = false;
            return;
        }
        const auto bytes = static_cast<std::uint64_t>(color.bytes);
        const auto keys = color.dccPipeAligned ? Graphics::DccKeyCount(Graphics::ColorTextureTileMode(color.tileMode), color.elementBytes, color.extent.width, color.extent.height, bytes) : Graphics::DccKeyBytes(bytes);
        if (exact) {
            add(color.address, bytes);
            add(color.dccAddress, keys);
            add(color.cmaskAddress, color.cmaskBytes);
            return;
        }
        const auto base = color.surfaceAddress != 0 ? std::min(color.surfaceAddress, color.address) : color.address;
        if (color.address > std::numeric_limits<std::uint64_t>::max() - Slack || bytes > (std::numeric_limits<std::uint64_t>::max() - color.address - Slack) / 2) bounded = false;
        else add(base, color.address - base + 2 * bytes + Slack);
        add(color.dccAddress, std::max<std::uint64_t>(keys, bytes / 16 + Slack));
        add(color.cmaskAddress, color.cmaskBytes);
    };
    if (graphics.hasColorTarget) addColor(graphics.color);
    for (const auto& color : graphics.colors) addColor(color);
    if (graphics.depth) {
        const auto& depth = *graphics.depth;
        // `narrow` (APS5_ARCHB_PIPE_NARROW_DRAW_WRITES, exact only): a single-sample, single-slice depth
        // target with HTILE writes its planes and its HTILE metadata, the HtileMetadataBytes(extent)
        // at htileAddress the driver reads and stores for it (DepthSurface.cpp) and nothing else.
        const bool htileBounded = narrow && exact && depth.htileAddress != 0;
        // `slicedDepth` (APS5_ARCHB_PIPE_NARROW_SLICED_DEPTH, exact only): a single-sample depth target
        // whose view names slices (DB_DEPTH_VIEW SLICE_START/SLICE_MAX) is written in one slice only, the
        // one decodeDepthPlane already moved address, stencilAddress and htileAddress to: the depth
        // surface is keyed and sized without the slice fields (DepthSurface sameSurface, DepthSliceBytes,
        // HtileMetadataBytes), its image has one layer, layered rendering with a depth target is refused
        // (State.cpp, renderLayers) and layer exports are ignored. So its writes are the ranges below, as
        // for a slice-0 view.
        const bool slicesBounded = slicedDepth && exact;
        if (depth.extent.width == 0 || depth.extent.height == 0 || depth.extent.width > 16384 || depth.extent.height > 16384 || depth.samples != 1 || ((depth.firstSlice != 0 || depth.lastSlice != 0) && !slicesBounded) || (depth.htileAddress != 0 && !htileBounded)) return {Unbounded};
        if (exact && depth.extent.width != 0 && depth.extent.height != 0) {
            const bool d16 = depth.format == VK_FORMAT_D16_UNORM || depth.format == VK_FORMAT_D16_UNORM_S8_UINT;
            add(depth.address, Graphics::DepthSliceBytes(depth.extent, d16 ? 2u : 4u));
            add(depth.stencilAddress, Graphics::DepthSliceBytes(depth.extent, 1u));
            if (htileBounded) add(depth.htileAddress, Graphics::HtileMetadataBytes(depth.extent));
        } else {
            const auto align = [](std::uint64_t value) { return (value + 255) & ~255ull; };
            const auto pixels = align(depth.extent.width) * align(depth.extent.height);
            const auto bytes = pixels * 8 + Slack;
            add(depth.address, bytes);
            add(depth.stencilAddress, bytes);
        }
    }
    if (!bounded) return {Unbounded};
    return ranges;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> Driver::drawWriteRanges(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages, bool narrow, bool slicedDepth) {
    return DrawWriteRanges(graphics, stages, ExactDrawWrites(), narrow, slicedDepth);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> QueuedDrawReadRanges(const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> stages) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    const auto add = [&](std::uint64_t begin, std::uint64_t bytes) {
        if (begin == 0 || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - begin) return;
        ranges.emplace_back(begin, begin + bytes);
    };
    if (draw.indexed && draw.indexSize != 0 && draw.indexCount != 0) add(draw.indexAddress, static_cast<std::uint64_t>(draw.indexCount) * draw.indexSize);
    if (draw.indirect) {
        add(draw.indirect->arguments, std::max<std::uint64_t>(draw.indirect->RangeBytes(), draw.indirect->recordBytes));
        if (draw.indirect->countIndirect) add(draw.indirect->countAddress, 4);
    }
    if (stages.empty()) return ranges;
    for (const auto& stage : stages) {
        if (stage.program == nullptr) continue;
        for (const auto& binding : stage.program->bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers || binding.guestDescriptor.size() < static_cast<std::size_t>(binding.count) * 4u) continue;
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                const auto* words = binding.guestDescriptor.data() + static_cast<std::size_t>(element) * 4u;
                const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1], words[2], words[3]}};
                add(descriptor.Base48(), descriptor.GetSize());
            }
        }
    }
    if (stages.empty() || stages.front().program == nullptr) return ranges;
    for (const auto& attribute : stages.front().program->vertexAttributes) {
        if (Graphics::NullVertexDescriptor(attribute)) continue;
        const auto address = attribute.resource.fields[0] | (static_cast<std::uint64_t>(attribute.resource.fields[1] & 0xffffu) << 32u);
        std::uint64_t bytes = 0;
        try {
            if (Graphics::VertexFetchOutOfRange(attribute)) continue;
            bytes = Graphics::VertexBufferExtent(attribute);
        } catch (const std::runtime_error&) {
            continue;
        }
        add(address, bytes);
    }
    return ranges;
}

std::optional<WrittenBuffer> Driver::newestWriterLocked(std::uint64_t begin, std::uint64_t end) const {
    for (auto it = writtenBuffers.rbegin(); it != writtenBuffers.rend(); ++it) {
        if (begin < it->end && it->begin < end) return *it;
    }
    return std::nullopt;
}

std::optional<WrittenBuffer> Driver::newestWriter(std::uint64_t begin, std::uint64_t end) {
    std::lock_guard lock(writtenBuffersMutex);
    return newestWriterLocked(begin, end);
}

std::string Driver::describeWriters(std::uint64_t begin, std::uint64_t end) {
    std::string text;
    std::lock_guard lock(writtenBuffersMutex);
    int shown = 0;
    for (auto it = writtenBuffers.rbegin(); it != writtenBuffers.rend() && shown < 3; ++it) {
        if (begin >= it->end || it->begin >= end) continue;
        char item[128];
        if (it->program == 0) std::snprintf(item, sizeof(item), " [foreign q0x%x 0x%llx+0x%llx age %llu]", it->queue, static_cast<unsigned long long>(it->begin), static_cast<unsigned long long>(it->end - it->begin), static_cast<unsigned long long>(writtenBufferSerial - it->serial));
        else std::snprintf(item, sizeof(item), " [0x%llx q0x%x 0x%llx+0x%llx%s%s age %llu]", static_cast<unsigned long long>(it->program), it->queue, static_cast<unsigned long long>(it->begin), static_cast<unsigned long long>(it->end - it->begin), it->atomic ? " atomic" : "", it->value ? " known" : "", static_cast<unsigned long long>(writtenBufferSerial - it->serial));
        text += item;
        ++shown;
    }
    if (shown == 0) return " none";
    std::uint32_t streakMin = 0xffffffffu, streakMax = 0, changed = 0, unseen = 0;
    for (std::uint64_t dword = begin & ~3ull; dword < end; dword += 4) {
        const auto found = dwordEvidence.find(dword);
        if (found == dwordEvidence.end()) {
            ++unseen;
            continue;
        }
        streakMin = std::min(streakMin, found->second.streak);
        streakMax = std::max(streakMax, found->second.streak);
        changed += found->second.changed;
    }
    char item[96];
    std::snprintf(item, sizeof(item), " dwords: streak %u..%u changed %u unseen %u", streakMin == 0xffffffffu ? 0u : streakMin, streakMax, changed, unseen);
    return text + item;
}

std::string Driver::describeSelf(const ShaderRecompiler::RecompileResult& compiled, std::uint64_t begin, std::uint64_t end) {
    std::string text;
    forEachWrittenBuffer(compiled, [&](std::uint32_t element, std::uint64_t base, std::uint64_t limit, bool atomic) {
        if (begin >= limit || base >= end) return;
        char item[96];
        std::snprintf(item, sizeof(item), " own element %u 0x%llx+0x%llx%s", element, static_cast<unsigned long long>(base), static_cast<unsigned long long>(limit - base), atomic ? " atomic" : "");
        text += item;
    });
    return text;
}

namespace {

struct WriteDiagState {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::pair<std::uint64_t, std::uint64_t>, DrawWriteTag>>> tags;
    std::size_t entries = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrawWriteKind::Count)> overlaps{};
    std::array<std::uint64_t, static_cast<std::size_t>(DrawWriteKind::Count)> overlapBytes{};
    std::uint64_t untagged = 0;
    std::uint64_t argumentsChanged = 0;
    std::uint64_t argumentsUnchanged = 0;
    std::uint64_t printed = 0;
    std::uint64_t events = 0;
    // Unbounded write sets: why, over enqueued draws and over the overlaps they caused (the newest
    // queued unbounded draw is the most recently enqueued one: FIFO, and it overlaps everything).
    std::map<std::string, std::uint64_t> unboundedDraws;
    std::map<std::string, std::uint64_t> unboundedOverlaps;
    std::string lastUnbounded;
    std::uint64_t boundedDraws = 0;
    std::uint64_t unboundedPrinted = 0;
};

WriteDiagState& writeDiag() {
    static auto* state = new WriteDiagState();
    return *state;
}

constexpr std::array<const char*, static_cast<std::size_t>(DrawWriteKind::Count)> DrawWriteKindNames{"buffer_written", "buffer_assumed", "storage_image", "storage_image_dcc", "color", "color_dcc", "color_cmask", "depth", "stencil"};

std::uint64_t diagKey(std::uint64_t begin, std::uint64_t end) {
    return begin * 0x9e3779b97f4a7c15ull ^ end;
}

// Mirrors DispatchWriteRanges: the first condition that makes one program's write set unbounded
// (empty: bounded unless a range overflows or is empty).
std::string programUnboundedReason(const ShaderRecompiler::RecompileResult& compiled, bool readOnlyBda) {
    if (compiled.bdaAbiVersion != 0 && (!readOnlyBda || compiled.bdaWrites)) return compiled.bdaWrites ? " bda abi, bda writes" : " bda abi, no bda writes";
    for (const auto& binding : compiled.bindings) {
        if (binding.role == ShaderRecompiler::DescriptorRole::Gds) return " gds binding";
        if (binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable && (!readOnlyBda || compiled.bdaWrites)) return " bda pagetable binding";
        if (binding.role == ShaderRecompiler::DescriptorRole::FaultBuffer && (!readOnlyBda || compiled.bdaWrites)) return " fault buffer binding";
        if (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
            if (std::getenv("APS5_ALL_BUFFERS_WRITTEN") != nullptr) return " APS5_ALL_BUFFERS_WRITTEN";
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                if (element < binding.Flags().bufferWritten.size() && !binding.Flags().bufferWritten[element]) continue;
                if (binding.guestDescriptor.size() < (static_cast<std::size_t>(element) + 1) * 4) return " written buffer descriptor missing";
                if ((binding.guestDescriptor[static_cast<std::size_t>(element) * 4 + 3] >> 30u) != 0) return " written buffer type bits";
            }
        }
        if (binding.role != ShaderRecompiler::DescriptorRole::ImageStorageTable && binding.role != ShaderRecompiler::DescriptorRole::ImageTable && (binding.role != ShaderRecompiler::DescriptorRole::GuestImages || binding.kind != ShaderRecompiler::DescriptorKind::StorageImage)) continue;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const bool written = element >= binding.Flags().imageWritten.size() || binding.Flags().imageWritten[element];
            if (!written) continue;
            const std::string what = binding.role == ShaderRecompiler::DescriptorRole::ImageTable ? " image table" : binding.role == ShaderRecompiler::DescriptorRole::ImageStorageTable ? " storage image table" : " storage image";
            const bool flagged = element < binding.Flags().imageWritten.size();
            if (binding.guestDescriptor.size() < (static_cast<std::size_t>(element) + 1) * 8) return what + (flagged ? " (written)" : " (no write flags)") + " descriptor missing";
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8, 8);
            if (Graphics::IsNullTextureResource(words)) continue;
            try {
                const auto resource = Graphics::DecodeTextureResource(words);
                if (Graphics::SurfaceGuestBytes(resource) == 0) return what + " zero bytes";
            } catch (const std::runtime_error&) {
                return what + " undecodable";
            }
        }
    }
    return {};
}

// Mirrors DrawWriteRanges (exact): the first condition that made the write set unbounded.
std::string unboundedReason(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages, bool readOnlyBda = false, bool slicedDepth = false) {
    static const char* stageNames[] = {"cs", "vs", "hs", "ds", "gs", "ps", "ls", "ms"};
    for (const auto& stage : stages) {
        const auto* compiled = stage.program;
        const auto index = static_cast<std::size_t>(stage.stage);
        const std::string name = index < std::size(stageNames) ? stageNames[index] : "stage" + std::to_string(index);
        if (compiled == nullptr) return name + " without program";
        if (auto reason = programUnboundedReason(*compiled, readOnlyBda); !reason.empty()) return name + reason;
    }
    const auto colorReason = [](const Graphics::ColorTarget& color) -> std::string {
        if (color.address == 0 && color.dccAddress == 0 && color.cmaskAddress == 0) return {};
        if (color.bytes == 0) return "color zero bytes";
        if (color.extent.width == 0 || color.extent.height == 0 || color.extent.width > 16384 || color.extent.height > 16384) return "color extent";
        if (color.samples != 1) return "color samples";
        if (color.depth != 1) return "color depth (3D)";
        if (color.layers != 1 || color.arrayAddress != 0 || color.arraySlice != 0 || color.arrayLastSlice != 0) return "color array";
        if (color.mipCount != 1 || color.mip != 0) return "color mips";
        if (color.depthSlice != 0) return "color depth slice";
        try {
            if (color.bytes < Graphics::ColorTargetLayout(color.extent.width, color.extent.height, color.tileMode, color.elementBytes).Bytes()) return "color bytes below layout";
        } catch (const std::runtime_error&) {
            return "color layout undecodable";
        }
        return {};
    };
    if (graphics.hasColorTarget) {
        if (auto reason = colorReason(graphics.color); !reason.empty()) return reason;
    }
    for (const auto& color : graphics.colors) {
        if (auto reason = colorReason(color); !reason.empty()) return reason;
    }
    if (graphics.depth) {
        const auto& depth = *graphics.depth;
        if (depth.extent.width == 0 || depth.extent.height == 0 || depth.extent.width > 16384 || depth.extent.height > 16384) return "depth extent";
        if (depth.samples != 1) return "depth samples";
        if ((depth.firstSlice != 0 || depth.lastSlice != 0) && !slicedDepth) return "depth slices";
        if (depth.htileAddress != 0 && !readOnlyBda) return "depth htile";
    }
    return "write range overflow or zero size";
}

}

bool PipeWriteDiag() {
    static const bool diag = [] { const char* text = std::getenv("APS5_PIPE_WRITE_DIAG"); return text != nullptr && text[0] == '1'; }();
    return diag;
}

const char* DrawWriteKindName(DrawWriteKind kind) {
    return static_cast<std::size_t>(kind) < DrawWriteKindNames.size() ? DrawWriteKindNames[static_cast<std::size_t>(kind)] : "?";
}

void NoteDrawWriteTags(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages) {
    std::vector<std::pair<std::pair<std::uint64_t, std::uint64_t>, DrawWriteTag>> found;
    const auto add = [&](std::uint64_t begin, std::uint64_t bytes, DrawWriteTag tag) {
        if (begin == 0 || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - begin) return;
        found.push_back({{begin, begin + bytes}, tag});
    };
    for (std::size_t s = 0; s < stages.size(); ++s) {
        const auto* compiled = stages[s].program;
        if (compiled == nullptr) continue;
        for (const auto& binding : compiled->bindings) {
            if (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
                for (std::uint32_t element = 0; element < binding.count; ++element) {
                    const bool flagged = element < binding.Flags().bufferWritten.size();
                    if (flagged && !binding.Flags().bufferWritten[element]) continue;
                    if (binding.guestDescriptor.size() < (static_cast<std::size_t>(element) + 1) * 4) continue;
                    const auto* words = binding.guestDescriptor.data() + static_cast<std::size_t>(element) * 4;
                    const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1], words[2], words[3]}};
                    add(descriptor.Base48(), descriptor.GetSize(), {flagged ? DrawWriteKind::BufferWritten : DrawWriteKind::BufferAssumed, static_cast<std::uint8_t>(stages[s].stage), element, descriptor.Stride(), descriptor.NumRecords(), compiled->variantId});
                }
                continue;
            }
            if (binding.role != ShaderRecompiler::DescriptorRole::ImageStorageTable && binding.role != ShaderRecompiler::DescriptorRole::ImageTable && (binding.role != ShaderRecompiler::DescriptorRole::GuestImages || binding.kind != ShaderRecompiler::DescriptorKind::StorageImage)) continue;
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                const bool written = element >= binding.Flags().imageWritten.size() || binding.Flags().imageWritten[element];
                if (!written || binding.guestDescriptor.size() < (static_cast<std::size_t>(element) + 1) * 8) continue;
                const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8, 8);
                if (Graphics::IsNullTextureResource(words)) continue;
                try {
                    const auto resource = Graphics::DecodeTextureResource(words);
                    const auto surfaceBytes = Graphics::SurfaceGuestBytes(resource);
                    add(resource.baseAddress, surfaceBytes, {DrawWriteKind::StorageImage, static_cast<std::uint8_t>(stages[s].stage), element, 0, 0, compiled->variantId});
                    if (resource.dccAddress != 0 && surfaceBytes != 0) add(resource.dccAddress, Graphics::DccKeyCount(resource, surfaceBytes), {DrawWriteKind::StorageImageDcc, static_cast<std::uint8_t>(stages[s].stage), element, 0, 0, compiled->variantId});
                } catch (const std::runtime_error&) {
                }
            }
        }
    }
    const auto addColor = [&](const Graphics::ColorTarget& color, std::uint32_t slot) {
        if (color.bytes == 0) return;
        add(color.address, color.bytes, {DrawWriteKind::Color, 0xff, slot, color.extent.width, color.extent.height, 0});
        try {
            const auto keys = color.dccPipeAligned ? Graphics::DccKeyCount(Graphics::ColorTextureTileMode(color.tileMode), color.elementBytes, color.extent.width, color.extent.height, color.bytes) : Graphics::DccKeyBytes(color.bytes);
            add(color.dccAddress, keys, {DrawWriteKind::ColorDcc, 0xff, slot, color.extent.width, color.extent.height, 0});
        } catch (const std::runtime_error&) {
        }
        add(color.cmaskAddress, color.cmaskBytes, {DrawWriteKind::ColorCmask, 0xff, slot, color.extent.width, color.extent.height, 0});
    };
    if (graphics.hasColorTarget) addColor(graphics.color, 0);
    for (std::size_t slot = 0; slot < graphics.colors.size(); ++slot) addColor(graphics.colors[slot], static_cast<std::uint32_t>(slot + 1));
    if (graphics.depth && graphics.depth->extent.width != 0 && graphics.depth->extent.height != 0) {
        const auto& depth = *graphics.depth;
        const bool d16 = depth.format == VK_FORMAT_D16_UNORM || depth.format == VK_FORMAT_D16_UNORM_S8_UINT;
        try {
            add(depth.address, Graphics::DepthSliceBytes(depth.extent, d16 ? 2u : 4u), {DrawWriteKind::Depth, 0xff, 0, depth.extent.width, depth.extent.height, 0});
            add(depth.stencilAddress, Graphics::DepthSliceBytes(depth.extent, 1u), {DrawWriteKind::Stencil, 0xff, 0, depth.extent.width, depth.extent.height, 0});
        } catch (const std::runtime_error&) {
        }
    }
    const auto ranges = DrawWriteRanges(graphics, stages, true);
    const bool unbounded = UnboundedWrites(ranges);
    std::string reason = unbounded ? unboundedReason(graphics, stages) : std::string{};
    if (unbounded && UnboundedWrites(DrawWriteRanges(graphics, stages, true, true, true))) reason += " | narrowed: " + unboundedReason(graphics, stages, true, true);
    auto& state = writeDiag();
    std::lock_guard lock(state.mutex);
    if (unbounded) {
        ++state.unboundedDraws[reason];
        state.lastUnbounded = reason;
        if (state.unboundedPrinted < 16) {
            ++state.unboundedPrinted;
            std::string variants;
            for (const auto& stage : stages) {
                char item[40];
                std::snprintf(item, sizeof(item), " %u:0x%llx", static_cast<unsigned>(stage.stage), static_cast<unsigned long long>(stage.program != nullptr ? stage.program->variantId : 0));
                variants += item;
            }
            std::fprintf(stderr, "[pipe-writer] unbounded draw write set: %s (stages%s)\n", reason.c_str(), variants.c_str());
        }
    } else {
        ++state.boundedDraws;
    }
    if (state.entries > 200000) {
        state.tags.clear();
        state.entries = 0;
    }
    for (const auto& [range, tag] : found) {
        auto& bucket = state.tags[diagKey(range.first, range.second)];
        const auto same = std::find_if(bucket.begin(), bucket.end(), [&](const auto& kept) { return kept.first == range; });
        if (same != bucket.end()) same->second = tag;
        else {
            bucket.push_back({range, tag});
            ++state.entries;
        }
    }
}

void NoteIndirectWriterOverlap(std::uint64_t arguments, std::uint64_t argumentBytes, std::pair<std::uint64_t, std::uint64_t> writer, std::optional<bool> changed) {
    auto& state = writeDiag();
    std::lock_guard lock(state.mutex);
    ++state.events;
    if (changed) ++(*changed ? state.argumentsChanged : state.argumentsUnchanged);
    std::optional<DrawWriteTag> tag;
    if (const auto bucket = state.tags.find(diagKey(writer.first, writer.second)); bucket != state.tags.end()) {
        for (const auto& [range, kept] : bucket->second) {
            if (range == writer) tag = kept;
        }
    }
    const bool unboundedWriter = writer.first == 0 && writer.second == std::numeric_limits<std::uint64_t>::max();
    if (unboundedWriter) ++state.unboundedOverlaps[state.lastUnbounded];
    if (!tag) ++state.untagged;
    else {
        ++state.overlaps[static_cast<std::size_t>(tag->kind)];
        state.overlapBytes[static_cast<std::size_t>(tag->kind)] += writer.second - writer.first;
    }
    if (state.printed >= 64 || (unboundedWriter && state.printed >= 4)) return;
    ++state.printed;
    std::fprintf(stderr, "[pipe-writer] indirect args 0x%llx+0x%llx overlap a queued draw's %s 0x%llx+0x%llx (args at +0x%llx) stage %u element %u stride/width %u records/height %u variant 0x%llx changed %s\n",
            static_cast<unsigned long long>(arguments), static_cast<unsigned long long>(argumentBytes), tag ? DrawWriteKindName(tag->kind) : "untagged range",
            static_cast<unsigned long long>(writer.first), static_cast<unsigned long long>(writer.second - writer.first),
            static_cast<unsigned long long>(arguments >= writer.first ? arguments - writer.first : 0), tag ? static_cast<unsigned>(tag->stage) : 0u, tag ? tag->element : 0u, tag ? tag->a : 0u, tag ? tag->b : 0u,
            static_cast<unsigned long long>(tag ? tag->variant : 0), !changed ? "unknown" : *changed ? "yes" : "no");
}

std::string DrawWriteDiagText() {
    if (!PipeWriteDiag()) return {};
    auto& state = writeDiag();
    std::lock_guard lock(state.mutex);
    std::string text = "; indirect writer overlaps";
    char item[128];
    std::snprintf(item, sizeof(item), " %llu (untagged %llu, args changed %llu, unchanged %llu)", static_cast<unsigned long long>(state.events), static_cast<unsigned long long>(state.untagged), static_cast<unsigned long long>(state.argumentsChanged), static_cast<unsigned long long>(state.argumentsUnchanged));
    text += item;
    for (std::size_t kind = 0; kind < state.overlaps.size(); ++kind) {
        if (state.overlaps[kind] == 0) continue;
        std::snprintf(item, sizeof(item), " %s %llu (mean %.0f KiB)", DrawWriteKindNames[kind], static_cast<unsigned long long>(state.overlaps[kind]), static_cast<double>(state.overlapBytes[kind]) / static_cast<double>(state.overlaps[kind]) / 1024.0);
        text += item;
    }
    text += "; overlaps by unbounded writer:";
    for (const auto& [reason, count] : state.unboundedOverlaps) text += " [" + reason + "] " + std::to_string(count);
    std::snprintf(item, sizeof(item), "; enqueued draws bounded %llu, unbounded:", static_cast<unsigned long long>(state.boundedDraws));
    text += item;
    for (const auto& [reason, count] : state.unboundedDraws) text += " [" + reason + "] " + std::to_string(count);
    return text;
}

// Drain diagnostics (APS5_PIPE_DRAIN_DIAG=1): what the dispatch_writes, wait_unsatisfied and
// dispatch_copy drains of the queue-0 pipeline are.
namespace {

struct DrainDiagState {
    std::mutex mutex;
    std::map<std::string, std::uint64_t> dispatchWrites;
    std::uint64_t waitDrains = 0;
    std::uint64_t waitSatisfiedAfterDrain = 0;
    std::uint64_t waitTableBefore = 0;
    std::uint64_t waitTableSameQueueBefore = 0;
    std::map<std::string, std::uint64_t> waitEnds;
    std::map<std::string, double> waitEndMs;
    std::map<std::uint64_t, std::uint64_t> waitAddresses;
    std::map<std::string, std::uint64_t> copies;
};

DrainDiagState& drainDiag() {
    static auto* state = new DrainDiagState();
    return *state;
}

}

bool PipeDrainDiag() {
    static const bool diag = [] { const char* text = std::getenv("APS5_PIPE_DRAIN_DIAG"); return text != nullptr && text[0] == '1'; }();
    return diag;
}

void NoteUnboundedDispatchWrites(const ShaderRecompiler::RecompileResult& compiled, bool withNarrowed) {
    auto reason = programUnboundedReason(compiled, false);
    if (reason.empty()) reason = " write range overflow or zero size";
    if (withNarrowed) {
        auto left = programUnboundedReason(compiled, true);
        reason += " | narrowed:" + (left.empty() ? std::string(" bounded unless a range is empty or overflows") : left);
    }
    auto& state = drainDiag();
    std::lock_guard lock(state.mutex);
    ++state.dispatchWrites[reason];
}

WaitDiagNote& ThreadWaitDiag() {
    static thread_local WaitDiagNote note;
    return note;
}

void NoteUnsatisfiedWaitDrain(std::uint64_t awaited, bool satisfiedAfterDrain, bool tableBefore, bool sameQueueBefore) {
    auto& state = drainDiag();
    std::lock_guard lock(state.mutex);
    ++state.waitDrains;
    if (satisfiedAfterDrain) ++state.waitSatisfiedAfterDrain;
    if (tableBefore) ++state.waitTableBefore;
    if (sameQueueBefore) ++state.waitTableSameQueueBefore;
    if (state.waitAddresses.size() < 4096 || state.waitAddresses.contains(awaited)) ++state.waitAddresses[awaited];
}

void NoteUnsatisfiedWaitEnd(std::uint32_t queue, double ms) {
    static constexpr const char* ends[] = {"entry", "polled", "recorder", "fence", "history", "held", "aborted"};
    const auto& note = ThreadWaitDiag();
    std::string key = note.end >= 0 && note.end < 7 ? ends[note.end] : "unknown";
    key += note.writer < 0 ? " writer ?" : note.writer == static_cast<int>(queue) ? " writer same queue" : " writer queue " + std::to_string(note.writer);
    auto& state = drainDiag();
    std::lock_guard lock(state.mutex);
    ++state.waitEnds[key];
    state.waitEndMs[key] += ms;
}

void NoteCopyDrain(const char* kind, int path, bool copied) {
    static constexpr const char* paths[] = {"cpu", "gpu", "not imported", "aliased", "record pending", "shape", "overlapping", "inaccessible", "over gpu max"};
    std::string key = std::string(kind) + (copied ? " copied " : " kernel ") + (path >= 0 && path < 9 ? paths[path] : "(no path)");
    auto& state = drainDiag();
    std::lock_guard lock(state.mutex);
    ++state.copies[key];
}

std::string DrainDiagText() {
    if (!PipeDrainDiag()) return {};
    auto& state = drainDiag();
    std::lock_guard lock(state.mutex);
    std::string text = "; drain diag: dispatch writes unbounded:";
    for (const auto& [reason, count] : state.dispatchWrites) text += " [" + reason + "] " + std::to_string(count);
    char item[256];
    std::uint64_t repeated = 0;
    for (const auto& [address, count] : state.waitAddresses) repeated = std::max(repeated, count);
    std::snprintf(item, sizeof(item), "; wait_unsatisfied drains %llu (satisfied in memory after the drain %llu, label table matched before %llu, same queue %llu; %zu addresses, busiest %llu):", static_cast<unsigned long long>(state.waitDrains), static_cast<unsigned long long>(state.waitSatisfiedAfterDrain), static_cast<unsigned long long>(state.waitTableBefore), static_cast<unsigned long long>(state.waitTableSameQueueBefore), state.waitAddresses.size(), static_cast<unsigned long long>(repeated));
    text += item;
    for (const auto& [key, count] : state.waitEnds) {
        std::snprintf(item, sizeof(item), " [%s] %llu (%.1f ms)", key.c_str(), static_cast<unsigned long long>(count), state.waitEndMs[key]);
        text += item;
    }
    text += "; dispatch_copy drains:";
    for (const auto& [key, count] : state.copies) text += " [" + key + "] " + std::to_string(count);
    return text;
}

}
