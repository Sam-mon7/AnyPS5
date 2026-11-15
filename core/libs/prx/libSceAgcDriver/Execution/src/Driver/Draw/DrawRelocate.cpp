#include "prx/libSceAgcDriver/Execution/include/ThreadScratch.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/PointerSet.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"
#include "ControlFlow/GraphBuilder.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <map>
#include <set>
#include <unordered_set>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::size_t MaxAliases = 16384;
constexpr std::size_t MaxContentWords = 64;

std::uint64_t AlignedWord(std::uint32_t word) {
    return word & ~std::uint32_t {3};
}

std::vector<std::size_t> runWordOffsets(const DispatchVariant& variant) {
    std::vector<std::size_t> offsets(variant.runs.size() + 1);
    for (std::size_t run = 0; run < variant.runs.size(); ++run) offsets[run + 1] = offsets[run] + (variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t);
    return offsets;
}

std::uint32_t relocationMask(const DispatchVariant& variant, std::size_t position) {
    if (std::find(variant.dataPositions.begin(), variant.dataPositions.end(), position) != variant.dataPositions.end()) return 0xffffffffu;
    const auto found = std::find_if(variant.bufferBaseMasks.begin(), variant.bufferBaseMasks.end(), [&](const auto& mask) { return mask.first == position; });
    return found != variant.bufferBaseMasks.end() ? found->second : 0;
}

}

bool Driver::drawRelocationEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_RELOCATION") == nullptr;
    return enabled;
}

std::uint64_t Driver::nestedContentKey(const RelocatableUserWord& word, std::uint32_t value, bool& readable, bool requireProof) {
    if (word.nestedVariant == nullptr || word.value) return relocatedContentKey(word, value, readable);
    const auto& variant = *word.nestedVariant;
    if (requireProof && !variant.nestedPointerProof) return relocatedContentKey(word, value, readable);
    if (variant.runParents.size() != variant.runs.size() || std::find(variant.runBases.begin(), variant.runBases.end(), static_cast<std::int8_t>(word.nestedUserWord)) == variant.runBases.end()) return relocatedContentKey(word, value, readable);
    const auto offsets = runWordOffsets(variant);
    struct AddressesTag {};
    struct StatesTag {};
    struct LiveTag {};
    auto& addresses = AgcDriver::ThreadScratch<std::vector<std::uint64_t>, AddressesTag>();
    auto& states = AgcDriver::ThreadScratch<std::vector<std::uint8_t>, StatesTag>();
    auto& live = AgcDriver::ThreadScratch<std::vector<std::uint32_t>, LiveTag>();
    addresses.assign(variant.runs.size(), 0);
    states.assign(variant.runs.size(), 0);
    live.resize(variant.words.size());
    std::uint64_t hash = 0x9e3779b97f4a7c15ull;
    const auto mix = [&](std::uint64_t mixed) { hash = (hash ^ mixed) * 0x100000001b3ull; };
    mix(value & 3u);
    if (value == 0) {
        readable = false;
        ArchB::Recorder::Count("draw_reloc_alias_value_zero");
    }
    const auto wordMask = [&](std::size_t run, std::size_t index) {
        auto mask = relocationMask(variant, offsets[run] + index);
        for (const auto& child : variant.runParents) {
            if (child.run != run) continue;
            if (index == child.word) mask |= 0xfffffffcu;
            if (index == child.word + 1) mask = 0xffffffffu;
        }
        return mask;
    };
    const auto visit = [&](auto&& self, std::size_t run) -> bool {
        if (states[run] != 0) return states[run] >= 2;
        states[run] = 1;
        const auto& parent = variant.runParents[run];
        std::uint64_t address = 0;
        std::int64_t relative = 0;
        if (parent.run == DispatchVariant::NoFlatBinding) {
            if (run >= variant.runBases.size() || variant.runBases[run] != static_cast<std::int8_t>(word.nestedUserWord)) return false;
            relative = static_cast<std::int64_t>(variant.runs[run].first - word.nestedBase);
            address = AlignedWord(value) + static_cast<std::uint64_t>(relative);
        } else {
            if (parent.run >= variant.runs.size() || !self(self, parent.run) || states[parent.run] != 2) return false;
            std::uint64_t current = 0, old = 0;
            const auto position = offsets[parent.run] + parent.word;
            if (position + 1 >= variant.words.size()) return false;
            std::memcpy(&old, variant.words.data() + position, sizeof(old));
            std::memcpy(&current, live.data() + position, sizeof(current));
            if (current == 0) {
                readable = false;
                ArchB::Recorder::Count("draw_reloc_alias_pointer_zero");
                return false;
            }
            relative = static_cast<std::int64_t>(variant.runs[run].first - (old & 0x0000fffffffffffcull));
            address = (current & 0x0000fffffffffffcull) + static_cast<std::uint64_t>(relative);
        }
        addresses[run] = address;
        const auto count = offsets[run + 1] - offsets[run];
        mix(static_cast<std::uint64_t>(relative));
        mix(count);
        bool opaque = true;
        for (std::size_t index = 0; opaque && index < count; ++index) opaque = wordMask(run, index) == 0xffffffffu;
        if (opaque && std::none_of(variant.runParents.begin(), variant.runParents.end(), [&](const auto& child) { return child.run == run; })) {
            for (std::size_t index = 0; index < count; ++index) mix(0);
            states[run] = 3;
            return true;
        }
        if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(live).subspan(offsets[run], count)), true) != GuestMemory::Compare::Equal) {
            readable = false;
            ArchB::Recorder::Count(parent.run == DispatchVariant::NoFlatBinding ? "draw_reloc_alias_root_copy_failed" : "draw_reloc_alias_child_copy_failed");
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) mix(live[offsets[run] + index] & ~wordMask(run, index));
        states[run] = 2;
        return true;
    };
    for (std::size_t run = 0; run < variant.runs.size(); ++run) static_cast<void>(visit(visit, run));
    return hash;
}

