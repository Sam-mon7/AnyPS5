#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTUREDUPLICATES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTUREDUPLICATES_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace AgcDriver::Graphics {

enum class TextureBacking : std::uint8_t { Snapshot, Mirror, DepthCopy, StorageView, Storage };

enum class DuplicateKind : std::uint8_t { SampledStorage, ViewVariant, SameDescriptor, FormatVariant, ShapeVariant, Superseded };

constexpr std::size_t DuplicateKindCount = 6;
constexpr std::size_t ViewFieldCount = 5;

struct TextureCopy {
    std::uint64_t address = 0;
    std::uint64_t guestBytes = 0;
    std::uint64_t gpuBytes = 0;
    std::uint64_t counted = 0;
    std::uint64_t used = 0;
    std::uint64_t other = 0;
    std::uint32_t pipeBankXor = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t mips = 0;
    std::uint32_t dimension = 0;
    std::uint32_t tile = 0;
    std::uint32_t format = 0;
    std::uint32_t elementBytes = 0;
    std::uint32_t blockWidth = 0;
    std::uint32_t baseLevel = 0;
    std::uint32_t lastLevel = 0;
    std::uint32_t baseArray = 0;
    std::uint32_t minLod = 0;
    std::array<std::uint32_t, 4> components{};
    TextureBacking backing = TextureBacking::Snapshot;
    bool compressed = false;
    bool depthCompare = false;
    bool stale = false;
};

struct DuplicateTotals {
    std::size_t copies = 0;
    std::uint64_t bytes = 0;
    void Add(std::uint64_t added) {
        ++copies;
        bytes += added;
    }
};

struct TextureDuplicateRange {
    std::uint64_t address = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mips = 0;
    std::uint32_t format = 0;
    std::size_t copies = 0;
    std::uint64_t heldBytes = 0;
    std::uint64_t extraBytes = 0;
    std::array<std::size_t, DuplicateKindCount> kinds{};
};

struct TextureDuplicateSummary {
    std::size_t entries = 0;
    std::size_t images = 0;
    std::size_t ranges = 0;
    std::size_t duplicatedRanges = 0;
    std::uint64_t heldBytes = 0;
    std::uint64_t extraBytes = 0;
    std::array<DuplicateTotals, DuplicateKindCount> kinds{};
    std::array<DuplicateTotals, ViewFieldCount> viewFields{};
    DuplicateTotals stale;
    DuplicateTotals overlapping;
    DuplicateTotals nested;
    DuplicateTotals depthCopies;
    DuplicateTotals uncounted;
    std::vector<TextureDuplicateRange> top;
};

inline const char* DuplicateKindName(DuplicateKind kind) {
    switch (kind) {
        case DuplicateKind::SampledStorage: return "sampled+storage";
        case DuplicateKind::ViewVariant: return "view";
        case DuplicateKind::SameDescriptor: return "same descriptor";
        case DuplicateKind::FormatVariant: return "format";
        case DuplicateKind::ShapeVariant: return "extent/mips";
        case DuplicateKind::Superseded: return "superseded";
    }
    return "";
}

inline const char* ViewFieldName(std::size_t field) {
    constexpr std::array<const char*, ViewFieldCount> names{"swizzle", "mip range", "base layer", "min LOD", "other"};
    return field < names.size() ? names[field] : "";
}

inline bool SameTextureLayout(const TextureCopy& first, const TextureCopy& second) {
    return first.pipeBankXor == second.pipeBankXor && first.width == second.width && first.height == second.height && first.depth == second.depth && first.mips == second.mips && first.dimension == second.dimension && first.tile == second.tile;
}

inline bool SameTextureShape(const TextureCopy& first, const TextureCopy& second) {
    return SameTextureLayout(first, second) && first.format == second.format && first.depthCompare == second.depthCompare;
}

inline bool StorageServesSampled(const TextureCopy& image, const TextureCopy& sampled) {
    return !image.compressed && !sampled.compressed && !sampled.depthCompare && image.pipeBankXor == sampled.pipeBankXor && image.width == sampled.width && image.height == sampled.height && image.depth == sampled.depth && image.dimension == sampled.dimension && image.tile == sampled.tile && sampled.mips <= image.mips && image.elementBytes == sampled.elementBytes && image.blockWidth == sampled.blockWidth;
}

inline unsigned TextureViewDifference(const TextureCopy& first, const TextureCopy& second) {
    unsigned fields = 0;
    if (first.components != second.components) fields |= 1u << 0u;
    if (first.baseLevel != second.baseLevel || first.lastLevel != second.lastLevel) fields |= 1u << 1u;
    if (first.baseArray != second.baseArray) fields |= 1u << 2u;
    if (first.minLod != second.minLod) fields |= 1u << 3u;
    if (first.other != second.other) fields |= 1u << 4u;
    return fields;
}

inline DuplicateKind ClassifyDuplicate(const TextureCopy& extra, const TextureCopy& kept, unsigned& viewFields) {
    viewFields = 0;
    if (extra.stale && !kept.stale) return DuplicateKind::Superseded;
    const bool extraStorage = extra.backing == TextureBacking::Storage;
    if (extraStorage != (kept.backing == TextureBacking::Storage) && (extraStorage ? StorageServesSampled(extra, kept) : StorageServesSampled(kept, extra))) return DuplicateKind::SampledStorage;
    if (SameTextureShape(extra, kept)) {
        viewFields = TextureViewDifference(extra, kept);
        return viewFields == 0 ? DuplicateKind::SameDescriptor : DuplicateKind::ViewVariant;
    }
    return SameTextureLayout(extra, kept) ? DuplicateKind::FormatVariant : DuplicateKind::ShapeVariant;
}

