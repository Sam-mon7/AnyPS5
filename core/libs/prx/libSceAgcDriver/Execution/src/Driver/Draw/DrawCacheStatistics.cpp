#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

namespace AgcDriver::DriverDetail {

const char* DrawMissDifference::Class() const {
    if (kinds & Layout) return "m1_miss_layout";
    if (kinds & Buffer) return "m1_miss_buffer";
    if (kinds & Data) return "m1_miss_data";
    if (kinds & Other) return "m1_miss_other";
    if ((kinds & (Image | Sampler)) == (Image | Sampler)) return kinds & (ImageFields | Specialized) ? "m1_miss_image_sampler_specialized" : "m1_miss_image_sampler_independent";
    if (kinds & Image) return kinds & (ImageFields | Specialized) ? "m1_miss_image_fields" : "m1_miss_image_address";
    if (kinds & Sampler) return kinds & (ImageFields | Specialized) ? "m1_miss_sampler_specialized" : "m1_miss_sampler_independent";
    return "m1_miss_other";
}

DrawMissDifference CompareDrawMissStage(const DispatchVariant& old, const StageCapture& fresh, std::span<const Graphics::DecodeRead> reads) {
    using Difference = DrawMissDifference;
    DrawMissDifference result;
    const auto imageChange = [&](std::size_t word, std::uint32_t changed) {
        constexpr std::array<std::uint32_t, 8> addressMasks{0xffffffffu, 0xffu, 0, 0, 0, 0, 0xff000000u, 0xffffffffu};
        result.imageChanges[word] |= changed;
        if ((changed & ~addressMasks[word]) != 0) result.kinds |= Difference::ImageFields;
    };
    const auto layout = [&] { result.kinds |= Difference::Layout; ++result.layout; };
    if (old.censusCapture == nullptr || fresh.capture == nullptr || old.compiled == nullptr || fresh.compiled == nullptr) {
        result.kinds = Difference::Other;
        result.layout = 1;
        return result;
    }
    const auto& before = *old.censusCapture;
    const auto& after = *fresh.capture;
    if (before.plan != after.plan || old.pushOffset != fresh.pushOffset) layout();
    const auto descriptors = [&](const auto& a, const auto& b, std::uint32_t kind) {
        if (a.size() != b.size()) layout();
        for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
            if (a[i] == b[i]) continue;
            if (a[i].dwordCount != b[i].dwordCount || a[i].availableDwordMask != b[i].availableDwordMask) layout();
            if (kind != Difference::Image) continue;
            for (std::size_t w = 0; w < a[i].dwords.size(); ++w) {
                imageChange(w, a[i].dwords[w] ^ b[i].dwords[w]);
            }
        }
    };
    descriptors(before.snapshot.images, after.snapshot.images, Difference::Image);
    descriptors(before.snapshot.samplers, after.snapshot.samplers, Difference::Sampler);
    descriptors(before.snapshot.buffers, after.snapshot.buffers, Difference::Buffer);
    if (before.snapshot.tables != after.snapshot.tables || before.snapshot.srtPoison != after.snapshot.srtPoison || before.snapshot.nullRootReads != after.snapshot.nullRootReads) layout();
    if (before.snapshot.userData.size() != after.snapshot.userData.size()) layout();
    const auto& a = *old.compiled;
    const auto& b = *fresh.compiled;
    if (a.vertexAttributes.size() != b.vertexAttributes.size()) layout();
    for (std::size_t i = 0; i < std::min(a.vertexAttributes.size(), b.vertexAttributes.size()); ++i) {
        if (a.vertexAttributes[i].resource.fields != b.vertexAttributes[i].resource.fields) result.kinds |= Difference::Buffer;
    }
    if (a.spirv.ContentHash() != b.spirv.ContentHash() || a.specialization != b.specialization) result.kinds |= Difference::Specialized;
    const auto sameBinding = [](const auto& x, const auto& y) {
        return x.kind == y.kind && x.role == y.role && x.descriptorSet == y.descriptorSet && x.binding == y.binding && x.count == y.count;
    };
    if (a.bindings.size() != b.bindings.size() || !std::equal(a.bindings.begin(), a.bindings.end(), b.bindings.begin(), sameBinding)) layout();
    for (std::size_t i = 0; i < std::min(a.bindings.size(), b.bindings.size()); ++i) {
        const auto& x = a.bindings[i];
        const auto& y = b.bindings[i];
        if (x.imageShape != y.imageShape || x.imageShapes != y.imageShapes || x.imageSamplers != y.imageSamplers || x.Flags() != y.Flags()) result.kinds |= Difference::Specialized;
    }
    std::map<std::uint64_t, std::uint32_t> was, now;
    std::set<std::uint64_t> refreshable;
    std::size_t word = 0;
    for (const auto& [begin, end] : old.runs) {
        for (auto address = begin; address + 4 <= end && word < old.words.size(); address += 4) {
            if (std::binary_search(old.dataPositions.begin(), old.dataPositions.end(), word)) refreshable.insert(address);
            was[address] = old.words[word++];
        }
    }
    const auto append = [&](std::uint64_t address, std::span<const std::byte> bytes) {
        for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4) {
            std::uint32_t value;
            std::memcpy(&value, bytes.data() + offset, 4);
            now[address + offset] = value;
        }
    };
    for (const auto& region : fresh.regions) append(region.guestAddress, region.bytes);
    for (const auto& read : reads) append(read.address, read.Bytes());
    std::map<std::uint64_t, std::uint32_t> roles;
    std::map<std::size_t, std::uint32_t> userRoles;
    std::map<std::uint64_t, std::uint8_t> imageWords;
    std::map<std::size_t, std::uint8_t> userImageWords;
    std::set<std::uint64_t> observed, pointers;
    const auto mark = [&](const ShaderRecompiler::ResourceCapture& capture) {
        if (capture.plan == nullptr) return;
        std::map<const ShaderRecompiler::IrValue*, std::vector<std::uint64_t>> addresses;
        for (const auto& [value, address] : capture.readTrace.slotReads) {
            addresses[value].push_back(address);
            observed.insert(address);
        }
        const auto sourceValue = [&](const ShaderRecompiler::IrValue* value) {
            std::set<const ShaderRecompiler::IrValue*> seen;
            while (value != nullptr) {
                value = value->Resolve();
                if (!seen.insert(value).second) return static_cast<const ShaderRecompiler::IrValue*>(nullptr);
                if (value->Opcode() == ShaderRecompiler::IrOpcode::ReadConst && value->ArgumentCount() >= 2) {
                    const auto* slot = value->Argument(1)->Resolve();
                    if (slot->HasImmediate() && slot->Type() == ShaderRecompiler::IrType::U32 && slot->ImmediateU32() < capture.plan->srtReads.size()) {
                        value = capture.plan->srtReads[slot->ImmediateU32()].value;
                        continue;
                    }
                }
                if ((value->Opcode() == ShaderRecompiler::IrOpcode::BitCastU32F32 || value->Opcode() == ShaderRecompiler::IrOpcode::BitCastF32U32) && value->ArgumentCount() != 0) {
                    value = value->Argument(0);
                    continue;
                }
                return value;
            }
            return value;
        };
        const auto sources = [&](const auto& resources, std::uint32_t kind) {
            for (const auto& resource : resources) {
                if constexpr (requires { resource.imageAlias; }) {
                    if (resource.imageAlias != ShaderRecompiler::BufferResource::NoImageAlias) continue;
                }
                if (resource.source >= capture.plan->descriptorSources.size()) continue;
                const auto& source = capture.plan->descriptorSources[resource.source];
                for (std::size_t w = 0; w < source.dwordCount; ++w) {
                    const auto* value = sourceValue(source.dwords[w]);
                    if (value == nullptr) continue;
                    if (value->Opcode() == ShaderRecompiler::IrOpcode::GetUserData && value->ArgumentCount() != 0) {
                        const auto reg = value->Argument(0)->Register().index;
                        if (reg >= capture.plan->userDataBase) {
                            userRoles[reg - capture.plan->userDataBase] |= kind;
                            if (kind == Difference::Image) userImageWords[reg - capture.plan->userDataBase] |= static_cast<std::uint8_t>(1u << w);
                        }
                    }
                    if (const auto found = addresses.find(value); found != addresses.end()) {
                        for (const auto address : found->second) {
                            roles[address] |= kind;
                            if (kind == Difference::Image) imageWords[address] |= static_cast<std::uint8_t>(1u << w);
                        }
                    }
                }
            }
        };
        sources(capture.plan->info.images, Difference::Image);
        sources(capture.plan->info.samplers, Difference::Sampler);
        sources(capture.plan->info.buffers, Difference::Buffer);
        std::set<const ShaderRecompiler::IrValue*> visited;
        const auto pointer = [&](auto&& self, const ShaderRecompiler::IrValue* value) -> void {
            value = sourceValue(value);
            if (value == nullptr) return;
            if (!visited.insert(value).second) return;
            if (const auto found = addresses.find(value); found != addresses.end()) {
                pointers.insert(found->second.begin(), found->second.end());
            }
            for (const auto* argument : value->Arguments()) self(self, argument);
        };
        for (const auto& [value, address] : capture.readTrace.slotReads) {
            for (const auto* argument : value->Arguments()) pointer(pointer, argument);
        }
    };
    mark(before);
    mark(after);
    for (const auto address : observed) if (!roles.contains(address)) roles[address] = pointers.contains(address) ? Difference::Layout : Difference::Data;
    for (std::size_t w = 0; w < std::min(before.snapshot.userData.size(), after.snapshot.userData.size()); ++w) {
        if (before.snapshot.userData[w] == after.snapshot.userData[w]) continue;
        ++result.words;
        if (const auto found = userImageWords.find(w); found != userImageWords.end()) {
            for (std::size_t i = 0; i < 8; ++i) if (found->second & (1u << i)) imageChange(i, before.snapshot.userData[w] ^ after.snapshot.userData[w]);
        }
        if (w < 64 && ((before.userDataUse.asBase | after.userDataUse.asBase) & (std::uint64_t{1} << w))) layout();
        else if (const auto role = userRoles.find(w); role != userRoles.end()) result.kinds |= role->second;
        else { result.kinds |= Difference::Data; ++result.userDataWords; }
    }
    for (const auto& [slot, address] : after.readTrace.leaves) if (!roles.contains(address)) roles[address] = Difference::Data;
    for (const auto& [address, value] : now) {
        const auto found = was.find(address);
        if (found == was.end()) { layout(); continue; }
        if (found->second == value) continue;
        ++result.words;
        if (const auto image = imageWords.find(address); image != imageWords.end()) {
            for (std::size_t i = 0; i < 8; ++i) if (image->second & (1u << i)) imageChange(i, found->second ^ value);
        }
        const auto role = roles.find(address);
        result.kinds |= role == roles.end() ? Difference::Other : role->second;
        if (role != roles.end() && (role->second & Difference::Layout)) ++result.layout;
        if (role != roles.end() && (role->second & Difference::Data)) {
            ++result.dataWords;
            if (refreshable.contains(address)) ++result.refreshableDataWords;
        }
    }
    for (const auto& [address, value] : was) if (!now.contains(address)) layout();
    if ((result.kinds & Difference::Specialized) && !(result.kinds & (Difference::Image | Difference::Sampler | Difference::Buffer | Difference::Data))) layout();
    return result;
}

