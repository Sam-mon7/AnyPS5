#include "prx/libSceAgc/DcbState/include/Init.hpp"

#include "prx/libSceAgc/Command/include/Packet.hpp"
#include "prx/libSceAgc/Command/include/RegisterDefaults.hpp"
#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#ifdef _WIN32
#include "prx/libSceAgcDriver/Execution/include/KytyGpu.hpp"

#include <windows.h>
#endif

static constexpr int SCE_AGC_ERROR_INVALID_ARGUMENT = static_cast<int>(0x80890001);

#ifdef _WIN32
namespace {

const bool kytyRedirected = [] {
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&kytyRedirected), &self)) {
        AgcDriverKytyGpuRedirectAgc_nid_no_patch(self);
    }
    return true;
}();

}
#endif

extern "C" {

int APS5_VABI sceAgcInit_0090(std::uint32_t* state, std::uint32_t version) {
    Agc::Command::CheckAddress(reinterpret_cast<std::uintptr_t>(state), alignof(std::uint32_t), __func__);
    Agc::Command::Require(version < 14, __func__, "unsupported AGC initialization version");
    return 0;
}

int APS5_VABI sceAgcInit(std::uint32_t version) {
    if (version >= 14) return SCE_AGC_ERROR_INVALID_ARGUMENT;
    Agc::Command::GetRegisterDefaults(version, false, __func__);
    Agc::Command::GetRegisterDefaults(version, true, __func__);
    return 0;
}

}
