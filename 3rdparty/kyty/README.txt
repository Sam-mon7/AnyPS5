Files copied unchanged from KytyPS5 (https://github.com/KytyPS5/KytyPS5) at commit 099de8dea62cbf17c8e06a6c24e72b38475d1566, GPL-2.0 (LICENSE).
Parts of KytyPS5 derive from shadPS4 (GPL-2.0-or-later); their SPDX headers are kept.
AnyPS5 builds bufferCache, memoryTracker and streamBuffer with its own adapters in
core/libs/prx/libSceAgcDriver/Graphics/kyty-adapters.
AnyPS5 changes to the copies: bufferCache.cpp allocates a 16-byte BDA page table and skips its
writes (AnyPS5 shaders use their own range table), so the 512 MiB table is not created.
faultManager.{h,cpp} and fault_buffer_process.comp are kept for reference; an adapter header replaces faultManager.h.