inline std::string DescribeDuplicateKinds(const TextureDuplicateRange& range) {
    std::string text;
    for (std::size_t kind = 0; kind < DuplicateKindCount; ++kind) {
        if (range.kinds[kind] == 0) continue;
        if (!text.empty()) text += ", ";
        text += DuplicateKindName(static_cast<DuplicateKind>(kind));
        text += " x" + std::to_string(range.kinds[kind]);
    }
    return text;
}

inline TextureDuplicateSummary SummarizeTextureDuplicates(std::span<const TextureCopy> copies, std::size_t topCount = 10) {
    TextureDuplicateSummary summary;
    summary.entries = copies.size();
    std::vector<const TextureCopy*> owners;
    owners.reserve(copies.size());
    for (const auto& copy : copies) {
        if (copy.gpuBytes == 0) continue;
        owners.push_back(&copy);
        summary.heldBytes += copy.gpuBytes;
        if (copy.stale) summary.stale.Add(copy.gpuBytes);
        if (copy.backing != TextureBacking::DepthCopy) continue;
        summary.depthCopies.Add(copy.gpuBytes);
        if (copy.counted == 0) summary.uncounted.Add(copy.gpuBytes);
    }
    summary.images = owners.size();
    std::stable_sort(owners.begin(), owners.end(), [](const TextureCopy* first, const TextureCopy* second) { return first->address < second->address; });
    const auto rank = [](const TextureCopy* copy) { return std::make_tuple(!copy->stale, copy->backing == TextureBacking::Storage, copy->used, copy->gpuBytes); };
    std::vector<TextureDuplicateRange> duplicated;
    std::uint64_t outerEnd = 0;
    for (std::size_t first = 0; first < owners.size();) {
        std::size_t last = first;
        std::uint64_t groupEnd = 0;
        for (; last < owners.size() && owners[last]->address == owners[first]->address; ++last) groupEnd = std::max(groupEnd, owners[last]->address + owners[last]->guestBytes);
        for (std::size_t index = first; index < last; ++index) {
            const auto& copy = *owners[index];
            if (copy.address >= outerEnd) continue;
            summary.overlapping.Add(copy.gpuBytes);
            if (copy.address + copy.guestBytes <= outerEnd) summary.nested.Add(copy.gpuBytes);
        }
        outerEnd = std::max(outerEnd, groupEnd);
        ++summary.ranges;
        if (last - first > 1) {
            const std::span<const TextureCopy* const> members(owners.data() + first, last - first);
            const auto kept = *std::max_element(members.begin(), members.end(), [&](const TextureCopy* left, const TextureCopy* right) { return rank(left) < rank(right); });
            const auto largest = *std::max_element(members.begin(), members.end(), [](const TextureCopy* left, const TextureCopy* right) { return left->gpuBytes < right->gpuBytes; });
            TextureDuplicateRange range;
            range.address = kept->address;
            range.width = largest->width;
            range.height = largest->height;
            range.mips = largest->mips;
            range.format = largest->format;
            range.copies = members.size();
            for (const auto* member : members) {
                range.heldBytes += member->gpuBytes;
                if (member == kept) continue;
                unsigned fields = 0;
                const auto kind = ClassifyDuplicate(*member, *kept, fields);
                range.extraBytes += member->gpuBytes;
                ++range.kinds[static_cast<std::size_t>(kind)];
                summary.kinds[static_cast<std::size_t>(kind)].Add(member->gpuBytes);
                for (std::size_t field = 0; field < ViewFieldCount; ++field) {
                    if (((fields >> field) & 1u) != 0) summary.viewFields[field].Add(member->gpuBytes);
                }
            }
            summary.extraBytes += range.extraBytes;
            duplicated.push_back(range);
        }
        first = last;
    }
    summary.duplicatedRanges = duplicated.size();
    const auto shown = std::min(topCount, duplicated.size());
    std::partial_sort(duplicated.begin(), duplicated.begin() + static_cast<std::ptrdiff_t>(shown), duplicated.end(), [](const TextureDuplicateRange& left, const TextureDuplicateRange& right) { return left.extraBytes != right.extraBytes ? left.extraBytes > right.extraBytes : left.address < right.address; });
    summary.top.assign(duplicated.begin(), duplicated.begin() + static_cast<std::ptrdiff_t>(shown));
    return summary;
}

inline void CanonicalizeTextureWords(std::array<std::uint32_t, 8>& words) {
    const auto baseLevel = (words[3] >> 12u) & 0xfu;
    auto lastLevel = (words[3] >> 16u) & 0xfu;
    const auto maxMip = (words[5] >> 4u) & 0xfu;
    if (baseLevel <= maxMip) lastLevel = std::min(lastLevel, maxMip);
    const auto minLod = (words[1] >> 8u) & 0xfffu;
    const auto effective = minLod <= baseLevel * 256u ? 0u : std::min(minLod, lastLevel * 256u);
    words[1] = (words[1] & ~((0xfffu << 8u) | (1u << 29u))) | (effective << 8u);
    words[2] &= ~(3u << 30u);
    words[3] &= ~0xfffu;
    words[4] &= ~((7u << 13u) | (7u << 29u));
    words[5] &= ~((7u << 20u) | (1u << 24u) | (31u << 27u));
    words[6] &= ~((3u << 8u) | (15u << 11u) | (15u << 15u) | (1u << 20u));
    if (((words[6] >> 21u) & 1u) != 0) return;
    words[6] &= ~((1u << 19u) | (3u << 22u) | (0xffu << 24u));
    words[7] = 0;
}

}

#endif
