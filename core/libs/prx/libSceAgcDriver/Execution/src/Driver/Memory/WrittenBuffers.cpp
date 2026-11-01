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

std::vector<std::pair<std::uint64_t, std::uint64_t>> DispatchWriteRanges(const ShaderRecompiler::RecompileResult& compiled, bool& bounded) {
    bounded = compiled.bdaAbiVersion == 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    const auto add = [&](std::uint64_t address, std::uint64_t bytes) {
        if (address == 0) return;
        if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) bounded = false;
        else ranges.emplace_back(address, address + bytes);
    };
    for (const auto& binding : compiled.bindings) {
        if (binding.role == ShaderRecompiler::DescriptorRole::Gds || binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable || binding.role == ShaderRecompiler::DescriptorRole::FaultBuffer) bounded = false;
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

std::vector<std::pair<std::uint64_t, std::uint64_t>> DrawWriteRanges(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages, bool exact) {
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
        auto writes = DispatchWriteRanges(*stage.program, bounded);
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
        if (depth.extent.width == 0 || depth.extent.height == 0 || depth.extent.width > 16384 || depth.extent.height > 16384 || depth.samples != 1 || depth.firstSlice != 0 || depth.lastSlice != 0 || depth.htileAddress != 0) return {Unbounded};
        if (exact && depth.extent.width != 0 && depth.extent.height != 0) {
            const bool d16 = depth.format == VK_FORMAT_D16_UNORM || depth.format == VK_FORMAT_D16_UNORM_S8_UINT;
            add(depth.address, Graphics::DepthSliceBytes(depth.extent, d16 ? 2u : 4u));
            add(depth.stencilAddress, Graphics::DepthSliceBytes(depth.extent, 1u));
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

std::vector<std::pair<std::uint64_t, std::uint64_t>> Driver::drawWriteRanges(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages) {
    return DrawWriteRanges(graphics, stages, ExactDrawWrites());
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
};

WriteDiagState& writeDiag() {
    static auto* state = new WriteDiagState();
    return *state;
}

constexpr std::array<const char*, static_cast<std::size_t>(DrawWriteKind::Count)> DrawWriteKindNames{"buffer_written", "buffer_assumed", "storage_image", "storage_image_dcc", "color", "color_dcc", "color_cmask", "depth", "stencil"};

std::uint64_t diagKey(std::uint64_t begin, std::uint64_t end) {
    return begin * 0x9e3779b97f4a7c15ull ^ end;
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
    auto& state = writeDiag();
    std::lock_guard lock(state.mutex);
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
    if (!tag) ++state.untagged;
    else {
        ++state.overlaps[static_cast<std::size_t>(tag->kind)];
        state.overlapBytes[static_cast<std::size_t>(tag->kind)] += writer.second - writer.first;
    }
    if (state.printed >= 64) return;
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
    return text;
}

}