std::uint64_t Driver::relocatedContentKey(const RelocatableUserWord& word, std::uint32_t value, bool& readable) {
    std::uint64_t hash = 0x9e3779b97f4a7c15ull;
    const auto mix = [&](std::uint64_t mixed) {
        hash ^= mixed;
        hash *= 0x100000001b3ull;
    };
    if (word.value) return hash;
    mix(value & 3u);
    if (value == 0) {
        readable = false;
        return hash;
    }
    std::array<std::uint32_t, MaxContentWords> content;
    for (const auto& [offset, words] : word.reads) {
        mix(static_cast<std::uint64_t>(offset));
        mix(words);
        for (std::size_t done = 0; done < words; done += MaxContentWords) {
            const auto count = std::min<std::size_t>(words - done, MaxContentWords);
            const auto address = (static_cast<std::uint64_t>(word.high) << 32u) + AlignedWord(value) + static_cast<std::uint64_t>(offset) + done * sizeof(std::uint32_t);
            if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(content).first(count))) != GuestMemory::Compare::Equal) {
                readable = false;
                ArchB::Recorder::Count("draw_reloc_alias_flat_copy_failed");
                return hash;
            }
            for (std::size_t i = 0; i < count; ++i) mix(content[i]);
        }
    }
    return hash;
}

void Driver::tagUserValueWords(DispatchVariant& variant, const DrawProgram& program) {
    if (!drawRelocationEnabled() || variant.compiled == nullptr) return;
    const auto& compiled = *variant.compiled;
    const auto& forwarded = compiled.shaderDataUserWords;
    const auto users = std::min<std::size_t>(program.userData.size(), 64);
    const auto bound = [&](std::uint32_t value) {
        std::size_t count = 0;
        for (const auto& binding : compiled.bindings) count += static_cast<std::size_t>(std::count(binding.guestDescriptor.begin(), binding.guestDescriptor.end(), value));
        return count;
    };
    const auto elsewhere = [&](std::uint32_t index, std::uint32_t value) {
        for (std::size_t slot = 0; (slot + 1) * sizeof(std::uint32_t) <= compiled.pushConstants.size(); ++slot) {
            std::uint32_t word = 0;
            std::memcpy(&word, compiled.pushConstants.data() + slot * sizeof(std::uint32_t), sizeof(word));
            if (word == value && (slot >= forwarded.size() || forwarded[slot] != index)) return true;
        }
        if (std::any_of(compiled.specialization.begin(), compiled.specialization.end(), [&](const auto& constant) { return constant.value == value; })) return true;
        return std::find(variant.words.begin(), variant.words.end(), value) != variant.words.end();
    };
    const auto readThrough = [&](const std::uint32_t* descriptor) {
        const auto base = static_cast<std::uint64_t>(descriptor[0]) | static_cast<std::uint64_t>(descriptor[1] & 0xffffu) << 32u;
        const auto stride = (descriptor[1] >> 16u) & 0x3fffu;
        const auto bytes = std::max<std::uint64_t>(stride != 0 ? static_cast<std::uint64_t>(stride) * descriptor[2] : descriptor[2], 16u);
        return std::any_of(variant.runs.begin(), variant.runs.end(), [&](const auto& run) { return run.first < base + bytes && run.second > base; });
    };
    const auto taken = [&](std::uint32_t index) { return ((variant.relocatableWords | variant.valueWords | variant.calleeWords) >> index & 1u) != 0; };
    if (ShaderRecompiler::GcnGuestShaders() && program.binary.stage == ShaderRecompiler::ShaderStage::Vertex) {
        for (const auto instruction : program.binary.code) {
            if ((instruction & 0xff80ff00u) != 0xbe802100u) continue;
            const auto target = instruction & 0xffu;
            if (target < program.firstUserSgpr) continue;
            const auto index = program.hiddenUserWords + target - program.firstUserSgpr;
            if (index + 1 >= users || taken(index) || taken(index + 1)) continue;
            const auto low = program.userData[index];
            const auto address = static_cast<std::uint64_t>(low) | static_cast<std::uint64_t>(program.userData[index + 1]) << 32u;
            if (address == 0 || address % 4u != 0 || bound(low) != 0 || elsewhere(index, low)) continue;
            std::array<std::uint32_t, 1024> code;
            if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(code))) != GuestMemory::Compare::Equal) continue;
            std::optional<std::uint32_t> length;
            try {
                length = ShaderRecompiler::UserDataCalleeLength(code, (instruction >> 16u) & 0x7fu);
            } catch (const std::exception&) {
                continue;
            }
            if (!length || *length == 0) continue;
            variant.calleeWords |= std::uint64_t {1} << index;
            variant.calleeReads.emplace_back(index, *length);
        }
    }
    for (std::uint32_t index = program.hiddenUserWords; index + 2 < users; ++index) {
        const auto base = program.userData[index];
        if (base == 0 || taken(index) || bound(base) != 1 || elsewhere(index, base)) continue;
        std::uint32_t binding = 0, word = 0;
        for (std::size_t b = 0; b < compiled.bindings.size(); ++b) {
            const auto& descriptor = compiled.bindings[b].guestDescriptor;
            const auto found = std::find(descriptor.begin(), descriptor.end(), base);
            if (found == descriptor.end()) continue;
            binding = static_cast<std::uint32_t>(b);
            word = static_cast<std::uint32_t>(found - descriptor.begin());
        }
        const auto& target = compiled.bindings[binding];
        const auto element = word / 4u;
        const auto& flags = target.Flags();
        const bool readOnly = target.role == ShaderRecompiler::DescriptorRole::GuestBuffers && target.guestDescriptor.size() == static_cast<std::size_t>(target.count) * 4u && word % 4u == 0 && element < flags.bufferWritten.size() && !flags.bufferWritten[element] && (element >= flags.bufferAtomic.size() || !flags.bufferAtomic[element]);
        if (!readOnly || target.guestDescriptor[word + 1] != program.userData[index + 1] || target.guestDescriptor[word + 2] != program.userData[index + 2] || readThrough(target.guestDescriptor.data() + word)) continue;
        variant.valueWords |= std::uint64_t {1} << index;
        variant.valueSlots.push_back({index, binding, word});
        const auto records = program.userData[index + 2];
        if (records != 0 && !taken(index + 2) && bound(records) == 1 && !elsewhere(index + 2, records)) {
            variant.valueWords |= std::uint64_t {1} << (index + 2);
            variant.valueSlots.push_back({index + 2, binding, word + 2});
        }
    }
}

