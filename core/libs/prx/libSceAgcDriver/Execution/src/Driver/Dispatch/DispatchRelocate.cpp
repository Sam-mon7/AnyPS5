#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::size_t MaxAliases = 16384;
constexpr std::size_t MaxContentWords = 64;

std::uint64_t AlignedWord(std::uint32_t word) {
    return word & ~std::uint32_t {3};
}

bool ExactSwitch(const char* name) {
    const auto* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
}

template<typename Read>
std::uint64_t ContentKey(const RelocatableUserWord& word, std::uint32_t value, bool& readable, Read&& read) {
    std::uint64_t hash = 0x9e3779b97f4a7c15ull;
    const auto mix = [&](std::uint64_t mixed) {
        hash ^= mixed;
        hash *= 0x100000001b3ull;
    };
    mix(value & 3u);
    if (value == 0) {
        readable = false;
        return hash;
    }
    std::array<std::uint32_t, MaxContentWords> content;
    for (const auto& [offset, words] : word.reads) {
        const auto count = std::min<std::size_t>(words, MaxContentWords);
        if (!read(AlignedWord(value) + static_cast<std::uint64_t>(offset), std::span(content).first(count))) {
            readable = false;
            return hash;
        }
        mix(static_cast<std::uint64_t>(offset));
        for (std::size_t i = 0; i < count; ++i) mix(content[i]);
    }
    return hash;
}

template<typename Read>
std::uint64_t AliasKey(std::uint64_t identity, const DrawRelocation& shape, std::span<const std::uint32_t> userData, Read&& read) {
    std::uint64_t hash = identity;
    bool readable = true;
    for (std::uint32_t index = 0; index < userData.size(); ++index) {
        const auto word = std::find_if(shape.words.begin(), shape.words.end(), [&](const RelocatableUserWord& candidate) { return candidate.reg == index; });
        hash ^= word != shape.words.end() ? ContentKey(*word, userData[index], readable, read) : userData[index];
        hash *= 0x100000001b3ull;
    }
    return readable && hash != 0 ? hash : 0;
}

}

bool Driver::dispatchRelocationEnabled() {
    static const bool enabled = ExactSwitch("APS5_DISPATCH_RELOCATION");
    return enabled;
}

bool Driver::verifyDispatchRelocation() {
    static const bool verify = dispatchRelocationEnabled() && ExactSwitch("APS5_VERIFY_DISPATCH_RELOCATION");
    return verify;
}

