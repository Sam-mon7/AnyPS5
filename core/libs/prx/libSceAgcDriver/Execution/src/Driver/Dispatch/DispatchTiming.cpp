#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchTiming.hpp"
#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

namespace AgcDriver::DriverDetail {

void Driver::addDriverPhases(DispatchClass which, const std::array<double, DriverPhaseCount>& ms, bool hit, bool validated) {
    std::lock_guard lock(driverPhasesMutex);
    auto& totals = driverPhaseTotals[which];
    for (std::size_t i = 0; i < DriverPhaseCount; ++i) totals.ms[i] += ms[i];
    ++totals.dispatches;
    if (hit) ++totals.hits;
    if (validated) ++totals.validations;
    const auto now = std::chrono::steady_clock::now();
    if (now - driverPhasesReport < std::chrono::seconds(10)) return;
    driverPhasesReport = now;
    for (std::size_t cls = 0; cls < DispatchClassCount; ++cls) {
        auto& line = driverPhaseTotals[cls];
        if (line.dispatches == 0) continue;
        std::string report;
        double total = 0;
        for (std::size_t i = 0; i < DriverPhaseCount; ++i) {
            char text[64];
            std::snprintf(text, sizeof(text), " %s %.1f", DriverPhaseNames[i], line.ms[i] * 1000 / static_cast<double>(line.dispatches));
            report += text;
            total += line.ms[i];
        }
        const double perValidation = line.validations != 0 ? 1000 / static_cast<double>(line.validations) : 0.0;
        AgcDriver::ProfilePrint_nid_no_patch("[dispatch] driver phases %s (10 s, %llu dispatches, %llu cache hits), us per dispatch:%s, total %.1f (%.1f ms); validate %.1f us per validation (%llu validations; GPU waits inside it %.1f us per validation apart)\n", DispatchClassNames[cls], static_cast<unsigned long long>(line.dispatches), static_cast<unsigned long long>(line.hits), report.c_str(), total * 1000 / static_cast<double>(line.dispatches), total, line.ms[PhaseValidate] * perValidation, static_cast<unsigned long long>(line.validations), line.ms[PhaseValidateWait] * perValidation);
        line = {};
    }
}

void Driver::addDispatchShader(DispatchClass which, const PendingDispatchPhases& pending, double packetMs) {
    std::lock_guard lock(driverPhasesMutex);
    auto& row = dispatchShaderTotals[pending.code | static_cast<std::uint64_t>(which)];
    row.code = pending.code;
    row.which = which;
    const auto outcome = static_cast<std::size_t>(pending.outcome);
    ++row.packets[outcome];
    row.packetMs[outcome] += packetMs;
    if (pending.phases) {
        for (std::size_t i = 0; i < DriverPhaseCount; ++i) row.ms[i] += pending.ms[i];
        if (pending.hit) ++row.hits;
        if (!pending.deviceMs.empty()) {
            if (row.deviceMs.size() < pending.deviceMs.size()) row.deviceMs.resize(pending.deviceMs.size());
            for (std::size_t i = 0; i < pending.deviceMs.size(); ++i) row.deviceMs[i] += pending.deviceMs[i];
            ++row.deviceCalls;
        }
        if (pending.proofSet) {
            ++row.proofs;
            if (!pending.proofProved) ++row.proofsFailed;
            if (pending.proofPath < row.proofPaths.size()) ++row.proofPaths[pending.proofPath];
            if (pending.proofFailure < row.proofFailures.size()) ++row.proofFailures[pending.proofFailure];
            row.proofMs += pending.proofMs;
            row.proofWaitMs += pending.proofWaitMs;
            for (std::size_t i = 0; i < row.proofExtent.size(); ++i) row.proofExtent[i] += pending.proofExtent[i];
        }
    }
    if (ShaderProfileReportEnabled() && std::chrono::steady_clock::now() - dispatchShaderReport >= std::chrono::seconds(10)) reportDispatchShaders();
}

std::vector<DispatchShaderTotals> Driver::DispatchShaderProfile(bool reset) {
    std::lock_guard lock(driverPhasesMutex);
    std::vector<DispatchShaderTotals> rows;
    rows.reserve(dispatchShaderTotals.size());
    for (const auto& [key, row] : dispatchShaderTotals) rows.push_back(row);
    if (reset) dispatchShaderTotals.clear();
    return rows;
}

void Driver::reportDispatchShaders() {
    const auto now = std::chrono::steady_clock::now();
    const auto seconds = std::chrono::duration<double>(now - dispatchShaderReport).count();
    dispatchShaderReport = now;
    const auto rowMs = [](const DispatchShaderTotals* row) { return std::accumulate(row->packetMs.begin(), row->packetMs.end(), 0.0); };
    const auto rowPackets = [](const DispatchShaderTotals* row) { return std::accumulate(row->packets.begin(), row->packets.end(), std::uint64_t{0}); };
    std::vector<const DispatchShaderTotals*> rows;
    double total = 0;
    std::uint64_t packets = 0;
    for (const auto& [key, row] : dispatchShaderTotals) {
        rows.push_back(&row);
        total += rowMs(&row);
        packets += rowPackets(&row);
    }
    std::sort(rows.begin(), rows.end(), [&](const auto* a, const auto* b) { return rowMs(a) > rowMs(b); });
    std::string report;
    for (std::size_t index = 0; index < rows.size() && index < 12; ++index) {
        const auto& row = *rows[index];
        const auto count = rowPackets(&row);
        std::array<std::size_t, DriverPhaseCount> order{};
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::partial_sort(order.begin(), order.begin() + 3, order.end(), [&](std::size_t a, std::size_t b) { return row.ms[a] > row.ms[b]; });
        const auto packetsOf = [&](DispatchOutcome outcome) { return static_cast<unsigned long long>(row.packets[static_cast<std::size_t>(outcome)]); };
        char text[512];
        std::snprintf(text, sizeof(text), "\n  0x%llx %s: %llu packets (real %llu, copy %llu, fill %llu, skipped %llu), %.1f ms, %.1f us each, %llu cache hits; top phases %s %.1f, %s %.1f, %s %.1f ms",
                      static_cast<unsigned long long>(row.code), DispatchClassNames[row.which], static_cast<unsigned long long>(count), packetsOf(DispatchOutcome::Real), packetsOf(DispatchOutcome::CopyHle), packetsOf(DispatchOutcome::FillHle),
                      packetsOf(DispatchOutcome::Skipped) + packetsOf(DispatchOutcome::SkippedMemo), rowMs(&row), count == 0 ? 0.0 : rowMs(&row) * 1000 / static_cast<double>(count), static_cast<unsigned long long>(row.hits),
                      DriverPhaseNames[order[0]], row.ms[order[0]], DriverPhaseNames[order[1]], row.ms[order[1]], DriverPhaseNames[order[2]], row.ms[order[2]]);
        report += text;
        if (row.deviceCalls != 0) {
            std::vector<std::size_t> deviceOrder(row.deviceMs.size());
            std::iota(deviceOrder.begin(), deviceOrder.end(), std::size_t{0});
            const auto shown = std::min<std::size_t>(5, deviceOrder.size());
            std::partial_sort(deviceOrder.begin(), deviceOrder.begin() + static_cast<std::ptrdiff_t>(shown), deviceOrder.end(), [&](std::size_t a, std::size_t b) { return row.deviceMs[a] > row.deviceMs[b]; });
            std::snprintf(text, sizeof(text), "; device call per dispatch (%llu timed):", static_cast<unsigned long long>(row.deviceCalls));
            report += text;
            for (std::size_t index = 0; index < shown; ++index) {
                if (row.deviceMs[deviceOrder[index]] <= 0) break;
                std::snprintf(text, sizeof(text), " %s %.0f us", VulkanDevice::DispatchProfileRowName(deviceOrder[index]), row.deviceMs[deviceOrder[index]] * 1000 / static_cast<double>(row.deviceCalls));
                report += text;
            }
        }
        if (row.proofs != 0) {
            const auto per = [&](std::uint64_t value) { return static_cast<double>(value) / static_cast<double>(row.proofs); };
            std::snprintf(text, sizeof(text), "; proofs %llu (%llu failed; fast %llu, own refreshed %llu, full %llu; failures imports %llu evicted %llu pending %llu changed %llu keys %llu other %llu) %.0f us each (hook waits %.0f us), each over %.1f direct regions %.1f KiB, %.1f described ranges %.1f KiB, %.1f storage images %.1f KiB",
                          static_cast<unsigned long long>(row.proofs), static_cast<unsigned long long>(row.proofsFailed), static_cast<unsigned long long>(row.proofPaths[0]), static_cast<unsigned long long>(row.proofPaths[1]), static_cast<unsigned long long>(row.proofPaths[2]),
                          static_cast<unsigned long long>(row.proofFailures[1]), static_cast<unsigned long long>(row.proofFailures[2]), static_cast<unsigned long long>(row.proofFailures[3]), static_cast<unsigned long long>(row.proofFailures[4]), static_cast<unsigned long long>(row.proofFailures[5]), static_cast<unsigned long long>(row.proofFailures[6]),
                          row.proofMs * 1000 / static_cast<double>(row.proofs), row.proofWaitMs * 1000 / static_cast<double>(row.proofs),
                          per(row.proofExtent[0]), per(row.proofExtent[1]) / 1024, per(row.proofExtent[2]), per(row.proofExtent[3]) / 1024, per(row.proofExtent[4]), per(row.proofExtent[5]) / 1024);
            report += text;
        }
    }
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-shaders] %.1f s, %llu dispatch packets of %zu shaders, %.1f ms; slowest:%s\n", seconds, static_cast<unsigned long long>(packets), rows.size(), total, report.c_str());
    dispatchShaderTotals.clear();
}

PendingDispatchPhases& Driver::pendingDispatchPhases() {
    static thread_local PendingDispatchPhases pending;
    return pending;
}

std::chrono::steady_clock::time_point& Driver::packetStartedAt() {
    static thread_local std::chrono::steady_clock::time_point started{};
    return started;
}

double DispatchPhaseTiming::Elapsed() {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<double, std::milli>(now - lap).count();
    lap = now;
    return ms;
}

void DispatchPhaseTiming::Phase(DriverPhase which) {
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    phaseMs[which] += std::chrono::duration<double, std::milli>(now - phaseLap).count();
    phaseLap = now;
}

}