void Driver::censusDrawMiss(const std::vector<DrawProgram>& programs, const std::vector<StageCapture>& captures, const std::vector<std::vector<Graphics::DecodeRead>>& reads, const std::vector<bool>& reused, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::shared_ptr<DrawEntry>& entry, std::uint64_t shape) {
    if (!missCensus()) return;
    ArchB::Recorder::Count("m1_miss_total");
    std::vector<std::shared_ptr<DrawEntry>> candidates;
    {
        std::lock_guard lock(drawCacheMutex);
        for (const auto& [key, candidate] : drawCache) {
            if (candidate->decode == nullptr || candidate->decode->programs.size() != programs.size() || candidate->stages.size() != programs.size()) continue;
            bool same = true;
            for (std::size_t i = 0; same && i < programs.size(); ++i) {
                const auto& kept = candidate->decode->programs[i];
                same = kept.snapshot->CodeDigest() == programs[i].snapshot->CodeDigest() && kept.codeOffset == programs[i].codeOffset && kept.binary.stage == programs[i].binary.stage;
            }
            if (same) candidates.push_back(candidate);
        }
    }
    if (candidates.empty()) {
        ArchB::Recorder::Count("m1_miss_no_pair");
        return;
    }
    if (entry == nullptr) ArchB::Recorder::Count("m1_miss_absent_pair");
    const auto score = [](const DrawMissDifference& value) { return std::pair(value.layout, value.words); };
    DrawMissDifference closest;
    closest.layout = std::numeric_limits<std::size_t>::max();
    for (const auto& candidate : candidates) {
        DrawMissDifference difference;
        if (shape != 0 && candidate->shape != shape) { difference.kinds |= DrawMissDifference::Layout; ++difference.layout; }
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (candidate->decode->roles[i] == ShaderRecompiler::ProgramRole::GeometryBack) continue;
            StageCapture current = captures[i];
            std::span<const Graphics::DecodeRead> currentReads = reads[i];
            if (reused[i] && matched[i] != nullptr) {
                current.capture = matched[i]->censusCapture;
                current.compiled = matched[i]->compiled;
                current.pushOffset = matched[i]->pushOffset;
                appendEntryRegions(*matched[i], current.regions);
                currentReads = {};
            }
            DrawMissDifference best;
            best.layout = std::numeric_limits<std::size_t>::max();
            for (const auto& variant : candidate->stages[i]) {
                const auto change = CompareDrawMissStage(*variant, current, currentReads);
                if (score(change) < score(best)) best = change;
            }
            if (best.layout == std::numeric_limits<std::size_t>::max()) { best.kinds = DrawMissDifference::Layout; best.layout = 1; }
            difference.kinds |= best.kinds;
            difference.words += best.words;
            difference.layout += best.layout;
            difference.dataWords += best.dataWords;
            difference.refreshableDataWords += best.refreshableDataWords;
            difference.userDataWords += best.userDataWords;
            for (std::size_t w = 0; w < difference.imageChanges.size(); ++w) difference.imageChanges[w] |= best.imageChanges[w];
        }
        if (score(difference) < score(closest)) closest = difference;
    }
    ArchB::Recorder::Count(closest.Class());
    if (closest.kinds & DrawMissDifference::Image) ArchB::Recorder::Count("m1_diff_image");
    if (closest.kinds & DrawMissDifference::Sampler) ArchB::Recorder::Count("m1_diff_sampler");
    const auto& image = closest.imageChanges;
    if (image[0] || (image[1] & 0xffu) || (image[6] & 0xff000000u) || image[7]) ArchB::Recorder::Count("m1_diff_image_address");
    if (image[1] & 0x1ff00000u) ArchB::Recorder::Count("m1_diff_image_format");
    if ((image[1] & 0xc0000000u) || (image[2] & 0x0fffcfffu) || (image[4] & 0x1fffu)) ArchB::Recorder::Count("m1_diff_image_dimensions");
    if (image[3] & 0xfffu) ArchB::Recorder::Count("m1_diff_image_swizzle");
    if (image[3] & 0xf0000000u) ArchB::Recorder::Count("m1_diff_image_type");
    if ((image[3] & 0xff000u) || (image[4] & 0x1fff0000u) || (image[5] & 0xffu)) ArchB::Recorder::Count("m1_diff_image_mip_array");
    if ((closest.kinds & (DrawMissDifference::Image | DrawMissDifference::Data)) == (DrawMissDifference::Image | DrawMissDifference::Data)) ArchB::Recorder::Count("m1_diff_image_and_data");
    if ((closest.kinds & DrawMissDifference::Image) && std::all_of(image.begin(), image.end(), [](std::uint32_t bits) { return bits == 0; })) ArchB::Recorder::Count("m1_diff_image_no_bits");
    if (closest.kinds & DrawMissDifference::Image) {
        static auto* bitNames = new std::set<std::string>;
        static std::mutex bitNamesMutex;
        for (std::size_t w = 0; w < image.size(); ++w) {
            for (std::uint32_t b = 0; b < 32; ++b) {
                if ((image[w] & (1u << b)) == 0) continue;
                char bit[48];
                std::snprintf(bit, sizeof bit, "m1_tbit_w%zu_b%02u", w, b);
                const char* bitCounter;
                {
                    std::lock_guard lock(bitNamesMutex);
                    bitCounter = bitNames->emplace(bit).first->c_str();
                }
                ArchB::Recorder::Count(bitCounter);
            }
        }
    }
    if (closest.userDataWords != 0) ArchB::Recorder::Count("m1_diff_user_data");
    if (closest.dataWords != 0) ArchB::Recorder::Count("m1_diff_captured_data");
    if (closest.dataWords != 0 && closest.dataWords == closest.refreshableDataWords && closest.userDataWords == 0) {
        ArchB::Recorder::Count("m1_diff_refreshable_data");
        if (closest.kinds & DrawMissDifference::Image) ArchB::Recorder::Count("m1_diff_image_with_refreshable_data");
    }
    std::uint64_t code = 0xcbf29ce484222325ull ^ programs.size();
    for (const auto& program : programs) {
        code = (code ^ program.snapshot->CodeDigest()) * 0x100000001b3ull;
        code = (code ^ program.codeOffset) * 0x100000001b3ull;
        code = (code ^ static_cast<std::uint64_t>(program.binary.stage)) * 0x100000001b3ull;
    }
    char name[128];
    std::snprintf(name, sizeof name, "%s_program_%016llx", closest.Class(), static_cast<unsigned long long>(code));
    static auto* names = new std::set<std::string>;
    static std::mutex namesMutex;
    const char* counter;
    {
        std::lock_guard lock(namesMutex);
        counter = names->emplace(name).first->c_str();
    }
    ArchB::Recorder::Count(counter);
}

