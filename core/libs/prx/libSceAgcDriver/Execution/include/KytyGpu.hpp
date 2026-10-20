#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_KYTYGPU_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_KYTYGPU_HPP

#include <cstdint>
#include "SceTypes.hpp"
#include "SceShaders.hpp"

namespace AgcDriver::KytyGpu {

bool Enabled();
bool Started();
void Start(void* window, std::uint32_t width, std::uint32_t height);
void Submit(const Packet* packet, std::uint32_t queue);
int AddEqEvent(KernelEqueue eq, int id, void* udata);
int DeleteEqEvent(KernelEqueue eq, int id);
void RegisterShader(const Shader* shader);
void SuspendPoint();
void WaitIdle();
void RedirectAgc(void* agcModule);

}

extern "C" bool AgcDriverKytyGpuEnabled_nid_no_patch();
extern "C" void AgcDriverKytyGpuStart_nid_no_patch(void* window, std::uint32_t width, std::uint32_t height);
extern "C" void* AgcDriverKytyGpuVideoOut_nid_no_patch(const char* name);
extern "C" void AgcDriverKytyGpuRedirectAgc_nid_no_patch(void* agcModule);

#endif
