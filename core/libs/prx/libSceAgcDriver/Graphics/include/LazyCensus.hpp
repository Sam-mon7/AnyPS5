#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LAZYCENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LAZYCENSUS_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace AgcDriver::Graphics::ArchC::LazyCensus {

enum class Kind : std::uint8_t { StagingCopyBack, HeapReadback, WriteBack, ShadowPublish, Count };
// StagingInSame (P5b): the copy-in of a staged region by a later use of the same build (RecordStagingCopies),
// so into the same shadow the copy-back came from; StagingIn is then a copy-in by another build.
enum class GpuUse : std::uint8_t { StagingIn, Carry, Read, AddressRead, Write, LabelStore, StagingInSame, Count };
enum class Outcome : std::uint8_t { GuestRead, GuestWrite, DriverRead, DriverWrite, Os, Forget, StagingIn, Carry, GpuRead, GpuAddressRead, GpuWrite, LabelStore, Republished, Expired, Unwatchable, StagingInSame, CarryCross, StagingInSameCross, Count };

bool Enabled();
std::uint8_t ReasonFor(const char* flushReason);
// `serial` (P5b, copy-backs and their carry/copy-in uses): the recorder batch; a same-shadow use in a later
// batch than the copy-back closes as CarryCross / StagingInSameCross.
void Published(std::uint64_t begin, std::uint64_t end, Kind kind, std::uint8_t reason, std::uint64_t serial = 0);
void UsedByGpu(std::uint64_t begin, std::uint64_t end, GpuUse use, std::uint64_t serial = 0);
void UsedByGpu(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, GpuUse use);
void LabelFlush(std::uint64_t ns, std::uint64_t waitedNs, bool queue0);
void LabelStored(bool queue0);
void NoteFenceWait(std::uint64_t ns);
std::uint64_t ThreadFenceWaitNs();
void Flip();
void Drain();

struct UseScope {
    UseScope();
    ~UseScope();
    UseScope(const UseScope&) = delete;
    UseScope& operator=(const UseScope&) = delete;
    bool active;
};

struct Totals {
    std::uint64_t count[static_cast<std::size_t>(Kind::Count)][static_cast<std::size_t>(Outcome::Count)];
    std::uint64_t bytes[static_cast<std::size_t>(Kind::Count)][static_cast<std::size_t>(Outcome::Count)];
    std::uint64_t watchedPages;
    std::uint64_t pages;
    std::uint64_t faults;
    std::uint64_t eagerTouches;
};
Totals Snapshot();
void ResetForTest();

}

#endif
