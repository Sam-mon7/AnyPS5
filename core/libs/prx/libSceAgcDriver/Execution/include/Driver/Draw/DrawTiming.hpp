#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWTIMING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWTIMING_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>

namespace AgcDriver::DriverDetail {

enum DrawDriverPhase { DrawRowPrologue, DrawRowPrecheck, DrawRowDecode, DrawRowProgramPrepare, DrawRowCaptureInvocation, DrawRowCapture, DrawRowCaptureExport, DrawRowCaptureHookWaits, DrawRowRecompile, DrawRowRectList, DrawRowVectors, DrawRowKeyLookupValidate, DrawRowValidateWait, DrawRowLockWait, DrawRowLabels, DrawRowGraphics, DrawRowSkipped, DrawRowEpilogue, DrawDriverPhaseCount };

inline constexpr const char* DrawDriverPhaseNames[DrawDriverPhaseCount] = {"prologue", "precheck", "decode", "program prepare", "capture invocation", "capture", "capture export", "capture hook waits", "recompile", "rect-list", "vectors", "key/lookup/validate", "validate GPU wait", "lock wait", "labels", "Graphics::Draw", "skipped", "epilogue"};

struct DrawPhaseTotals {
    std::array<double, DrawDriverPhaseCount> ms{};
    std::uint64_t packets = 0, drawn = 0, captures = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

inline constexpr std::size_t DrawProfilePrograms = 6;

struct DrawProgramIdentity {
    std::array<std::uint64_t, DrawProfilePrograms> code{};
    std::array<std::uint8_t, DrawProfilePrograms> stage{};
    std::uint8_t programs = 0;

    bool operator==(const DrawProgramIdentity&) const = default;
};

struct DrawProgramIdentityHash {
    std::size_t operator()(const DrawProgramIdentity& identity) const noexcept {
        std::uint64_t hash = 0xcbf29ce484222325ull ^ identity.programs;
        for (std::size_t i = 0; i < identity.programs; ++i) {
            hash = (hash ^ identity.code[i]) * 0x100000001b3ull;
            hash = (hash ^ identity.stage[i]) * 0x100000001b3ull;
        }
        return static_cast<std::size_t>(hash);
    }
};

struct DrawShaderTotals {
    DrawProgramIdentity identity;
    std::array<double, DrawDriverPhaseCount> ms{};
    double commitMs = 0;
    std::uint64_t draws = 0, drawn = 0, captures = 0, commits = 0;
};

bool DrawShaderProfileEnabled();
bool ShaderProfileReportEnabled();

struct PendingDrawPhases {
    bool phases = false;
    std::uint64_t captures = 0;
    std::array<double, DrawDriverPhaseCount> ms{};
    std::chrono::steady_clock::time_point tailAt{};
    DrawProgramIdentity identity;
};

struct DrawPhaseTiming {
    bool profile;
    std::array<double, DrawDriverPhaseCount>& phaseMs;
    std::chrono::steady_clock::time_point& phaseLap;
    void Phase(DrawDriverPhase which);
};

}

#endif
