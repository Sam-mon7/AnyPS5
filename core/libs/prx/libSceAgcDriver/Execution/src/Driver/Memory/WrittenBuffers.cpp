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
                const auto surfaceBytes = Graphics::DescribeSurface(resource).guestBytes;
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

}
