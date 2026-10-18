#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_PREPARATIONPROOF_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_PREPARATIONPROOF_HPP

#include "WorkerPool.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/RecordProof.hpp"

namespace AgcDriver::DriverDetail::Lanes {

inline Chunk WithProofs(Chunk chunk, const PageTracker::Tracker& tracker, std::vector<std::shared_ptr<const PinnedRecordProof>> proofs, std::function<std::uint64_t()> forget, Commit fallback) {
    if (!fallback || !forget) throw std::invalid_argument("GPU lanes proof preparation requires a forget serial and ordered fallback");
    if (chunk.validate || chunk.fallback) throw std::invalid_argument("GPU lanes chunk already has a proof guard");
    chunk.validate = [&tracker, proofs = std::move(proofs), forget = std::move(forget)] {
        if (!Enabled() || !RecordProof::Enabled() || proofs.empty()) return false;
        const auto before = forget();
        for (const auto& proof : proofs) if (proof == nullptr || proof->Check(tracker, before) != RecordProofResult::Accepted) return false;
        return forget() == before;
    };
    chunk.fallback = std::move(fallback);
    return chunk;
}

}
#endif
