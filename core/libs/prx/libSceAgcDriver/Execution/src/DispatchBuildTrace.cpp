#include "prx/libSceAgcDriver/Execution/include/DispatchBuildTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Recompiler.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver {
namespace {

std::uint64_t mix(std::uint64_t value, std::uint64_t word) {
    for (unsigned byte = 0; byte < 8; ++byte) { value ^= (word >> (byte * 8u)) & 255u; value *= 1099511628211ull; }
    return value;
}

std::uint64_t keyId(std::span<const std::uint32_t> key) {
    auto result = mix(14695981039346656037ull, key.size());
    for (const auto word : key) result = mix(result, word);
    return result;
}

DispatchBuildTrace::Context& current() {
    static thread_local DispatchBuildTrace::Context value;
    return value;
}

std::atomic<std::uint64_t> invocations{0};

struct Input {
    std::uint32_t role, kind, binding, element;
    std::vector<std::uint32_t> words;
    std::optional<std::uint64_t> address, bytes;
};

std::vector<Input> inputs(const ShaderRecompiler::RecompileResult* shader, bool& truncated) {
    std::vector<Input> result;
    if (shader == nullptr) return result;
    for (const auto& binding : shader->bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers && binding.role != ShaderRecompiler::DescriptorRole::GuestImages) continue;
        if (binding.count == 0 || binding.guestDescriptor.size() % binding.count != 0) continue;
        const auto width = binding.guestDescriptor.size() / binding.count;
        if (width > 8) { truncated = true; continue; }
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            if (result.size() == 128) { truncated = true; return result; }
            const auto words = std::span(binding.guestDescriptor).subspan(element * width, width);
            Input entry{static_cast<std::uint32_t>(binding.role), static_cast<std::uint32_t>(binding.kind), binding.binding, element, {words.begin(), words.end()}, {}, {}};
            if (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers && width == 4) {
                entry.address = words[0] | (static_cast<std::uint64_t>(words[1] & 0xffffu) << 32u);
                const auto stride = (words[1] >> 16u) & 0x3fffu;
                entry.bytes = stride == 0 ? words[2] : static_cast<std::uint64_t>(stride) * words[2];
            }
            result.push_back(std::move(entry));
        }
    }
    return result;
}

const char* roleName(std::uint32_t role) {
    return role == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorRole::GuestBuffers) ? "GuestBuffers" : "GuestImages";
}

struct Previous {
    std::uint64_t key = 0;
    std::vector<Input> inputs;
    bool truncated = false;
};
struct Bucket {
    std::uint64_t seen = 0, emitted = 0;
    std::uint64_t completeNs = 0, imagesNs = 0, uploadNs = 0, revalidateNs = 0, uploadBytes = 0;
};
struct TraceState {
    std::mutex mutex;
    DispatchBuildTrace::Statistics stats{};
    std::map<std::array<std::uint64_t, 3>, Bucket> buckets;
    std::map<std::uint64_t, Previous> previous;
    std::map<std::array<std::uint64_t, 2>, Bucket> sites;
    std::map<std::array<std::uint64_t, 2>, Bucket> queues;
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool capped = false;
    double tokens = 128;
    std::chrono::steady_clock::time_point tokenTime = std::chrono::steady_clock::now();
};
TraceState& state() { static auto* value = new TraceState; return *value; }