void Driver::tagVariantRuns(DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, const ShaderRecompiler::SrtUserDataUse& use, bool eligible, std::uint32_t hiddenWords, std::span<const std::shared_ptr<const ShaderRecompiler::SrtGuestWords>> supplied) {
    variant.userWordsWalked = use.asBase | use.asValue;
    std::uint64_t relocatable = eligible && drawRelocationEnabled() ? use.asBase & ~use.asValue : 0;
    if (hiddenWords != 0) relocatable &= hiddenWords >= 64 ? 0 : ~((std::uint64_t {1} << hiddenWords) - 1);
    std::vector<std::pair<std::uint64_t, std::int32_t>> reads;
    const auto covered = [&](std::uint64_t address) {
        return std::any_of(regions.begin(), regions.end(), [&](const ShaderRecompiler::MemoryRegion& region) { return address >= region.guestAddress && address - region.guestAddress + sizeof(std::uint32_t) <= region.bytes.size(); });
    };
    if (relocatable != 0) {
        reads = use.reads;
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        const auto drop = [&](std::int32_t origin) {
            if (origin >= 0) relocatable &= ~(std::uint64_t {1} << origin);
        };
        for (std::size_t i = 0; i < reads.size(); ++i) {
            if (i + 1 < reads.size() && reads[i].first == reads[i + 1].first) {
                drop(reads[i].second);
                drop(reads[i + 1].second);
            }
            if (reads[i].second >= 0 && ((reads[i].first & 3u) != 0 || !covered(reads[i].first))) drop(reads[i].second);
        }
    }
    const auto origin = [&](std::uint64_t address) -> std::int8_t {
        const auto found = std::lower_bound(reads.begin(), reads.end(), std::make_pair(address, std::numeric_limits<std::int32_t>::min()));
        if (found == reads.end() || found->first != address || found->second < 0) return -1;
        return (relocatable >> found->second & 1u) != 0 ? static_cast<std::int8_t>(found->second) : std::int8_t {-1};
    };
    variant.relocatableWords = relocatable;
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const auto& region = regions[index];
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        const auto offset = variant.words.size();
        variant.words.resize(offset + count);
        std::memcpy(variant.words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        if (relocatable == 0) {
            AppendSharedWords(variant.shared, variant.runs.size(), offset, region.bytes, supplied);
            variant.runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
            variant.runBases.push_back(-1);
            continue;
        }
        std::size_t start = 0;
        auto current = count != 0 ? origin(region.guestAddress) : std::int8_t {-1};
        for (std::size_t word = 1; word <= count; ++word) {
            const auto next = word < count ? origin(region.guestAddress + word * sizeof(std::uint32_t)) : std::int8_t {-2};
            if (next == current) continue;
            AppendSharedWords(variant.shared, variant.runs.size(), offset + start, region.bytes.subspan(start * sizeof(std::uint32_t), (word - start) * sizeof(std::uint32_t)), supplied);
            variant.runs.emplace_back(region.guestAddress + start * sizeof(std::uint32_t), region.guestAddress + word * sizeof(std::uint32_t));
            variant.runBases.push_back(current);
            start = word;
            current = next;
        }
    }
    variant.runParents.resize(variant.runs.size());
    if (!eligible || relocatable == 0) return;
    auto nested = use.nested;
    std::sort(nested.begin(), nested.end());
    nested.erase(std::unique(nested.begin(), nested.end()), nested.end());
    const auto pointerWord = [&](std::uint64_t address) {
        const auto found = std::lower_bound(nested.begin(), nested.end(), std::make_pair(address, std::uint64_t {0}));
        if (found == nested.end() || found->first != address || found->second == 0 || (found + 1 != nested.end() && found[1].first == address)) return std::uint64_t {0};
        return found->second;
    };
    for (std::size_t run = 0; run < variant.runs.size(); ++run) {
        if (variant.runBases[run] != -1) continue;
        const auto& [begin, end] = variant.runs[run];
        const auto pointer = pointerWord(begin);
        if (pointer == 0) continue;
        bool same = true;
        for (auto address = begin; address < end; address += sizeof(std::uint32_t)) same &= pointerWord(address) == pointer;
        if (!same) continue;
        for (std::size_t parent = 0; parent < variant.runs.size(); ++parent) {
            const auto& range = variant.runs[parent];
            if (parent != run && pointer >= range.first && pointer + sizeof(std::uint64_t) <= range.second && (pointer - range.first) % sizeof(std::uint32_t) == 0) {
                variant.runParents[run] = {static_cast<std::uint32_t>(parent), static_cast<std::uint32_t>((pointer - range.first) / sizeof(std::uint32_t))};
                break;
            }
        }
    }
}

namespace {

template <typename TMap>
std::size_t dropEveryOther(TMap& map) {
    std::size_t dropped = 0;
    bool drop = false;
    for (auto it = map.begin(); it != map.end();) {
        drop = !drop;
        if (!drop) {
            ++it;
            continue;
        }
        it = map.erase(it);
        ++dropped;
    }
    return dropped;
}

}

