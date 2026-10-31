#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTARENA_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTARENA_HPP

#include <cstddef>
#include <cstdint>
#include <chrono>

// Guest virtual memory is placed inside one reserved arena below the PS5 application map limit
// absolute address and breaks on host addresses outside that range.
namespace GuestArena {

enum class CensusOperation : std::uint8_t {
    Query, QueryReset, Reset, VirtualQuery, VirtualProtect, Collection, Walk,
    PageStamps, BlockStamps, CoarseStamps, EpochMemo, EpochMiss, Count
};
using CensusCallback = std::uint64_t (*)(CensusOperation, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, bool);
extern "C" void GuestArenaSetCensusCallback_nid_no_patch(CensusCallback callback);
extern "C" CensusCallback GuestArenaCensusCallback_nid_no_patch();

class CensusTimer {
public:
    using Clock = std::chrono::steady_clock;
    explicit CensusTimer(CensusOperation operation, std::uint64_t pages = 0) : callback(GuestArenaCensusCallback_nid_no_patch()), operation(operation), pages(pages) {
        if (callback != nullptr) {
            children = callback(CensusOperation::Count, 0, 0, 0, 0, false);
            start = Clock::now();
        }
    }
    ~CensusTimer() { Finish(); }
    CensusTimer(const CensusTimer&) = delete;
    CensusTimer& operator=(const CensusTimer&) = delete;
    bool Active() const { return callback != nullptr; }
    void Result(std::uint64_t covered, std::uint64_t dirty = 0, bool failed = false) {
        pages = covered;
        reported = dirty;
        error = failed;
    }
    void Finish() {
        if (callback == nullptr) return;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        callback(operation, pages, reported, ns, children, error);
        callback = nullptr;
    }
    static std::uint64_t Pages(std::uint64_t address, std::uint64_t bytes) {
        return bytes == 0 ? 0 : bytes / 4096 + ((address % 4096 + bytes % 4096 + 4095) / 4096);
    }
private:
    CensusCallback callback;
    CensusOperation operation;
    std::uint64_t pages;
    std::uint64_t reported = 0;
    std::uint64_t children = 0;
    bool error = false;
    Clock::time_point start{};
};

#ifdef _WIN32
extern "C" unsigned int GuestArenaCensusGetWriteWatch_nid_no_patch(unsigned long flags, void* address, std::size_t bytes, void** pages, std::uintptr_t* count, unsigned long* granularity);
extern "C" unsigned int GuestArenaCensusResetWriteWatch_nid_no_patch(void* address, std::size_t bytes);
extern "C" std::size_t GuestArenaCensusVirtualQuery_nid_no_patch(const void* address, void* info, std::size_t bytes);
extern "C" int GuestArenaCensusVirtualProtect_nid_no_patch(void* address, std::size_t bytes, unsigned long protection, unsigned long* previous);
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
bool GuestArenaHostRegionOverlaps_nid_postfix(std::uintptr_t address, std::size_t bytes);
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