void field(std::ostringstream& out, const char* name, const char* value) { out << ",\"" << name << "\":\"" << value << '"'; }
void field(std::ostringstream& out, const char* name, std::uint64_t value) { out << ",\"" << name << "\":" << value; }
void optional(std::ostringstream& out, const char* name, std::optional<std::uint64_t> value) {
    out << ",\"" << name << "\":";
    if (value) out << *value; else out << "null";
}
void index(std::ostringstream& out, const char* name, std::size_t value) {
    optional(out, name, value == static_cast<std::size_t>(-1) ? std::nullopt : std::optional<std::uint64_t>{value});
}
void aggregate(std::ostringstream& out, const char* prefix, const Bucket& value, bool overflow, const char* scope) {
    const std::string name(prefix);
    field(out, (name + "_aggregate_scope").c_str(), overflow ? "global-overflow" : scope);
    const auto number = [&](const char* suffix, std::uint64_t total) {
        optional(out, (name + suffix).c_str(), overflow ? std::nullopt : std::optional{total});
        if (overflow) field(out, (name + "_overflow" + suffix).c_str(), total);
    };
    number("_occurrence", value.seen);
    number("_complete_ns_total", value.completeNs);
    number("_images_ns_total", value.imagesNs);
    number("_upload_ns_total", value.uploadNs);
    number("_revalidate_ns_total", value.revalidateNs);
    number("_upload_bytes_total", value.uploadBytes);
}
void detail(std::ostringstream& out, const DispatchBuildTrace::Detail& value) {
    field(out, "failed_check", value.check); field(out, "failed_role", value.role);
    index(out, "failed_binding", value.binding); index(out, "failed_element", value.element);
    optional(out, "prior_address", value.priorAddress); optional(out, "current_address", value.currentAddress);
    optional(out, "prior_size", value.priorBytes); optional(out, "current_size", value.currentBytes);
    optional(out, "prior_identity", value.priorIdentity); optional(out, "current_identity", value.currentIdentity);
}

}

bool DispatchBuildTrace::Enabled() noexcept {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_TRACE_DISPATCH_BUILDS"); return value != nullptr && std::string_view(value) == "1"; }();
    return enabled;
}
DispatchBuildTrace::Context DispatchBuildTrace::Current() noexcept { return Enabled() ? current() : Context{}; }
void DispatchBuildTrace::Recipe(const char* result) noexcept { if (Enabled()) current().recipe = result; }
DispatchBuildTrace::Scope::Scope(std::uint32_t queue, std::uint64_t program, std::uint64_t digest, std::uint64_t packetOffset, std::uint64_t submission) {
    if (!Enabled()) return;
    active = true;
    previous = current();
    current() = {true, queue, program, digest, packetOffset, submission, ArchB::Recorder::Generation(), invocations.fetch_add(1, std::memory_order_relaxed) + 1, "not-attempted"};
}
DispatchBuildTrace::Scope::Scope(Context context) {
    if (!Enabled()) return;
    active = true;
    previous = current();
    current() = context;
}
DispatchBuildTrace::Scope::~Scope() { if (active) current() = previous; }