bool Driver::missCensus() {
    static const bool census = std::getenv("APS5_ARCHB_MISS_CENSUS") != nullptr;
    return census && ArchB::Recorder::Enabled();
}

bool Driver::traceDrawCache() {
    static const bool trace = std::getenv("APS5_TRACE_DRAW_CACHE") != nullptr;
    return trace;
}

void Driver::traceInsert(std::unordered_set<std::uint64_t>& set, std::uint64_t key) {
    if (set.size() >= (1u << 16u)) set.clear();
    set.insert(key);
}

void Driver::maybeReportDrawCache(bool profile) {
    auto& counters = drawEntryCounters;
    if (!profile && !traceDrawCache()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - counters.lastReport < std::chrono::seconds(10)) return;
    reportDrawCache(counters, profile);
    counters.lastReport = now;
}

void Driver::reportDrawCache(DrawEntryCounters& counters, bool profile) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto miss = [&](DrawMiss reason) { return count(counters.misses[static_cast<std::size_t>(reason)]); };
    std::string text;
    if (traceDrawCache()) {
        char line[1536];
        std::uint64_t misses = 0;
        for (const auto value : counters.misses) misses += value;
        std::snprintf(line, sizeof(line), "[draw-cache] 10 s: %llu lookups, %llu hits (%.1f%%), %llu stage misses, %llu without an entry, %llu data-only hits (stages: %llu reused, %llu compiled; %llu verified); no entry: new registers %llu, user words/pointers only %llu, evicted %llu, seen but never inserted %llu; not inserted: indirect with CPU arguments %llu, not admitted %llu; stage misses: front differing %llu, fragment differing %llu, other differing %llu, layout %llu, gate %llu, stage count %llu; differing captures: same runs %llu (%.1f words changed each), other runs %llu; %zu entries, %llu evicted\n", count(counters.lookups), count(counters.hits), counters.lookups != 0 ? 100.0 * static_cast<double>(counters.hits) / static_cast<double>(counters.lookups) : 0.0, count(misses), count(counters.absent), count(counters.dataHits), count(counters.dataStagesReused), count(counters.dataStagesCompiled), count(counters.dataVerified), count(counters.absentNewRegisters), count(counters.absentUserWords), count(counters.absentEvicted), count(counters.absentNeverInserted), count(counters.skippedIndirectCpu), count(counters.skippedNotAdmitted), miss(DrawMiss::FrontDiffering), miss(DrawMiss::FragmentDiffering), miss(DrawMiss::OtherDiffering), miss(DrawMiss::Layout), miss(DrawMiss::Gate), miss(DrawMiss::Stages), count(counters.differingSameRuns), counters.differingSameRuns != 0 ? static_cast<double>(counters.differingWords) / static_cast<double>(counters.differingSameRuns) : 0.0, count(counters.differingRunsChanged), drawCache.size(), count(counters.evictions));
        text += line;
        std::snprintf(line, sizeof(line), "[draw-cache] register-key miss component changes (10 s): stable registers %llu, user data %llu, auxiliary registers %llu, device %llu, first shader set %llu, unchanged components %llu\n", count(counters.registerKeyMissChanges[0]), count(counters.registerKeyMissChanges[1]), count(counters.registerKeyMissChanges[2]), count(counters.registerKeyMissChanges[3]), count(counters.registerKeyMissChanges[4]), count(counters.registerKeyMissChanges[5]));
        text += line;
    }
    if (profile) text += profileDrawCacheLines(counters);
    AgcDriver::ProfilePrint_nid_no_patch("%s", text.c_str());
    counters = DrawEntryCounters{};
}