void Driver::tagNestedPointerPositions(DispatchVariant& variant, const ShaderRecompiler::ResourceCapture& capture) {
    variant.nestedPointerProof = false;
    variant.nestedPointerPatches.clear();
    const auto refuse = [&] {
        variant.runParents.assign(variant.runs.size(), {});
        variant.nestedPointerPatches.clear();
        ArchB::Recorder::Count("fix_draw_reloc_nested_refused_provenance");
    };
    if (variant.runParents.size() != variant.runs.size()) return refuse();
    if (std::none_of(variant.runParents.begin(), variant.runParents.end(), [](const auto& parent) { return parent.run != DispatchVariant::NoFlatBinding; })) {
        variant.nestedPointerProof = true;
        return;
    }
    if (capture.plan == nullptr || variant.compiled == nullptr || !capture.readTrace.profile) return refuse();
    const auto& plan = *capture.plan;
    if (!plan.srtPlanComplete || !plan.resourceTrackingComplete || plan.requiresSpecializationMemory || !capture.snapshot.tables.tables.empty() || !capture.snapshot.srtPoison.empty() || capture.snapshot.nullRootReads != 0 || plan.uniformFill.fill.kind != ShaderRecompiler::UniformFillKind::None || plan.shaderFlatSlots.size() != plan.srtReads.size()) return refuse();
    struct AddressesTag {};
    struct ReadersTag {};
    struct ForbiddenTag {};
    struct VisitedTag {};
    auto& addresses = AgcDriver::ThreadScratch<std::vector<std::pair<const ShaderRecompiler::IrValue*, std::uint64_t>>, AddressesTag>();
    auto& readers = AgcDriver::ThreadScratch<std::vector<std::pair<std::uint64_t, const ShaderRecompiler::IrValue*>>, ReadersTag>();
    auto& forbidden = AgcDriver::ThreadScratch<PointerSet, ForbiddenTag>();
    auto& visited = AgcDriver::ThreadScratch<PointerSet, VisitedTag>();
    addresses.clear();
    readers.clear();
    forbidden.clear();
    visited.clear();
    for (const auto& [raw, address] : capture.readTrace.slotReads) {
        if (raw == nullptr) continue;
        addresses.emplace_back(raw->Resolve(), address);
        readers.emplace_back(address, raw->Resolve());
    }
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    std::sort(readers.begin(), readers.end());
    readers.erase(std::unique(readers.begin(), readers.end()), readers.end());
    const auto capturedBy = [&](const ShaderRecompiler::IrValue* value) {
        return std::equal_range(addresses.begin(), addresses.end(), std::make_pair(value, std::uint64_t{0}), [](const auto& left, const auto& right) { return std::less<const ShaderRecompiler::IrValue*>{}(left.first, right.first); });
    };
    const auto readersOf = [&](std::uint64_t address) {
        return std::equal_range(readers.begin(), readers.end(), std::make_pair(address, static_cast<const ShaderRecompiler::IrValue*>(nullptr)), [](const auto& left, const auto& right) { return left.first < right.first; });
    };
    const auto source = [&](const ShaderRecompiler::IrValue* value) {
        std::uint32_t mask = 0xffffffffu;
        for (std::size_t depth = 0; value != nullptr && depth < 64; ++depth) {
            value = value->Resolve();
            if (ShaderRecompiler::Detail::IsRawRead(plan, *value)) return std::make_pair(value, mask);
            if ((value->Opcode() == ShaderRecompiler::IrOpcode::BitCastU32F32 || value->Opcode() == ShaderRecompiler::IrOpcode::BitCastF32U32) && value->ArgumentCount() == 1) value = value->Argument(0);
            else if (value->Opcode() == ShaderRecompiler::IrOpcode::ReadConst && value->ArgumentCount() == 2) {
                const auto* slot = value->Argument(1)->Resolve();
                if (!slot->HasImmediate() || slot->Type() != ShaderRecompiler::IrType::U32 || slot->ImmediateU32() >= plan.srtReads.size()) break;
                value = plan.srtReads[slot->ImmediateU32()].value;
            } else if (value->Opcode() == ShaderRecompiler::IrOpcode::BitwiseAnd32 && value->ArgumentCount() == 2 && value->Argument(1)->Resolve()->HasImmediate()) {
                mask &= value->Argument(1)->Resolve()->ImmediateU32();
                value = value->Argument(0);
            } else break;
        }
        return std::make_pair(static_cast<const ShaderRecompiler::IrValue*>(nullptr), mask);
    };
    const auto markValue = [&](auto&& self, const ShaderRecompiler::IrValue* value) -> void {
        if (value == nullptr) return;
        value = value->Resolve();
        if (!visited.insert(value)) return;
        if (ShaderRecompiler::Detail::IsRawRead(plan, *value)) {
            forbidden.insert(value);
            return;
        }
        if (value->Opcode() == ShaderRecompiler::IrOpcode::ReadConst && value->ArgumentCount() == 2) {
            const auto* slot = value->Argument(1)->Resolve();
            if (slot->HasImmediate() && slot->Type() == ShaderRecompiler::IrType::U32 && slot->ImmediateU32() < plan.srtReads.size()) self(self, plan.srtReads[slot->ImmediateU32()].value);
            return;
        }
        for (const auto* argument : value->Arguments()) self(self, argument);
    };
    for (const auto& descriptor : plan.descriptorSources) for (std::size_t word = 0; word < std::min<std::size_t>(descriptor.dwordCount, descriptor.dwords.size()); ++word) markValue(markValue, descriptor.dwords[word]);
    for (const auto& block : plan.controlFlow) markValue(markValue, block.condition);
    for (std::size_t slot = 0; slot < plan.srtReads.size(); ++slot) {
        if (plan.shaderFlatSlots[slot] != 0 || (slot < plan.cleanFlatSlots.size() && plan.cleanFlatSlots[slot] != 0) || source(plan.srtReads[slot].value).first == nullptr) markValue(markValue, plan.srtReads[slot].value);
    }
    const auto addressOf = [&](const ShaderRecompiler::IrValue* value) -> std::optional<std::uint64_t> {
        const auto found = capturedBy(value);
        if (found.second - found.first != 1) return {};
        return found.first->second;
    };
    for (auto group = addresses.begin(); group != addresses.end();) {
        const auto* raw = group->first;
        const auto captured = capturedBy(raw);
        group = captured.second;
        if (!ShaderRecompiler::Detail::IsRawRead(plan, *raw) || raw->ArgumentCount() < 2) continue;
        const auto* handle = raw->Argument(0)->Resolve();
        if (handle->ArgumentCount() < 2) return refuse();
        const auto low = source(handle->Argument(0));
        const auto high = source(handle->Argument(1));
        const auto lowAddress = addressOf(low.first), highAddress = addressOf(high.first);
        const bool pointer = raw->Opcode() == ShaderRecompiler::IrOpcode::LoadAddressU32 && lowAddress && highAddress && *highAddress == *lowAddress + 4u && (low.second | 3u) == 0xffffffffu && (high.second & 0xffffu) == 0xffffu;
        bool covered = pointer;
        if (pointer) {
            for (auto entry = captured.first; entry != captured.second; ++entry) {
                const auto address = entry->second;
                bool child = false;
                for (std::size_t run = 0; run < variant.runs.size(); ++run) {
                    const auto& parent = variant.runParents[run];
                    if (parent.run >= variant.runs.size()) continue;
                    child |= address >= variant.runs[run].first && address + 4u <= variant.runs[run].second && variant.runs[parent.run].first + parent.word * 4u == *lowAddress;
                }
                covered &= child;
            }
        }
        if (!covered) {
            markValue(markValue, handle->Argument(0));
            markValue(markValue, handle->Argument(1));
        }
        for (std::size_t index = 2; index < handle->ArgumentCount(); ++index) markValue(markValue, handle->Argument(index));
        markValue(markValue, raw->Argument(1));
    }
    const auto offsets = runWordOffsets(variant);
    for (const auto& parent : variant.runParents) {
        if (parent.run == DispatchVariant::NoFlatBinding) continue;
        if (parent.run >= variant.runs.size()) return refuse();
        const auto address = variant.runs[parent.run].first + parent.word * 4u;
        const auto position = offsets[parent.run] + parent.word;
        for (std::uint32_t half = 0; half < 2; ++half) {
            const auto found = readersOf(address + half * 4u);
            if (found.second - found.first != 1 || position + half >= variant.words.size() || relocationMask(variant, position + half) != 0 || WordMask(variant.feedback.memory, position + half) != 0) return refuse();
            const auto* raw = found.first->second;
            if (forbidden.contains(raw) || !addressOf(raw)) return refuse();
            for (std::size_t slot = 0; slot < plan.srtReads.size(); ++slot) {
                const auto flatSource = source(plan.srtReads[slot].value);
                if (flatSource.first != raw) continue;
                for (std::size_t binding = 0; binding < variant.compiled->bindings.size(); ++binding) {
                    const auto& flat = variant.compiled->bindings[binding];
                    if (flat.role != ShaderRecompiler::DescriptorRole::FlattenedSrt) continue;
                    const auto at = plan.srtReads[slot].flatOffset;
                    if (at >= flat.guestDescriptor.size() || flat.guestDescriptor[at] != (variant.words[position + half] & flatSource.second)) return refuse();
                    variant.nestedPointerPatches.push_back({static_cast<std::uint32_t>(position + half), static_cast<std::uint32_t>(binding), at, flatSource.second});
                }
            }
        }
    }
    std::sort(variant.nestedPointerPatches.begin(), variant.nestedPointerPatches.end());
    variant.nestedPointerPatches.erase(std::unique(variant.nestedPointerPatches.begin(), variant.nestedPointerPatches.end()), variant.nestedPointerPatches.end());
    variant.nestedPointerProof = true;
}

