#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_KYTYVIDEOOUT_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_KYTYVIDEOOUT_HPP

#include <stdexcept>
#include <string>
#include "prx/libSceAgcDriver/Execution/include/KytyGpu.hpp"
#include "prx/libSceVideoOut/include/VideoOutDriver.hpp"

inline bool KytyVideoOutEnabled() {
    return AgcDriverKytyGpuEnabled_nid_no_patch();
}

template <typename Function>
Function KytyVideoOut(const char* name) {
    static_cast<void>(VideoOutDriver::Get());
    auto* found = AgcDriverKytyGpuVideoOut_nid_no_patch(name);
    if (found == nullptr) throw std::runtime_error(std::string("Kyty GPU: no video out function ") + name);
    return reinterpret_cast<Function>(found);
}

#endif
