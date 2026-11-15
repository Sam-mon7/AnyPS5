#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWHITCENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWHITCENSUS_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <chrono>

namespace AgcDriver::DriverDetail {

bool DrawHitCensusEnabled();

struct DrawHitObservation {
    std::uint64_t fault = 0, stored = 0, forget = 0, allocations = 0;
    std::vector<std::uint32_t> packet;
    std::array<std::uint64_t, 8> arguments{};
    std::vector<std::vector<std::uint32_t>> userData;
    std::vector<std::shared_ptr<DispatchVariant>> stages;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
    std::vector<GuestMemory::UnchangedQuery> queries;
};

class DrawHitCensus {
public:
    DrawHitCensus(const std::shared_ptr<DrawEntry>& entry, bool registerKey, std::span<const std::uint32_t> packet, const Pm4::DrawParameters& parameters, const Graphics::State& graphics, const std::vector<DrawProgram>& programs);
    void Finish(bool hit, bool exact, const std::vector<std::shared_ptr<DispatchVariant>>& matched);
    bool Active() const { return enabled; }

private:
    const char* refusal(const DrawHitObservation& observation) const;
    std::shared_ptr<DrawEntry> entry;
    const std::vector<DrawProgram>* programs = nullptr;
    std::span<const std::uint32_t> packet;
    std::array<std::uint64_t, 8> arguments{};
    std::shared_ptr<const DrawHitObservation> predicted;
    std::uint64_t fault = 0, stored = 0, forget = 0, allocations = 0;
    std::chrono::steady_clock::time_point began{};
    double waitedMs = 0;
    bool enabled = false, eligible = false, indirect = false;
};

}

#endif