std::shared_ptr<DispatchEntry> Driver::relocateDispatchEntry(std::unique_lock<std::mutex>& cacheLock, std::uint64_t identity, std::span<const std::uint32_t> userData) {
    auto& counters = dispatchRelocationCounters;
    ++counters.lookups;
    const auto known = dispatchRelocations.find(identity);
    if (known == dispatchRelocations.end() || known->second->words.empty()) {
        ++counters.unlearned;
        return nullptr;
    }
    const auto shape = known->second;
    cacheLock.unlock();
    const auto alias = AliasKey(identity, *shape, userData, [](std::uint64_t address, std::span<std::uint32_t> words) {
        return GuestMemory::CopyMapped(address, std::as_writable_bytes(words)) == GuestMemory::Compare::Equal;
    });
    cacheLock.lock();
    if (alias == 0) {
        ++counters.unreadable;
        return nullptr;
    }
    const auto found = dispatchAliases.find(alias);
    if (found == dispatchAliases.end() || found->second.userData.size() != userData.size()) return nullptr;
    const auto& donor = found->second;
    struct Move {
        std::uint32_t index, from, to;
    };
    std::vector<Move> moves;
    for (std::uint32_t index = 0; index < userData.size(); ++index) {
        if (donor.userData[index] != userData[index]) moves.push_back({index, donor.userData[index], userData[index]});
    }
    if (moves.empty()) return nullptr;
    ++counters.donors;
    auto relocated = std::make_shared<DispatchEntry>();
    for (const auto& variant : donor.entry->variants) {
        if (variant == nullptr || variant->compiled == nullptr) continue;
        const bool allowed = std::all_of(moves.begin(), moves.end(), [&](const Move& move) { return move.index < 64 && (variant->relocatableWords >> move.index & 1u) != 0 && move.from != 0 && move.to != 0 && ((move.from ^ move.to) & 3u) == 0; });
        if (!allowed) {
            ++counters.refused;
            continue;
        }
        auto copy = std::make_shared<DispatchVariant>();
        copy->runs = variant->runs;
        copy->runBases = variant->runBases;
        copy->relocatableWords = variant->relocatableWords;
        copy->words = variant->words;
        copy->forgetSerial = variant->forgetSerial;
        copy->compiled = variant->compiled;
        copy->shader = variant->shader;
        copy->dataPositions = variant->dataPositions;
        copy->dataSlots = variant->dataSlots;
        copy->flatBinding = variant->flatBinding;
        copy->pushOffset = variant->pushOffset;
        for (const auto& move : moves) {
            const auto delta = AlignedWord(move.to) - AlignedWord(move.from);
            for (std::size_t run = 0; run < copy->runs.size() && run < copy->runBases.size(); ++run) {
                if (copy->runBases[run] != static_cast<std::int8_t>(move.index)) continue;
                copy->runs[run].first += delta;
                copy->runs[run].second += delta;
            }
            const auto& forwarded = variant->compiled->shaderDataUserWords;
            for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
                if (forwarded[slot] == move.index) copy->shaderDataPatches.emplace_back(slot, move.to);
            }
        }
        applyShaderDataPatches(*copy);
        relocated->variants.push_back(std::move(copy));
    }
    if (relocated->variants.empty()) return nullptr;
    return relocated;
}

void Driver::learnDispatchRelocation(std::uint64_t identity, std::span<const std::uint32_t> userData, const std::shared_ptr<DispatchEntry>& entry, const DispatchVariant& variant) {
    const auto known = dispatchRelocations.find(identity);
    auto shape = std::make_shared<DrawRelocation>();
    if (known != dispatchRelocations.end()) *shape = *known->second;
    bool changed = known == dispatchRelocations.end();
    for (std::uint32_t index = 0; index < userData.size() && index < 64; ++index) {
        const auto held = std::find_if(shape->words.begin(), shape->words.end(), [&](const RelocatableUserWord& word) { return word.reg == index; });
        if ((variant.relocatableWords >> index & 1u) == 0) {
            if (held == shape->words.end()) continue;
            shape->words.erase(held);
            shape->banned.push_back(index);
            changed = true;
            continue;
        }
        if (held != shape->words.end() || std::find(shape->banned.begin(), shape->banned.end(), index) != shape->banned.end()) continue;
        RelocatableUserWord word{index, {}};
        const auto base = AlignedWord(userData[index]);
        for (std::size_t run = 0; run < variant.runs.size() && run < variant.runBases.size(); ++run) {
            if (variant.runBases[run] != static_cast<std::int8_t>(index)) continue;
            word.reads.emplace_back(static_cast<std::int64_t>(variant.runs[run].first - base), static_cast<std::uint32_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t)));
        }
        shape->words.push_back(std::move(word));
        changed = true;
    }
    if (changed) {
        std::sort(shape->words.begin(), shape->words.end(), [](const RelocatableUserWord& a, const RelocatableUserWord& b) { return a.reg < b.reg; });
        dispatchRelocations[identity] = shape;
    }
    if (shape->words.empty()) return;
    const auto alias = AliasKey(identity, *shape, userData, [&](std::uint64_t address, std::span<std::uint32_t> words) {
        std::size_t offset = 0;
        for (const auto& [begin, end] : variant.runs) {
            if (address >= begin && address + words.size_bytes() <= end) {
                std::memcpy(words.data(), variant.words.data() + offset + (address - begin) / sizeof(std::uint32_t), words.size_bytes());
                return true;
            }
            offset += static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
        }
        return false;
    });
    if (alias == 0) return;
    if (dispatchAliases.size() >= MaxAliases) dispatchAliases.clear();
    dispatchAliases[alias] = DispatchAlias{entry, std::vector<std::uint32_t>(userData.begin(), userData.end())};
}