void DispatchBuildTrace::Emit(const Event& event) noexcept {
    if (!Enabled()) return;
    try {
        const auto source = Current();
        const auto queue = source.known ? source.queue : GuestMemory::CurrentPacket().queue;
        const auto variant = event.shader != nullptr ? event.shader->PipelineVariantId() : 0;
        auto site = mix(mix(14695981039346656037ull, queue), event.program);
        site = mix(site, source.known ? source.digest : variant);
        site = mix(site, source.known ? source.packetOffset : 0);
        const auto resource = keyId(event.key);
        auto category = std::uint64_t{14695981039346656037ull};
        for (const auto ch : std::string_view(event.event)) category = mix(category, static_cast<unsigned char>(ch));
        for (const auto ch : std::string_view(event.reason)) category = mix(category, static_cast<unsigned char>(ch));
        for (const auto ch : std::string_view(event.detail.check)) category = mix(category, static_cast<unsigned char>(ch));
        for (const auto ch : std::string_view(event.detail.role)) category = mix(category, static_cast<unsigned char>(ch));
        category = mix(mix(category, event.detail.binding), event.detail.element);
        for (const auto ch : std::string_view(event.reusable)) category = mix(category, static_cast<unsigned char>(ch));
        for (const auto ch : std::string_view(event.recipeBuilt)) category = mix(category, static_cast<unsigned char>(ch));
        for (const auto flag : {event.addressTable, event.bdaReadOnly, event.spaceOnly, event.copiedWrites, event.holdsLease}) category = mix(category, flag.value_or(2));
        auto& shared = state();
        std::lock_guard lock(shared.mutex);
        ++shared.stats.events;
        auto id = std::array{site, resource, category};
        if (!shared.buckets.contains(id) && shared.buckets.size() >= 2048) id = {0, 0, 0};
        auto& bucket = shared.buckets[id];
        ++bucket.seen;
        bucket.completeNs += event.completeNs; bucket.imagesNs += event.imagesNs; bucket.uploadNs += event.uploadNs;
        bucket.revalidateNs += event.revalidateNs; bucket.uploadBytes += event.cpuBytes + event.gpuBytes;
        auto siteKey = std::array{site, category};
        if (!shared.sites.contains(siteKey) && shared.sites.size() >= 1024) siteKey = {0, 0};
        auto& siteBucket = shared.sites[siteKey];
        ++siteBucket.seen;
        siteBucket.completeNs += event.completeNs; siteBucket.imagesNs += event.imagesNs; siteBucket.uploadNs += event.uploadNs;
        siteBucket.revalidateNs += event.revalidateNs; siteBucket.uploadBytes += event.cpuBytes + event.gpuBytes;
        auto queueKey = std::array{static_cast<std::uint64_t>(queue), category};
        if (!shared.queues.contains(queueKey) && shared.queues.size() >= 512) queueKey = {0, 0};
        auto& queueBucket = shared.queues[queueKey];
        ++queueBucket.seen;
        queueBucket.completeNs += event.completeNs; queueBucket.imagesNs += event.imagesNs; queueBucket.uploadNs += event.uploadNs;
        queueBucket.revalidateNs += event.revalidateNs; queueBucket.uploadBytes += event.cpuBytes + event.gpuBytes;
        constexpr std::uint64_t budget = 4u * 1024u * 1024u;
        const bool sample = siteBucket.seen <= 2 || (siteBucket.seen & (siteBucket.seen - 1)) == 0 || queueBucket.seen <= 2 || (queueBucket.seen & (queueBucket.seen - 1)) == 0;
        const auto nowTime = std::chrono::steady_clock::now();
        shared.tokens = std::min(128.0, shared.tokens + std::chrono::duration<double>(nowTime - shared.tokenTime).count() * 8.0);
        shared.tokenTime = nowTime;
        if (!sample || shared.capped || shared.tokens < 1.0) { ++shared.stats.suppressed; return; }
        shared.tokens -= 1.0;
        std::ostringstream out;
        out << "[dispatch-build] {\"version\":2";
        field(out, "event", event.event); field(out, "reason", event.reason);
        field(out, "steady_ns", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowTime.time_since_epoch()).count()));
        field(out, "trace_elapsed_ns", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowTime - shared.started).count()));
        field(out, "aggregate_category_id", category);
        field(out, "queue", queue); field(out, "program", event.program); field(out, "stable_dispatch_id", site);
        field(out, "stable_id_scope", source.known ? "queue-program-code-packet-offset" : "api-queue-program-variant");
        optional(out, "code_digest", source.known ? std::optional{source.digest} : std::nullopt);
        optional(out, "submission", source.known ? std::optional{source.submission} : std::nullopt);
        optional(out, "packet_offset", source.known ? std::optional{source.packetOffset} : std::nullopt);
        optional(out, "invocation", source.known ? std::optional{source.invocation} : std::nullopt);
        optional(out, "interval_generation", source.known ? std::optional{source.interval} : std::nullopt);
        optional(out, "frame", std::nullopt);
        field(out, "variant_id", variant); optional(out, "resource_key_id", event.key.empty() ? std::nullopt : std::optional{resource});
        field(out, "resource_key_words", event.key.size()); field(out, "exact_find", event.exactFind); field(out, "movable_find", event.movableFind);
        field(out, "recipe_result", source.known ? source.recipe : "unknown-api-context"); field(out, "recipe_built", event.recipeBuilt); field(out, "reuse_result", event.reuse); field(out, "reusable_reason", event.reusable);
        field(out, "cache_insert", event.cacheInsert); field(out, "cache_membership", event.cacheMembership);
        detail(out, event.detail);
        optional(out, "address_table", event.addressTable); optional(out, "bda_read_only", event.bdaReadOnly);
        optional(out, "space_only", event.spaceOnly); optional(out, "copied_writes", event.copiedWrites); optional(out, "holds_lease", event.holdsLease);
        field(out, "eligibility_scope", "preliminary-space-flags-not-reuse-proof");
        field(out, "stage_a_ns", event.stageANs); field(out, "complete_ns", event.completeNs); field(out, "images_ns", event.imagesNs);
        field(out, "upload_ns", event.uploadNs); field(out, "bda_ns", event.bdaNs); field(out, "revalidate_ns", event.revalidateNs);
        field(out, "upload_bytes", event.cpuBytes + event.gpuBytes); field(out, "upload_scope", "guest-region-cpu+staging-in");
        field(out, "region_cpu_bytes", event.cpuBytes); field(out, "staging_in_gpu_bytes", event.gpuBytes); field(out, "copied_buffer_footprint", event.copiedBytes);
        optional(out, "key_previously_emitted", id[0] == 0 ? std::nullopt : std::optional{bucket.emitted});
        field(out, "aggregate_overflow", static_cast<std::uint64_t>(id[0] == 0));
        field(out, "site_aggregate_overflow", static_cast<std::uint64_t>(siteKey[0] == 0));
        field(out, "queue_aggregate_overflow", static_cast<std::uint64_t>(queueKey[1] == 0));
        aggregate(out, "key", bucket, id[0] == 0, "resource-key+site+category");
        aggregate(out, "site", siteBucket, siteKey[0] == 0, "site+category");
        aggregate(out, "queue", queueBucket, queueKey[1] == 0, "queue+category");
        field(out, "events_total", shared.stats.events); field(out, "suppressed_total", shared.stats.suppressed);
        if (std::string_view(event.event) == "build") {
            bool truncated = false;
            auto live = inputs(event.shader, truncated);
            const auto prior = shared.previous.find(site);
            field(out, "comparison_scope", "previous-emitted-build-same-site");
            field(out, "changed_size_scope", "raw-descriptor-not-binding-range");
            optional(out, "prior_resource_key_id", prior != shared.previous.end() ? std::optional{prior->second.key} : std::nullopt);
            field(out, "comparison_truncated", static_cast<std::uint64_t>(truncated || (prior != shared.previous.end() && prior->second.truncated)));
            const Input* old = nullptr;
            const Input* now = nullptr;
            if (prior != shared.previous.end()) {
                const auto count = std::min(live.size(), prior->second.inputs.size());
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& a = prior->second.inputs[i]; const auto& b = live[i];
                    if (a.role != b.role || a.kind != b.kind || a.binding != b.binding || a.element != b.element || a.words != b.words) { old = &a; now = &b; break; }
                }
            }
            field(out, "changed_role", now != nullptr ? roleName(now->role) : "unknown");
            index(out, "changed_binding", now != nullptr ? now->binding : static_cast<std::size_t>(-1));
            index(out, "changed_element", now != nullptr ? now->element : static_cast<std::size_t>(-1));
            optional(out, "changed_prior_address", old != nullptr ? old->address : std::nullopt);
            optional(out, "changed_current_address", now != nullptr ? now->address : std::nullopt);
            optional(out, "changed_prior_size", old != nullptr ? old->bytes : std::nullopt);
            optional(out, "changed_current_size", now != nullptr ? now->bytes : std::nullopt);
            if (prior != shared.previous.end() || shared.previous.size() < 256) shared.previous[site] = {resource, std::move(live), truncated};
        }
        out << "}\n";
        const auto line = out.str();
        if (line.size() > budget - std::min(budget, shared.stats.bytes)) {
            ++shared.stats.suppressed;
            shared.capped = true;
            std::fprintf(stderr, "[dispatch-build-limit] budget=4194304 events=%llu suppressed=%llu\n", static_cast<unsigned long long>(shared.stats.events), static_cast<unsigned long long>(shared.stats.suppressed));
            return;
        }
        shared.stats.bytes += line.size();
        ++shared.stats.emitted; ++bucket.emitted;
        std::fwrite(line.data(), 1, line.size(), stderr);
    } catch (...) {
        try { auto& shared = state(); std::lock_guard lock(shared.mutex); ++shared.stats.failures; } catch (...) {}
    }
}
DispatchBuildTrace::Statistics DispatchBuildTrace::Counts() noexcept {
    try { auto& shared = state(); std::lock_guard lock(shared.mutex); return shared.stats; } catch (...) { return {}; }
}

}