void Driver::learnPendingRelocation() {
    const auto pending = drawRelocationPending;
    drawRelocationPending = {};
    if (pending.key == 0) return;
    const auto inserted = drawCache.find(pending.key);
    if (inserted == drawCache.end() || inserted->second->decode == nullptr) return;
    const auto& entry = *inserted->second;
    const auto& programs = entry.decode->programs;
    if (entry.stages.size() != programs.size()) return;
    const auto known = drawRelocations.find(pending.identity);
    auto shape = std::make_shared<DrawRelocation>();
    if (known != drawRelocations.end()) *shape = *known->second;
    bool changed = known == drawRelocations.end();
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (entry.stages[i].empty()) continue;
        const auto& variant = *entry.stages[i].front();
        const auto& program = programs[i];
        std::shared_ptr<const DispatchVariant> content;
        const auto learnedContent = [&] {
            if (content == nullptr) {
                auto model = std::make_shared<DispatchVariant>();
                model->runs = variant.runs;
                model->words = variant.words;
                model->runBases = variant.runBases;
                model->runParents = variant.runParents;
                model->dataPositions = variant.dataPositions;
                model->bufferBaseMasks = variant.bufferBaseMasks;
                model->nestedPointerProof = variant.nestedPointerProof;
                content = std::move(model);
            }
            return content;
        };
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size() && index < 64; ++index) {
            const auto reg = program.userDataBase + index - program.hiddenUserWords;
            const auto held = std::find_if(shape->words.begin(), shape->words.end(), [&](const RelocatableUserWord& word) { return word.reg == reg; });
            const bool relocatable = (variant.relocatableWords >> index & 1u) != 0;
            const bool value = !relocatable && (variant.valueWords >> index & 1u) != 0;
            if (!relocatable && !value && (variant.calleeWords >> index & 1u) == 0) {
                if (held == shape->words.end()) continue;
                shape->words.erase(held);
                shape->banned.push_back(reg);
                changed = true;
                continue;
            }
            if (held != shape->words.end()) {
                if (pending.nested && variant.nestedPointerProof && (held->nestedVariant == nullptr || !held->nestedVariant->nestedPointerProof)) {
                    held->nestedVariant = learnedContent();
                    held->nestedUserWord = index;
                    held->nestedBase = AlignedWord(program.userData[index]);
                    changed = true;
                }
                continue;
            }
            if (std::find(shape->banned.begin(), shape->banned.end(), reg) != shape->banned.end()) continue;
            RelocatableUserWord word{reg, {}, value};
            if (pending.nested || std::getenv("APS5_DRAW_RELOC_NESTED_CENSUS") != nullptr) {
                word.nestedVariant = learnedContent();
                word.nestedUserWord = index;
                word.nestedBase = AlignedWord(program.userData[index]);
            }
            for (const auto& [callee, length] : variant.calleeReads) {
                if (callee != index) continue;
                word.reads.emplace_back(0, length);
                word.high = program.userData[index + 1];
            }
            const auto base = AlignedWord(program.userData[index]);
            for (std::size_t run = 0; run < variant.runs.size() && run < variant.runBases.size(); ++run) {
                if (variant.runBases[run] != static_cast<std::int8_t>(index)) continue;
                word.reads.emplace_back(static_cast<std::int64_t>(variant.runs[run].first - base), static_cast<std::uint32_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t)));
            }
            shape->words.push_back(std::move(word));
            changed = true;
        }
    }
    if (changed) {
        std::sort(shape->words.begin(), shape->words.end(), [](const RelocatableUserWord& a, const RelocatableUserWord& b) { return a.reg < b.reg; });
        drawRelocations[pending.identity] = std::move(shape);
    }
    if (pending.censusAlias != 0) {
        if (nestedCensusAliases.size() >= MaxAliases) dropEveryOther(nestedCensusAliases);
        nestedCensusAliases[pending.censusAlias] = pending.key;
    }
    if (pending.alias == 0) return;
    auto& aliases = pending.nested ? nestedDrawAliases : drawAliases;
    if (aliases.size() >= MaxAliases) drawRelocationCounters.evicted += dropEveryOther(aliases);
    aliases[pending.alias] = pending.key;
}

