#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LAZYCOPYBACK_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LAZYCOPYBACK_HPP

#include <cstdint>

#include <vulkan/vulkan.h>

// Arch C P5b, APS5_ARCHB_C_LAZY_COPYBACK (runtime, default off; needs C_GPU_STAGING_CARRY and a device
// with VK_KHR_copy_memory_indirect's indirectMemoryCopy, enabled at device creation when the variable is
// set at all). A staged region's whole copy-back (shadow -> host import, GuestBufferMemory::RecordCopyBacks)
// is recorded as an indirect copy whose parameters sit in host-visible memory. When the same shadow's next
// use in the same open batch carries it (no copy-in) and copies it back whole again, and the recorder noted
// no read or write of the range on the batch since the first copy-back's use, that first copy-back is dead:
// its bytes are overwritten by the second before anything reads them. Its size is set to 0 before submit.
// APS5_ARCHB_C_LAZY_COPYBACK_VERIFY=N: one candidate in N keeps its copy-back, and the completion checks that
// the import holds the shadow's bytes over the range after the second copy-back (what the cancellation
// relies on).
namespace AgcDriver::Graphics::LazyCopyBack {

// Set by the device once VK_KHR_copy_memory_indirect is enabled (null: not available).
void SetIndirectCopy(PFN_vkCmdCopyMemoryIndirectKHR function);
// The switch is on and the device has the indirect copy.
bool Active();

struct Counts {
    std::uint64_t indirect = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t cancelledBytes = 0;
    std::uint64_t accessed = 0;
    std::uint64_t verified = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t refusedBaseline = 0;
};
Counts Totals();

}

#endif
