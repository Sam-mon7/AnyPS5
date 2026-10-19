#include "prx/libSceAgcDriver/Execution/include/KytyGpu.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

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
        using Switch = const char* (*)(const char*);
        auto* module = GetModuleHandleA("kytygpu.dll");
        const auto lookup = module != nullptr ? reinterpret_cast<Switch>(GetProcAddress(module, "KytyHostSwitch")) : nullptr;
        if (lookup == nullptr) {
            std::fprintf(stderr, "kytygpu.dll lacks KytyHostSwitch\n");
            return 1;
        }
        const char* safeText = std::getenv("APS5_KYTY_GPU_SAFE");
        const bool safe = safeText != nullptr && std::string_view(safeText) == "1";
        const char* pipeline = lookup("KYTY_FRAME_PIPELINE");
        const char* batch = lookup("KYTY_DISPATCH_BATCH");
        const bool recommended = pipeline != nullptr && std::string_view(pipeline) == "1" && batch != nullptr && std::string_view(batch) == "128";
        if (safe ? (pipeline != nullptr || batch != nullptr) : !recommended) {
            std::fprintf(stderr, "APS5_KYTY_GPU_SAFE parsed wrongly\n");
            return 1;
        }
        const char* stream = lookup("KYTY_STREAM_UPLOAD");
        if (stream == nullptr || std::string_view(stream) != "0" || lookup("KYTY_NO_SUCH_SWITCH") != nullptr) {
            std::fprintf(stderr, "KytyHostSwitch ignored the environment or invented a switch\n");
            return 1;
        }
    }
    std::printf("Kyty GPU switch %s passed\n", expected ? "on" : "off");
    return 0;
}