const char* Driver::moveNestedRuns(DispatchVariant& copy) {
    if (copy.runParents.size() != copy.runs.size()) return nullptr;
    if (std::none_of(copy.runParents.begin(), copy.runParents.end(), [](const auto& parent) { return parent.run != DispatchVariant::NoFlatBinding; })) return nullptr;
    const auto offsets = runWordOffsets(copy);
    const auto original = copy.words;
    std::vector<std::optional<std::uint64_t>> pointers(original.size());
    std::vector<std::uint8_t> states(copy.runs.size());
    const auto visit = [&](auto&& self, std::size_t run) -> const char* {
        if (states[run] == 2) return nullptr;
        if (states[run] == 1) return "content_differs_elsewhere";
        states[run] = 1;
        const auto& parent = copy.runParents[run];
        if (parent.run != DispatchVariant::NoFlatBinding) {
            if (parent.run >= copy.runs.size()) return "content_differs_elsewhere";
            if (const auto* reason = self(self, parent.run)) return reason;
            const auto position = offsets[parent.run] + parent.word;
            if (position + 1 >= original.size()) return "content_differs_elsewhere";
            std::uint64_t live = 0, old = 0;
            std::memcpy(&old, original.data() + position, sizeof(old));
            if (pointers[position]) live = *pointers[position];
            else {
                if (GuestMemory::CopyMapped(copy.runs[parent.run].first + parent.word * sizeof(std::uint32_t), std::as_writable_bytes(std::span(&live, 1)), true) != GuestMemory::Compare::Equal) return "pointer_unreadable";
                pointers[position] = live;
            }
            if (live != old) {
                if (live == 0 || old == 0 || ((live ^ old) & 3u) != 0) return "alignment_changed";
                const auto delta = static_cast<std::int64_t>(live & 0x0000fffffffffffcull) - static_cast<std::int64_t>(old & 0x0000fffffffffffcull);
                const auto begin = static_cast<std::int64_t>(copy.runs[run].first) + delta;
                const auto end = static_cast<std::int64_t>(copy.runs[run].second) + delta;
                if (begin < 0 || end < begin || end > 0x0001000000000000ll) return "pointer_unreadable";
                copy.runs[run] = {static_cast<std::uint64_t>(begin), static_cast<std::uint64_t>(end)};
                if (!GuestMemory::Accessible(reinterpret_cast<const void*>(copy.runs[run].first), static_cast<std::size_t>(end - begin))) return "pointer_unreadable";
                std::memcpy(copy.words.data() + position, &live, sizeof(live));
                copy.nestedRelocated = true;
            }
        }
        states[run] = 2;
        return nullptr;
    };
    for (std::size_t run = 0; run < copy.runs.size(); ++run) if (const auto* reason = visit(visit, run)) return reason;
    if (copy.nestedRelocated) {
        auto ranges = copy.runs;
        std::sort(ranges.begin(), ranges.end());
        for (std::size_t run = 1; run < ranges.size(); ++run) if (ranges[run - 1].second > ranges[run].first) return "overlap";
    }
    return nullptr;
}

void Driver::censusNestedDraw(const QueueState& queue, std::uint64_t alias) {
    ArchB::Recorder::Count("draw_nested_census_lookups");
    const auto found = nestedCensusAliases.find(alias);
    const auto cached = found != nestedCensusAliases.end() ? drawCache.find(found->second) : drawCache.end();
    if (alias == 0 || cached == drawCache.end() || cached->second->decode == nullptr) {
        ArchB::Recorder::Count("draw_nested_census_no_donor");
        return;
    }
    const auto& donor = *cached->second;
    ArchB::Recorder::Count("draw_nested_census_donors");
    const auto& programs = donor.decode->programs;
    if (donor.stages.size() != programs.size()) {
        ArchB::Recorder::Count("draw_nested_census_no_donor");
        return;
    }
    for (std::size_t stage = 0; stage < programs.size(); ++stage) {
        const auto& program = programs[stage];
        const char* firstReason = nullptr;
        bool matched = false;
        for (const auto& variant : donor.stages[stage]) {
            if (variant == nullptr || variant->compiled == nullptr) continue;
            ArchB::Recorder::Count("draw_nested_census_parent_runs", std::count_if(variant->runParents.begin(), variant->runParents.end(), [](const auto& parent) { return parent.run != DispatchVariant::NoFlatBinding; }));
            ArchB::Recorder::Count("draw_nested_census_buffer_base_words", variant->bufferBaseMasks.size());
            DispatchVariant copy;
            copy.runs = variant->runs;
            copy.runParents = variant->runParents;
            copy.words = variant->words;
            const char* reason = nullptr;
            for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
                const auto from = program.userData[index];
                const auto to = readUserData(queue.shader, program.userDataBase + index - program.hiddenUserWords);
                if (from == to) continue;
                if (index >= 64 || (((variant->relocatableWords | variant->calleeWords | variant->valueWords) >> index) & 1u) == 0) {
                    reason = "content_differs_elsewhere";
                    break;
                }
                if (((variant->valueWords >> index) & 1u) == 0 && (from == 0 || to == 0 || ((from ^ to) & 3u) != 0)) {
                    reason = "alignment_changed";
                    break;
                }
                const auto delta = AlignedWord(to) - AlignedWord(from);
                for (std::size_t run = 0; run < copy.runs.size() && run < variant->runBases.size(); ++run) {
                    if (variant->runBases[run] != static_cast<std::int8_t>(index)) continue;
                    copy.runs[run].first += delta;
                    copy.runs[run].second += delta;
                }
            }
            if (reason == nullptr) reason = moveNestedRuns(copy);
            std::size_t position = 0;
            for (const auto& [begin, end] : copy.runs) {
                if (reason != nullptr) break;
                std::vector<std::uint32_t> live((end - begin) / sizeof(std::uint32_t));
                if (GuestMemory::CopyMapped(begin, std::as_writable_bytes(std::span(live)), true) != GuestMemory::Compare::Equal) {
                    reason = "pointer_unreadable";
                    break;
                }
                for (const auto value : live) {
                    if (((value ^ copy.words[position]) & ~relocationMask(*variant, position)) != 0) {
                        reason = "content_differs_elsewhere";
                        break;
                    }
                    ++position;
                }
            }
            if (reason == nullptr) {
                matched = true;
                break;
            }
            if (firstReason == nullptr) firstReason = reason;
        }
        if (!matched) {
            if (firstReason == nullptr) ArchB::Recorder::Count("draw_nested_census_no_donor");
            else if (std::strcmp(firstReason, "pointer_unreadable") == 0) ArchB::Recorder::Count("draw_nested_census_pointer_unreadable");
            else if (std::strcmp(firstReason, "alignment_changed") == 0) ArchB::Recorder::Count("draw_nested_census_alignment_changed");
            else ArchB::Recorder::Count("draw_nested_census_content_differs_elsewhere");
            return;
        }
    }
    ArchB::Recorder::Count("draw_nested_census_would_hit");
}

