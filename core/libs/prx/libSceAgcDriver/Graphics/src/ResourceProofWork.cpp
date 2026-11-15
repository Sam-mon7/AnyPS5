#include "prx/libSceAgcDriver/Graphics/include/ResourceProofWork.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Graphics::ResourceProofWork {

struct ScopeSlot {};
bool Enabled() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_ARCHB_C_DRAW_REPLAY_STEPS"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    return enabled;
}
Scope* Current() { return Enabled() ? HostThreadSlot<Scope*, ScopeSlot>::Get() : nullptr; }
void SetCurrent(Scope* scope) { HostThreadSlot<Scope*, ScopeSlot>::Set(scope); }

Counters& Totals() { static Counters value; return value; }
Statistics Snapshot() {
    Statistics result;
    const auto& totals = Totals();
    for (std::size_t path = 0; path < result.proofs.size(); ++path) {
        result.proofs[path] = totals.proofs[path].load(std::memory_order_relaxed);
        result.failed[path] = totals.failed[path].load(std::memory_order_relaxed);
        for (std::size_t kind = 0; kind < result.proofsWith[path].size(); ++kind) result.proofsWith[path][kind] = totals.proofsWith[path][kind].load(std::memory_order_relaxed);
        for (std::size_t phase = 0; phase < result.events[path].size(); ++phase) {
            for (std::size_t kind = 0; kind < result.events[path][phase].size(); ++kind) {
                const auto& value = totals.events[path][phase][kind];
                result.events[path][phase][kind] = {value.operations.load(std::memory_order_relaxed), value.bytes.load(std::memory_order_relaxed), value.proofs.load(std::memory_order_relaxed)};
            }
        }
    }
    return result;
}


}
