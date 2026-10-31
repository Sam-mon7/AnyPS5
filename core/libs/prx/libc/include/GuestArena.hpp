#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTARENA_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTARENA_HPP

#include <cstddef>
#include <cstdint>

// Guest virtual memory is placed inside one reserved arena below the PS5 application map limit
// absolute address and breaks on host addresses outside that range.
namespace GuestArena {

#ifdef _WIN32
struct QueryMemoContext {
    std::uint64_t (*unwatch)() = nullptr;
    std::uint64_t (*forget)() = nullptr;
    bool verify = false;
    void (*count)(const char*) = nullptr;
};
extern "C" bool GuestArenaCollectWritesMemo_nid_no_patch(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear, bool armed, const QueryMemoContext* memo);
extern "C" int GuestArenaVirtualProtect_nid_no_patch(void* address, std::size_t bytes, unsigned long protection, unsigned long* previous);
extern "C" std::uint64_t GuestArenaQueryProtectionSerial_nid_no_patch();
#endif

#ifdef ANYPS5_ENABLE_ASAN
inline constexpr std::uintptr_t GuestAddressOffset = 0x0000100000000000ull;
#else
inline constexpr std::uintptr_t GuestAddressOffset = 0;
#endif

#ifndef _WIN32
using SharedBackingResolver = bool (*)(std::uintptr_t address, std::size_t bytes, int* file, std::uint64_t* offset);
using SharedBackingWriter = bool (*)(std::uintptr_t address, const void* source, std::size_t bytes);
#endif

extern "C" {

bool GuestArenaAvailable_nid_postfix();
bool GuestArenaContains_nid_postfix(const void* pointer, std::size_t bytes);
void* GuestArenaAllocate_nid_postfix(std::size_t bytes, std::size_t alignment);
void* GuestArenaAllocateAtOrAbove_nid_postfix(std::uintptr_t hint, std::size_t bytes, std::size_t alignment);
void* GuestArenaAllocateFromHint_nid_postfix(std::uintptr_t hint, std::size_t bytes, std::size_t alignment);
std::uintptr_t GuestArenaHighestFree_nid_no_patch(std::size_t bytes, std::size_t alignment);
bool GuestArenaNextUsed_nid_no_patch(std::uintptr_t address, std::uintptr_t* start, std::uintptr_t* end);
void GuestArenaMarkUsed_nid_postfix(const void* pointer, std::size_t bytes);
void GuestArenaRelease_nid_postfix(const void* pointer, std::size_t bytes);
// The reserved range, and whether it was reserved with page write watching (Windows MEM_WRITE_WATCH).
void GuestArenaRange_nid_postfix(std::uintptr_t* base, std::size_t* bytes);
bool GuestArenaWriteWatched_nid_postfix();
void GuestArenaSetProtection_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uint32_t protection);
void GuestArenaCommit_nid_postfix(void* pointer, std::size_t bytes, std::uint32_t protection, std::size_t granule);
void GuestArenaReset_nid_postfix(void* pointer, std::size_t bytes);
void GuestArenaSetAccessObserver_nid_postfix(void (*callback)(std::uintptr_t address, std::size_t bytes));
#ifdef _WIN32
bool GuestArenaHandleWrite_nid_postfix(std::uintptr_t address);
bool GuestArenaHandleRead_nid_postfix(std::uintptr_t address);
void GuestArenaPinWritable_nid_postfix(const void* pointer, std::size_t bytes);
void GuestArenaUnpinWritable_nid_postfix(const void* pointer, std::size_t bytes);
bool GuestArenaProtection_nid_postfix(std::uintptr_t address, std::uint32_t* protection);
bool GuestArenaCollectWrites_nid_postfix(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear);
bool GuestArenaCollectArmedWrites_nid_postfix(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear);
bool GuestArenaCollectFreshWrites_nid_postfix(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear);
bool GuestArenaWritesClean_nid_no_patch(std::uintptr_t address, std::size_t bytes);
struct WriteWatchTiming {
    double queryUs = 0;
    double walkUs = 0;
};
WriteWatchTiming GuestArenaWriteWatchTiming_nid_no_patch();
// The calling thread's write-watch work so far (cumulative): collection entries, OS GetWriteWatch and
// ResetWriteWatch calls, the 4 KiB pages they covered and the dirty pages reported. The time inside
// the OS calls is measured only under APS5_ARCHB_WRITE_TRACKING_CENSUS=1.
struct WriteWatchCensus {
    std::uint64_t collects = 0;
    std::uint64_t queries = 0;
    std::uint64_t resets = 0;
    std::uint64_t pages = 0;
    std::uint64_t dirty = 0;
    std::uint64_t ns = 0;
};
WriteWatchCensus GuestArenaWriteWatchCensus_nid_no_patch();
// One non-resetting GetWriteWatch over every committed private region of the reservations (what a
// sweep of everything watched would cost): watched regions, their bytes, dirty pages, regions the OS
// refused (not write-watched), and the time of the region walk and of the queries.
struct WriteWatchSweep {
    std::uint64_t regions = 0;
    std::uint64_t bytes = 0;
    std::uint64_t dirty = 0;
    std::uint64_t refused = 0;
    std::uint64_t queries = 0;
    std::uint64_t residentRuns = 0;
    std::uint64_t residentBytes = 0;
    std::uint64_t walkNs = 0;
    std::uint64_t queryNs = 0;
};
WriteWatchSweep GuestArenaWriteWatchSweep_nid_no_patch();
bool GuestArenaHostRegionOverlaps_nid_postfix(std::uintptr_t address, std::size_t bytes);
// The same answer by binary search over the host regions sorted once (the regions do not change after
// the arena is reserved); the driver uses it under APS5_ARCHB_FIX_WRITE_TRACKING_BOOKKEEPING.
bool GuestArenaHostRegionOverlapsSorted_nid_no_patch(std::uintptr_t address, std::size_t bytes);
std::uint64_t GuestArenaCommitGeneration_nid_postfix();
void GuestArenaSetPrivateMappingObserver_nid_postfix(void (*callback)(std::uintptr_t address, std::size_t bytes, std::uint64_t generation));
void GuestArenaMap_nid_postfix(void* pointer, std::size_t bytes, void* section, std::uint64_t offset, std::uint32_t protection);
void GuestArenaMapDirect_nid_postfix(void* pointer, std::size_t bytes, void* section, std::uint64_t offset, std::uint32_t protection);
void GuestArenaMapFile_nid_postfix(void* pointer, std::size_t bytes, void* section, std::uint64_t offset, std::uint32_t protection, std::uint32_t viewProtection, std::size_t sectionBytes);
void* GuestArenaMapAlias_nid_postfix(std::uintptr_t address, std::size_t bytes);
void GuestArenaUnmapAlias_nid_postfix(void* alias);
bool GuestArenaResidentSpan_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uintptr_t* start, std::uintptr_t* end);
void GuestArenaCompareWhenShared_nid_no_patch(std::uintptr_t address, std::size_t bytes);
void GuestArenaCompareResidentWhenShared_nid_no_patch(std::uintptr_t address, std::size_t bytes);
void GuestArenaComparedPages_nid_no_patch(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count);
void GuestArenaTakeUnreportedPages_nid_no_patch(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count);
void GuestArenaForgetSection_nid_postfix(void* section);
bool GuestArenaBeginHostImport_nid_no_patch(void* pointer, std::size_t bytes);
void GuestArenaEndHostImport_nid_no_patch(void* pointer, std::size_t bytes);
bool GuestArenaForgetReusableSection_nid_no_patch(void* section);
#else
void GuestArenaSetSharedBacking_nid_postfix(SharedBackingResolver resolver);
void GuestArenaSetSharedBackingWriter_nid_no_patch(SharedBackingWriter writer);
bool GuestArenaWriteSharedBacking_nid_no_patch(std::uintptr_t address, const void* source, std::size_t bytes);
bool GuestArenaSharedBacking_nid_postfix(std::uintptr_t address, std::size_t bytes, int* file, std::uint64_t* offset);
#endif
bool GuestArenaBeginHostWrite_nid_postfix(void* pointer, std::size_t bytes);
void GuestArenaEndHostWrite_nid_postfix(void* pointer, std::size_t bytes);
// Called with end=false before a host write scope opens (BeginHostWrite) and with end=true after it
// closes (EndHostWrite), before the host call returns to the guest; one observer (the AGC driver's
// write-generation table). Never called for host imports.
void GuestArenaSetHostWriteObserver_nid_no_patch(void (*observer)(std::uintptr_t address, std::size_t bytes, bool end));
void GuestArenaSetHostReadHook_nid_no_patch(void (*hook)(std::uintptr_t address, std::size_t bytes));
void GuestArenaPrepareHostRead_nid_no_patch(const void* pointer, std::size_t bytes);

}

class HostWrite {
public:
    HostWrite(void* pointer, std::size_t bytes) : pointer(pointer), bytes(bytes), open(GuestArenaBeginHostWrite_nid_postfix(pointer, bytes)) {}
    ~HostWrite() {
        if (open) GuestArenaEndHostWrite_nid_postfix(pointer, bytes);
    }
    HostWrite(const HostWrite&) = delete;
    HostWrite& operator=(const HostWrite&) = delete;
    bool Open() const { return open; }

private:
    void* pointer;
    std::size_t bytes;
    bool open;
};

#ifdef _WIN32
class HostImportAccess {
public:
    HostImportAccess(void* pointer, std::size_t bytes) : pointer(pointer), bytes(bytes), open(GuestArenaBeginHostImport_nid_no_patch(pointer, bytes)) {}
    ~HostImportAccess() {
        if (open) GuestArenaEndHostImport_nid_no_patch(pointer, bytes);
    }
    HostImportAccess(const HostImportAccess&) = delete;
    HostImportAccess& operator=(const HostImportAccess&) = delete;

private:
    void* pointer;
    std::size_t bytes;
    bool open;
};
#endif

}

#endif