void Driver::relocateDrawEntry(const QueueState& queue, std::uint64_t alias, std::shared_ptr<DrawEntry>& entry, std::shared_ptr<const DrawDecode>& decode, std::vector<DrawUserPatch>& patches, bool nested) {
    auto& counters = drawRelocationCounters;
    ++counters.lookups;
    auto& aliases = nested ? nestedDrawAliases : drawAliases;
    const auto found = aliases.find(alias);
    if (found == aliases.end()) {
        ArchB::Recorder::Count("draw_reloc_alias_unknown");
        if (nested) ArchB::Recorder::Count("fix_draw_reloc_nested_refused_no_donor");
        return;
    }
    const auto cached = drawCache.find(found->second);
    if (cached == drawCache.end()) {
        ArchB::Recorder::Count("draw_reloc_donor_evicted");
        if (nested) ArchB::Recorder::Count("fix_draw_reloc_nested_refused_donor_evicted");
        aliases.erase(found);
        return;
    }
    if (cached->second->decode == nullptr) return;
    const auto donor = cached->second;
    const auto& programs = donor->decode->programs;
    if (donor->stages.size() != programs.size()) return;
    struct Move {
        std::size_t program;
        std::uint32_t index, from, to;
    };
    std::vector<Move> moves;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
            const auto current = readUserData(queue.shader, program.userDataBase + index - program.hiddenUserWords);
            if (current != program.userData[index]) moves.push_back({i, index, program.userData[index], current});
        }
    }
    if (moves.empty()) {
        ArchB::Recorder::Count("draw_reloc_no_moves");
        return;
    }
    ++counters.donors;
    ArchB::Recorder::Count("draw_reloc_donors");
    auto relocated = std::make_shared<DrawEntry>();
    relocated->decode = donor->decode;
    relocated->donor = donor->donor != nullptr ? donor->donor : donor;
    relocated->stages.resize(donor->stages.size());
    for (std::size_t i = 0; i < donor->stages.size(); ++i) {
        const bool moved = std::any_of(moves.begin(), moves.end(), [&](const Move& move) { return move.program == i; });
        if (!moved && !nested) {
            relocated->stages[i] = donor->stages[i];
            continue;
        }
        for (const auto& variant : donor->stages[i]) {
            if (variant == nullptr || variant->compiled == nullptr) continue;
            const bool allowed = std::all_of(moves.begin(), moves.end(), [&](const Move& move) { return move.program != i || (move.index < 64 && move.from != 0 && move.to != 0 && ((((variant->relocatableWords | variant->calleeWords) >> move.index & 1u) != 0 && ((move.from ^ move.to) & 3u) == 0) || (variant->valueWords >> move.index & 1u) != 0)); });
            if (!allowed) {
                ArchB::Recorder::Count("draw_reloc_refused");
                if (nested) ArchB::Recorder::Count("fix_draw_reloc_nested_refused_user_words");
                ++counters.refused;
                continue;
            }
            auto copy = std::make_shared<DispatchVariant>();
            copy->runs = variant->runs;
            copy->runBases = variant->runBases;
            copy->runParents = variant->runParents;
            copy->nestedPointerPatches = variant->nestedPointerPatches;
            copy->nestedPointerProof = variant->nestedPointerProof;
            copy->relocatableWords = variant->relocatableWords;
            copy->userWordsWalked = variant->userWordsWalked;
            copy->valueWords = variant->valueWords;
            copy->valueSlots = variant->valueSlots;
            copy->calleeWords = variant->calleeWords;
            copy->calleeReads = variant->calleeReads;
            copy->donor = variant->donor != nullptr ? variant->donor : variant;
            copy->words = variant->words;
            copy->forgetSerial = variant->forgetSerial;
            copy->compiled = variant->compiled;
            copy->registeredViews = variant->registeredViews;
            copy->meshIndexDescriptor = variant->meshIndexDescriptor;
            copy->shader = variant->shader;
            copy->dataPositions = variant->dataPositions;
            copy->dataSlots = variant->dataSlots;
            if (nested) {
                copy->bufferBaseMasks = variant->bufferBaseMasks;
                copy->bufferBasePatches = variant->bufferBasePatches;
                copy->feedback = variant->feedback;
                copy->userDataMask = variant->userDataMask;
                copy->userData = programs[i].userData;
                copy->indirectCapture = variant->indirectCapture;
            }
            copy->flatBinding = variant->flatBinding;
            copy->pushOffset = variant->pushOffset;
            copy->vertexInfo = variant->vertexInfo;
            for (const auto& move : moves) {
                if (move.program != i) continue;
                if (nested && move.index < copy->userData.size()) copy->userData[move.index] = move.to;
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
                for (const auto& [index, binding, word] : variant->valueSlots) {
                    if (index == move.index) copy->descriptorPatches.push_back({binding, word, move.to});
                }
            }
            if (nested) {
                if (!copy->nestedPointerProof && std::any_of(copy->runParents.begin(), copy->runParents.end(), [](const auto& parent) { return parent.run != DispatchVariant::NoFlatBinding; })) {
                    ArchB::Recorder::Count("fix_draw_reloc_nested_refused_provenance");
                    continue;
                }
                if (const auto* reason = moveNestedRuns(*copy)) {
                    if (std::strcmp(reason, "pointer_unreadable") == 0) ArchB::Recorder::Count("fix_draw_reloc_nested_refused_pointer_unreadable");
                    else if (std::strcmp(reason, "alignment_changed") == 0) ArchB::Recorder::Count("fix_draw_reloc_nested_refused_alignment_changed");
                    else if (std::strcmp(reason, "overlap") == 0) ArchB::Recorder::Count("fix_draw_reloc_nested_refused_overlap");
                    else ArchB::Recorder::Count("fix_draw_reloc_nested_refused_cycle");
                    continue;
                }
                for (const auto& [position, binding, word, mask] : copy->nestedPointerPatches) {
                    if ((copy->words[position] & mask) != (variant->words[position] & mask)) copy->descriptorPatches.push_back({binding, word, copy->words[position] & mask});
                }
            }
            relocated->stages[i].push_back(std::move(copy));
        }
    }
    patches.clear();
    for (const auto& move : moves) patches.push_back({move.program, move.index, move.to});
    entry = std::move(relocated);
    decode = donor->decode;
}

