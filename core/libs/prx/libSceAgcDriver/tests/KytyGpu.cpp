#include "prx/libSceAgcDriver/Execution/include/KytyGpu.hpp"

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
    }
    std::printf("Kyty GPU switch %s passed\n", expected ? "on" : "off");
    return 0;
}