void Driver::verifyRelocatedHit(const ShaderSnapshot& snapshot, std::size_t codeOffset, ShaderRecompiler::RecompileRequest request, std::span<const ShaderRecompiler::MemoryRegion> memory, std::uint64_t address, const DispatchVariant& variant, std::span<const std::uint32_t> words, const ShaderRecompiler::RecompileResult& reused) {
    const auto fail = [&](const char* what, std::size_t index) {
        std::fprintf(stderr, "[dispatch-relocate] APS5_VERIFY_DISPATCH_RELOCATION: %s disagrees (program 0x%llx, index %zu)\n", what, static_cast<unsigned long long>(address), index);
        std::fflush(stderr);
        std::abort();
    };
    auto verifyMemory = std::make_shared<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
    const auto invocation = InvocationFor(snapshot, codeOffset, request);
    const auto capture = verifyMemory->Capture(invocation);
    const auto regions = verifyMemory->Regions();
    request.context.memory = regions;
    const auto result = invocation.Materialize(*capture);
    std::vector<std::pair<std::uint64_t, std::uint32_t>> fresh, cached;
    for (const auto& region : regions) {
        const bool registered = std::any_of(memory.begin(), memory.end(), [&](const auto& known) { return region.guestAddress >= known.guestAddress && region.guestAddress < known.guestAddress + known.bytes.size(); });
        if (registered) continue;
        for (std::size_t i = 0; i + sizeof(std::uint32_t) <= region.bytes.size(); i += sizeof(std::uint32_t)) {
            std::uint32_t word = 0;
            std::memcpy(&word, region.bytes.data() + i, sizeof(word));
            fresh.emplace_back(region.guestAddress + i, word);
        }
    }
    std::size_t position = 0;
    for (const auto& [begin, end] : variant.runs) {
        for (auto at = begin; at < end; at += sizeof(std::uint32_t)) {
            if (position >= words.size()) fail("the cached word count", position);
            cached.emplace_back(at, words[position++]);
        }
    }
    std::sort(fresh.begin(), fresh.end());
    std::sort(cached.begin(), cached.end());
    if (fresh.size() != cached.size()) fail("the read set size", fresh.size());
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        if (fresh[i].first != cached[i].first) fail("the read set", i);
        if (fresh[i].second != cached[i].second) fail("the captured bytes", i);
    }
    if (result->variantId != reused.variantId) fail("the variant", 0);
    if (result->bindings.size() != reused.bindings.size() || result->pushConstants != reused.pushConstants) fail("the bindings", 0);
    for (std::size_t b = 0; b < result->bindings.size(); ++b) {
        const auto& made = result->bindings[b];
        const auto& kept = reused.bindings[b];
        if (made.kind != kept.kind || made.role != kept.role || made.binding != kept.binding || made.count != kept.count || made.guestDescriptor != kept.guestDescriptor) fail("the bindings", b);
    }
}

void Driver::reportDispatchRelocation() {
    auto& counters = dispatchRelocationCounters;
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-relocate] (10 s) %llu lookups without an entry: %llu with no learned shape, %llu with unreadable content; %llu donors with moved user words (%llu variants refused), %llu hits through a relocated entry (%llu kept under their own key), %llu relocated entries that did not validate; %zu shapes learned, %zu aliases held\n", static_cast<unsigned long long>(counters.lookups), static_cast<unsigned long long>(counters.unlearned), static_cast<unsigned long long>(counters.unreadable), static_cast<unsigned long long>(counters.donors), static_cast<unsigned long long>(counters.refused), static_cast<unsigned long long>(counters.hits), static_cast<unsigned long long>(counters.inserted), static_cast<unsigned long long>(counters.differing), dispatchRelocations.size(), dispatchAliases.size());
    counters = {};
}

DispatchRelocationCounters Driver::DispatchRelocationCounts() {
    std::lock_guard cacheLock(dispatchCacheMutex);
    return dispatchRelocationCounters;
}

}
