#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINESTATEREUSE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINESTATEREUSE_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace AgcDriver::Graphics {

class PipelineStateReuse {
public:
    enum class Dynamic : std::uint8_t {
        Viewport,
        Scissor,
        DepthBias,
        DepthBounds,
        CullMode,
        FrontFace,
        DepthTest,
        DepthWrite,
        DepthCompare,
        DepthBoundsTest,
        StencilTest,
        FrontStencilOp,
        FrontCompareMask,
        FrontWriteMask,
        FrontReference,
        BackStencilOp,
        BackCompareMask,
        BackWriteMask,
        BackReference,
        Count,
    };
    static constexpr std::size_t PushSlots = 2;
    static constexpr std::size_t PushSlotBytes = 128;

    void Reset() { *this = PipelineStateReuse{}; }

    bool Bound(VkPipeline pipeline) const { return pipeline != VK_NULL_HANDLE && pipeline == bound; }

    void NoteBind(VkPipeline pipeline, VkPipelineLayout layout, bool libraries) {
        if (!libraries || !boundLibraries) dynamic = {};
        if (layout != pushLayout) {
            push = {};
            pushLayout = layout;
        }
        bound = pipeline;
        boundLibraries = libraries;
    }

    template<typename... TValues>
    bool Same(Dynamic slot, const TValues&... values) const {
        const auto packed = pack(values...);
        const auto& held = dynamic[static_cast<std::size_t>(slot)];
        return held.size == packed.size && std::memcmp(held.bytes.data(), packed.bytes.data(), packed.size) == 0;
    }

    template<typename... TValues>
    void Note(Dynamic slot, const TValues&... values) {
        dynamic[static_cast<std::size_t>(slot)] = pack(values...);
    }

    bool SamePush(VkPipelineLayout layout, VkShaderStageFlags stages, std::size_t slot, std::span<const std::byte, PushSlotBytes> bytes) const {
        const auto& held = push.at(slot);
        return stages != 0 && layout == pushLayout && held.stages == stages && std::memcmp(held.bytes.data(), bytes.data(), PushSlotBytes) == 0;
    }

    void NotePush(VkPipelineLayout layout, VkShaderStageFlags stages, std::size_t slot, std::span<const std::byte, PushSlotBytes> bytes) {
        if (layout != pushLayout) {
            push = {};
            pushLayout = layout;
        }
        auto& held = push.at(slot);
        held.stages = stages;
        std::memcpy(held.bytes.data(), bytes.data(), PushSlotBytes);
    }

private:
    struct Value {
        std::array<std::byte, 24> bytes{};
        std::size_t size = 0;
    };
    struct PushValue {
        std::array<std::byte, PushSlotBytes> bytes{};
        VkShaderStageFlags stages = 0;
    };

    template<typename... TValues>
    static Value pack(const TValues&... values) {
        static_assert((sizeof(TValues) + ...) <= sizeof(Value::bytes));
        Value value;
        const auto append = [&](const auto& part) {
            std::memcpy(value.bytes.data() + value.size, &part, sizeof(part));
            value.size += sizeof(part);
        };
        (append(values), ...);
        return value;
    }

    std::array<Value, static_cast<std::size_t>(Dynamic::Count)> dynamic{};
    std::array<PushValue, PushSlots> push{};
    VkPipeline bound = VK_NULL_HANDLE;
    VkPipelineLayout pushLayout = VK_NULL_HANDLE;
    bool boundLibraries = false;
};

}

#endif
