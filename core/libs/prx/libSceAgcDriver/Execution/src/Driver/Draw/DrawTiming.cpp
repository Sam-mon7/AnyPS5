#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawTiming.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <string>
#include <vector>

namespace AgcDriver::DriverDetail {

PendingDrawPhases& Driver::pendingDrawPhases() {
    static thread_local PendingDrawPhases pending;
    return pending;
}

bool DrawShaderProfileEnabled() {
    static const bool enabled = std::getenv("APS5_PROFILE_DRAW") != nullptr && std::getenv("APS5_PROFILE_DRAW_SHADERS") != nullptr;
    return enabled;
}

bool ShaderProfileReportEnabled() {
    static const bool enabled = DrawShaderProfileEnabled() && std::getenv("APS5_PROFILE_SHADERS_REPORT") != nullptr;
    return enabled;
}

void Driver::reportDrawShaders() {
    const auto now = std::chrono::steady_clock::now();
    const auto seconds = std::chrono::duration<double>(now - drawShaderReport).count();
    drawShaderReport = now;
    const auto rowMs = [](const DrawShaderTotals* row) { return std::accumulate(row->ms.begin(), row->ms.end(), 0.0) + row->commitMs; };
    std::vector<const DrawShaderTotals*> rows;
    double total = 0;
    std::uint64_t packets = 0;
    for (const auto& [identity, row] : drawShaderTotals) {
        rows.push_back(&row);
        total += rowMs(&row);
        packets += row.draws;
    }
    std::sort(rows.begin(), rows.end(), [&](const auto* a, const auto* b) { return rowMs(a) > rowMs(b); });
    std::string report;
    for (std::size_t index = 0; index < rows.size() && index < 12; ++index) {
        const auto& row = *rows[index];
        std::string programs;
        for (std::size_t program = 0; program < row.identity.programs; ++program) {
            char code[48];
            std::snprintf(code, sizeof(code), "%s%u:0x%llx", program == 0 ? "" : "+", static_cast<unsigned>(row.identity.stage[program]), static_cast<unsigned long long>(row.identity.code[program]));
            programs += code;
        }
        std::array<std::size_t, DrawDriverPhaseCount> order{};
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::partial_sort(order.begin(), order.begin() + 3, order.end(), [&](std::size_t a, std::size_t b) { return row.ms[a] > row.ms[b]; });
        char text[512];
        std::snprintf(text, sizeof(text), "\n  %s: %llu packets (%llu drawn, %llu captures), %.1f ms (commit %.1f), %.1f us each; top phases %s %.1f, %s %.1f, %s %.1f ms", programs.empty() ? "(no programs)" : programs.c_str(),
                      static_cast<unsigned long long>(row.draws), static_cast<unsigned long long>(row.drawn), static_cast<unsigned long long>(row.captures), rowMs(&row), row.commitMs, row.draws == 0 ? 0.0 : rowMs(&row) * 1000 / static_cast<double>(row.draws),
                      DrawDriverPhaseNames[order[0]], row.ms[order[0]], DrawDriverPhaseNames[order[1]], row.ms[order[1]], DrawDriverPhaseNames[order[2]], row.ms[order[2]]);
        report += text;
        report += "; cache";
        for (std::size_t outcome = 0; outcome < DrawCacheOutcomeCount; ++outcome) {
            if (row.cache[outcome] == 0) continue;
            std::snprintf(text, sizeof(text), " %s %llu", DrawCacheOutcomeNames[outcome], static_cast<unsigned long long>(row.cache[outcome]));
            report += text;
        }
        if (row.graphicsDraws != 0) {
            std::array<std::size_t, DrawProfileGraphicsPhases> graphicsOrder{};
            std::iota(graphicsOrder.begin(), graphicsOrder.end(), std::size_t{0});
            std::partial_sort(graphicsOrder.begin(), graphicsOrder.begin() + 4, graphicsOrder.end(), [&](std::size_t a, std::size_t b) { return row.graphicsUs[a] > row.graphicsUs[b]; });
            std::snprintf(text, sizeof(text), "; Graphics::Draw per draw (%llu timed): %s %.0f, %s %.0f, %s %.0f, %s %.0f us", static_cast<unsigned long long>(row.graphicsDraws),
                          Graphics::DrawGraphicsPhaseName(graphicsOrder[0]), row.graphicsUs[graphicsOrder[0]] / static_cast<double>(row.graphicsDraws), Graphics::DrawGraphicsPhaseName(graphicsOrder[1]), row.graphicsUs[graphicsOrder[1]] / static_cast<double>(row.graphicsDraws),
                          Graphics::DrawGraphicsPhaseName(graphicsOrder[2]), row.graphicsUs[graphicsOrder[2]] / static_cast<double>(row.graphicsDraws), Graphics::DrawGraphicsPhaseName(graphicsOrder[3]), row.graphicsUs[graphicsOrder[3]] / static_cast<double>(row.graphicsDraws));
            report += text;
        }
    }
    AgcDriver::ProfilePrint_nid_no_patch("[draw-shaders] %.1f s, %llu draw packets of %zu program sets, %.1f ms; slowest:%s\n", seconds, static_cast<unsigned long long>(packets), rows.size(), total, report.c_str());
    drawShaderTotals.clear();
}

std::vector<DrawShaderTotals> Driver::DrawShaderProfile(bool reset) {
    std::lock_guard lock(drawPhasesMutex);
    std::vector<DrawShaderTotals> rows;
    rows.reserve(drawShaderTotals.size());
    for (const auto& [identity, row] : drawShaderTotals) rows.push_back(row);
    if (reset) drawShaderTotals.clear();
    return rows;
}

void Driver::addDrawCommit(const DrawProgramIdentity& identity, double ms) {
    std::lock_guard lock(drawPhasesMutex);
    auto& row = drawShaderTotals[identity];
    row.identity = identity;
    row.commitMs += ms;
    ++row.commits;
}

void Driver::addDrawPhases(const std::array<double, DrawDriverPhaseCount>& ms, bool drawn, std::uint64_t captures, const DrawProgramIdentity& identity, DrawCacheOutcome cache, const std::array<double, DrawProfileGraphicsPhases>* graphicsUs) {
    std::lock_guard lock(drawPhasesMutex);
    if (DrawShaderProfileEnabled()) {
        auto& row = drawShaderTotals[identity];
        row.identity = identity;
        for (std::size_t i = 0; i < DrawDriverPhaseCount; ++i) row.ms[i] += ms[i];
        ++row.draws;
        if (drawn) ++row.drawn;
        row.captures += captures;
        ++row.cache[cache];
        if (graphicsUs != nullptr) {
            for (std::size_t i = 0; i < DrawProfileGraphicsPhases; ++i) row.graphicsUs[i] += (*graphicsUs)[i];
            ++row.graphicsDraws;
        }
        if (ShaderProfileReportEnabled() && std::chrono::steady_clock::now() - drawShaderReport >= std::chrono::seconds(10)) reportDrawShaders();
    }
    auto& totals = drawPhaseTotals;
    for (std::size_t i = 0; i < DrawDriverPhaseCount; ++i) totals.ms[i] += ms[i];
    ++totals.packets;
    if (drawn) ++totals.drawn;
    totals.captures += captures;
    const auto now = std::chrono::steady_clock::now();
    if (now - totals.lastReport < std::chrono::seconds(10)) return;
    totals.lastReport = now;
    std::string report;
    double total = 0;
    for (std::size_t i = 0; i < DrawDriverPhaseCount; ++i) {
        char text[64];
        std::snprintf(text, sizeof(text), " %s %.1f", DrawDriverPhaseNames[i], totals.ms[i] * 1000 / static_cast<double>(totals.packets));
        report += text;
        total += totals.ms[i];
    }
    AgcDriver::ProfilePrint_nid_no_patch( "[draw] driver phases (10 s, %llu draw packets, %llu drawn, %llu captures), us per packet:%s, total %.1f (%.1f ms; Graphics::Draw %.1f ms, skipped %.1f ms)\n", static_cast<unsigned long long>(totals.packets), static_cast<unsigned long long>(totals.drawn), static_cast<unsigned long long>(totals.captures), report.c_str(), total * 1000 / static_cast<double>(totals.packets), total, totals.ms[DrawRowGraphics], totals.ms[DrawRowSkipped]);
    totals.ms = {};
    totals.packets = totals.drawn = totals.captures = 0;
}

void DrawPhaseTiming::Phase(DrawDriverPhase which) {
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    phaseMs[which] += std::chrono::duration<double, std::milli>(now - phaseLap).count();
    phaseLap = now;
}

}
