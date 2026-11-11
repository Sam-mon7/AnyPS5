#include "prx/libSceAgcDriver/Execution/include/Driver/Replay/RegionCensus.hpp"
#include <algorithm>

namespace AgcDriver::DriverDetail::Replay {

std::uint64_t ResolvedSignature(std::span<const ResolvedItem> items) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 0x100000001b3ull; };
    mix(items.size());
    for (const auto& item : items) {
        mix(item.offset);
        mix(static_cast<std::uint64_t>(item.kind));
        for (const auto* fields : {&item.prepared, &item.committed}) {
            for (const auto& values : *fields) {
                mix(values.size());
                for (const auto value : values) mix(value);
            }
        }
    }
    return hash;
}

ResolvedDifference CompareResolved(std::span<const ResolvedItem> before, std::span<const ResolvedItem> after) {
    const auto count = std::min(before.size(), after.size());
    for (std::size_t item = 0; item < count; ++item) {
        if (before[item].offset != after[item].offset || before[item].kind != after[item].kind) return {false, item, ResolvedFieldNames.size(), 0, before[item].offset, after[item].offset};
        for (std::size_t field = 0; field < ResolvedFieldNames.size(); ++field) {
            for (const auto committed : {false, true}) {
                const auto& a = (committed ? before[item].committed : before[item].prepared)[field];
                const auto& b = (committed ? after[item].committed : after[item].prepared)[field];
                const auto common = std::min(a.size(), b.size());
                for (std::size_t word = 0; word < common; ++word) {
                    if (a[word] != b[word]) return {false, item, field, word, a[word], b[word]};
                }
                if (a.size() != b.size()) return {false, item, field, common, a.size(), b.size()};
            }
        }
    }
    return before.size() == after.size() ? ResolvedDifference{} : ResolvedDifference{false, count, ResolvedFieldNames.size(), 0, before.size(), after.size()};
}

}
