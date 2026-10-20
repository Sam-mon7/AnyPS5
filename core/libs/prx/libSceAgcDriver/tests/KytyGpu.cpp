#include "prx/libSceAgcDriver/Execution/include/KytyGpu.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

int main() {
    const char* text = std::getenv("APS5_KYTY_GPU");
    const bool expected = text != nullptr && std::string_view(text) == "1";
    if (AgcDriverKytyGpuEnabled_nid_no_patch() != expected || AgcDriver::KytyGpu::Enabled() != expected) {
        std::fprintf(stderr, "APS5_KYTY_GPU parsed wrongly\n");
        return 1;
    }
    if (AgcDriver::KytyGpu::Started()) {
        std::fprintf(stderr, "the Kyty GPU started without a window\n");
        return 1;
    }
    if (expected) {
        for (const char* name : {"Open", "Close", "SubmitFlip", "GetFlipStatus", "AddFlipEvent", "RegisterBuffers2", "SetBufferAttribute2"}) {
            if (AgcDriverKytyGpuVideoOut_nid_no_patch(name) == nullptr) {
                std::fprintf(stderr, "kytygpu.dll lacks video out function %s\n", name);
                return 1;
            }
        }
        if (AgcDriverKytyGpuVideoOut_nid_no_patch("NoSuchFunction") != nullptr) {
            std::fprintf(stderr, "kytygpu.dll resolved an unknown video out function\n");
            return 1;
        }
        using Switch = bool (*)();
        auto* module = GetModuleHandleA("kytygpu.dll");
        const std::pair<const char*, const char*> switches[] = {
            {"KytyHostCaptureEnabled", "APS5_KYTY_GPU_CAPTURE"},
            {"KytyHostReadWindowEnabled", "APS5_KYTY_READ_WINDOW"},
            {"KytyHostAsyncReadbackEnabled", "APS5_KYTY_ASYNC_READBACK"},
            {"KytyHostComputeChainEnabled", "APS5_KYTY_COMPUTE_CHAIN"},
            {"KytyHostProfileEnabled", "APS5_KYTY_GPU_PROFILE"},
        };
        for (const auto& [function, variable] : switches) {
            const auto enabled = module != nullptr ? reinterpret_cast<Switch>(GetProcAddress(module, function)) : nullptr;
            const char* switchText = std::getenv(variable);
            const bool switchExpected = switchText != nullptr && std::string_view(switchText) == "1";
            if (enabled == nullptr || enabled() != switchExpected) {
                std::fprintf(stderr, "%s parsed wrongly\n", variable);
                return 1;
            }
        }
    }
    std::printf("Kyty GPU switch %s passed\n", expected ? "on" : "off");
    return 0;
}