DrawEntryCounters Driver::DrawCacheCounters() {
    std::lock_guard cacheLock(drawCacheMutex);
    return drawEntryCounters;
}

std::string Driver::profileDrawCacheLines(const DrawEntryCounters& counters) {
    std::string text;
    const auto append = [&](const char* format, auto... values) {
        char line[2048];
        std::snprintf(line, sizeof(line), format, values...);
        text += line;
    };
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto validated = counters.lookups > counters.absent ? counters.lookups - counters.absent : 0;
    const auto miss = [&](DrawMiss reason) { return count(counters.misses[static_cast<std::size_t>(reason)]); };
    std::string ranks;
    for (std::size_t rank = 0; rank < dispatchVariants(); ++rank) {
        char item[32];
        std::snprintf(item, sizeof(item), "%s%llu", rank == 0 ? "" : " / ", count(counters.variantHitsByRank[rank]));
        ranks += item;
    }
    append("[draw-cache] %llu lookups (10 s): %llu no entry, %llu validated in %.1f us each (key, lookup and every stage): %llu hits (every stage equal; stage hits by variant rank 1..k %s), misses by reason: front stage differing %llu, fragment differing %llu, other stage differing %llu, layout (push offset) %llu, gate %llu, stage count %llu; %llu stage validations (%llu equal, %.2f variants compared each); %llu inserts (%llu shape updates, %llu variants inserted, %llu evicted beyond k, %llu already present, %llu unstable), %zu entries (%llu variants, ~%.1f MiB), %llu evictions in total, %llu LRU moves; verify: %llu hits captured again, %llu stages differed\n", count(counters.lookups), count(counters.absent), count(validated), validated != 0 ? counters.validateUs / static_cast<double>(validated) : 0.0, count(counters.hits), ranks.c_str(), miss(DrawMiss::FrontDiffering), miss(DrawMiss::FragmentDiffering), miss(DrawMiss::OtherDiffering), miss(DrawMiss::Layout), miss(DrawMiss::Gate), miss(DrawMiss::Stages), count(counters.stageValidations), count(counters.stageEqual), counters.stageValidations != 0 ? static_cast<double>(counters.variantsCompared) / static_cast<double>(counters.stageValidations) : 0.0, count(counters.inserts), count(counters.shapeUpdates), count(counters.variantsInserted), count(counters.variantsEvicted), count(counters.present), count(counters.unstable), drawCache.size(), count(drawCacheVariants), static_cast<double>(drawCacheVariantBytes) / (1024.0 * 1024.0), count(drawCacheEvictions), count(counters.touches), count(counters.verifyHits), count(counters.verifyMismatches));
    append("[draw-cache] register key (10 s): %llu lookups, %llu hits, key build %.1f us each; decode skipped %llu, partial (state/pixel/programs from the entry) %llu; facade log != table %llu; verify: %llu decodes compared, %llu differed\n", count(counters.registerKeyLookups), count(counters.registerKeyHits), counters.registerKeyLookups != 0 ? counters.keyUs / static_cast<double>(counters.registerKeyLookups) : 0.0, count(counters.decodeSkipped), count(counters.decodePartial), count(counters.facadeMismatches), count(counters.verifyDecodes), count(counters.verifyDecodeMismatches));
    append("[draw-cache] stage data hits (10 s): %llu stages (%llu words refreshed), verified %llu; stage inserts with data positions %llu (%.1f positions each)\n", count(counters.stageDataHits), count(counters.stageDataWords), count(counters.stageDataVerified), count(counters.stageDataInserts), counters.stageDataInserts != 0 ? static_cast<double>(counters.stageDataPositions) / static_cast<double>(counters.stageDataInserts) : 0.0);
    append("[draw-cache] CPU indirect (10 s): %llu hits, %llu misses; refused:", count(counters.indirectCpuHits), count(counters.indirectCpuMisses));
    constexpr std::array<const char*, static_cast<std::size_t>(IndirectCacheRefusal::Count)> refusalNames{"none", "switch off", "entries disabled", "shader path", "no capture", "incomplete plan", "capture reads patched word", "vertex decode", "push mapping", "not admitted"};
    for (std::size_t i = 1; i < refusalNames.size(); ++i) append(" %s %llu%s", refusalNames[i], count(counters.indirectCpuRefused[i]), i + 1 == refusalNames.size() ? "\n" : ",");
    return text;
}

}