std::shared_ptr<const DrawRelocation> Driver::dispatchRelocationShape(const DispatchVariant& variant) {
    if (variant.relocatableWords == 0) return nullptr;
    auto shape = std::make_shared<DrawRelocation>();
    for (std::uint32_t index = 0; index < variant.userData.size() && index < 64; ++index) {
        if ((variant.relocatableWords >> index & 1u) == 0) continue;
        RelocatableUserWord word{index, {}};
        const auto base = AlignedWord(variant.userData[index]);
        for (std::size_t run = 0; run < variant.runs.size() && run < variant.runBases.size(); ++run) {
            if (variant.runBases[run] != static_cast<std::int8_t>(index)) continue;
            word.reads.emplace_back(static_cast<std::int64_t>(variant.runs[run].first - base), static_cast<std::uint32_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t)));
        }
        if (!word.reads.empty()) shape->words.push_back(std::move(word));
    }
    if (shape->words.empty()) return nullptr;
    return shape;
}

std::uint64_t Driver::dispatchAliasKey(std::uint64_t identity, std::span<const std::uint32_t> userData, const DrawRelocation& shape, bool& readable) {
    // The shape alias: a relocatable base contributes only its alignment, not its value or the
    // content behind it (ring-allocated constants move and change every frame); the donor's copy is
    // validated by value, so content that changed only at data positions is a data hit and anything
    // else a miss. (A content-keyed alias, as for draws, found ~0 donors live: 0.06 hits/f.)
    std::uint64_t key = identity ^ 0x6a09e667f3bcc908ull;
    readable = true;
    for (std::uint32_t index = 0; index < userData.size(); ++index) {
        const bool relocatable = std::any_of(shape.words.begin(), shape.words.end(), [&](const RelocatableUserWord& candidate) { return candidate.reg == index; });
        const std::uint64_t mixed = relocatable ? (0xa5a5a5a500000000ull | (userData[index] & 3u)) : userData[index];
        key = (key ^ mixed) * 0x100000001b3ull;
    }
    return key;
}

std::shared_ptr<DispatchEntry> Driver::relocateDispatchEntry(std::unique_lock<AgcDriver::Mutex>& cacheLock, std::uint64_t identity, std::span<const std::uint32_t> userData) {
    const auto known = dispatchRelocations.find(identity);
    if (known == dispatchRelocations.end()) return nullptr;
    const auto shape = known->second;
    ArchB::Recorder::Count("fix_dispatch_relocation_lookups");
    static_cast<void>(cacheLock);
    bool readable = true;
    const auto alias = dispatchAliasKey(identity, userData, *shape, readable);
    if (!readable) return nullptr;
    const auto found = dispatchAliases.find(alias);
    if (found == dispatchAliases.end()) return nullptr;
    const auto cached = dispatchCache.find(found->second);
    if (cached == dispatchCache.end()) {
        dispatchAliases.erase(found);
        return nullptr;
    }
    const auto donor = cached->second;
    auto relocated = std::make_shared<DispatchEntry>();
    for (const auto& variant : donor->variants) {
        if (variant == nullptr || variant->compiled == nullptr || variant->userData.size() != userData.size()) continue;
        struct Move {
            std::uint32_t index, from, to;
        };
        std::vector<Move> moves;
        for (std::uint32_t index = 0; index < userData.size(); ++index) {
            if (variant->userData[index] != userData[index]) moves.push_back({index, variant->userData[index], userData[index]});
        }
        const bool allowed = !moves.empty() && std::all_of(moves.begin(), moves.end(), [&](const Move& move) { return move.index < 64 && (variant->relocatableWords >> move.index & 1u) != 0 && move.from != 0 && move.to != 0 && ((move.from ^ move.to) & 3u) == 0; });
        if (!allowed) {
            ArchB::Recorder::Count("fix_dispatch_relocation_refused");
            continue;
        }
        auto copy = std::make_shared<DispatchVariant>();
        copy->runs = variant->runs;
        copy->runBases = variant->runBases;
        copy->runParents = variant->runParents;
        copy->relocatableWords = variant->relocatableWords;
        copy->words = variant->words;
        copy->forgetSerial = variant->forgetSerial;
        copy->compiled = variant->compiled;
        copy->registeredViews = variant->registeredViews;
        copy->meshIndexDescriptor = variant->meshIndexDescriptor;
        copy->shader = variant->shader;
        copy->dataPositions = variant->dataPositions;
        copy->dataSlots = variant->dataSlots;
        copy->flatBinding = variant->flatBinding;
        copy->feedback = variant->feedback;
        copy->bufferBaseMasks = variant->bufferBaseMasks;
        copy->bufferBasePatches = variant->bufferBasePatches;
        copy->pushOffset = variant->pushOffset;
        copy->userDataMask = variant->userDataMask;
        copy->userData.assign(userData.begin(), userData.end());
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
        relocated->variants.push_back(std::move(copy));
    }
    if (relocated->variants.empty()) return nullptr;
    ArchB::Recorder::Count("fix_dispatch_relocation_donors");
    return relocated;
}

void Driver::applyShaderDataPatches(DispatchVariant& variant) {
    if (variant.shaderDataPatches.empty() && variant.descriptorPatches.empty()) return;
    auto rebased = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    for (const auto& [slot, value] : variant.shaderDataPatches) patchForwardedWord(*rebased, slot, value);
    for (const auto& [binding, word, value] : variant.descriptorPatches) {
        if (binding < rebased->bindings.size() && word < rebased->bindings[binding].guestDescriptor.size()) rebased->bindings[binding].guestDescriptor[word] = value;
    }
    variant.compiled = std::move(rebased);
    variant.shaderDataPatches.clear();
    variant.descriptorPatches.clear();
}

void Driver::patchForwardedWord(ShaderRecompiler::RecompileResult& result, std::uint32_t slot, std::uint32_t value) {
    const auto at = static_cast<std::size_t>(slot) * sizeof(std::uint32_t);
    if (at + sizeof(std::uint32_t) <= result.pushConstants.size()) std::memcpy(result.pushConstants.data() + at, &value, sizeof(value));
    for (auto& binding : result.bindings) {
        if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && slot < binding.guestDescriptor.size()) binding.guestDescriptor[slot] = value;
    }
}

void Driver::reportDrawRelocation() {
    auto& counters = drawRelocationCounters;
    AgcDriver::ProfilePrint_nid_no_patch("[draw-relocate] %llu alias lookups: %llu donors with moved user words, %llu refusals, %llu hits through a relocated entry; %zu shader sets learned, %zu aliases held (%llu dropped at the limit)\n", static_cast<unsigned long long>(counters.lookups), static_cast<unsigned long long>(counters.donors), static_cast<unsigned long long>(counters.refused), static_cast<unsigned long long>(counters.hits), drawRelocations.size(), drawAliases.size() + nestedDrawAliases.size(), static_cast<unsigned long long>(counters.evicted));
    counters = {};
}

}
