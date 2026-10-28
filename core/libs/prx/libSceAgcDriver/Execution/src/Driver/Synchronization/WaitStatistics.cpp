#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

PollStats& Driver::pollStats() {
    static thread_local PollStats stats;
    return stats;
}

WaitOutcomes& Driver::waitOutcomes() {
    static thread_local WaitOutcomes outcomes;
    return outcomes;
}

Graphics::Recorder::LateStatistics& Driver::lateCountsSeen() {
    static thread_local Graphics::Recorder::LateStatistics seen{};
    return seen;
}

EpochBumps& Driver::epochBumps() {
    static thread_local EpochBumps bumps;
    return bumps;
}

std::chrono::steady_clock::time_point& Driver::lastEpochBump() {
    static thread_local std::chrono::steady_clock::time_point at{};
    return at;
}

void Driver::bumpEpoch(std::uint64_t EpochBumps::*counter, GuestMemory::CollectEpochSource source, std::uint64_t producerFloor) {
    lastEpochBump() = std::chrono::steady_clock::now();
    if (source == GuestMemory::CollectEpochSource::Other) {
        if (counter == &EpochBumps::submissions) source = GuestMemory::CollectEpochSource::Submission;
        else if (counter == &EpochBumps::drains) source = GuestMemory::CollectEpochSource::Drain;
        else if (counter == &EpochBumps::reaps) source = GuestMemory::CollectEpochSource::Reap;
        else if (counter == &EpochBumps::packets) source = GuestMemory::CollectEpochSource::Packet;
    }
    GuestMemory::BumpCollectEpoch(source, producerFloor);
    if (DrawPipeline::Active()) DrawPipeline::EpochToken().fetch_add(1, std::memory_order_relaxed);
    ++(epochBumps().*counter);
}

std::uint64_t Driver::entryWaitFloor(std::span<const std::uint32_t> packet, std::uint32_t queue, std::uint64_t received) {
    using Reason = GuestMemory::CollectEntryReason;
    static const bool enabled = std::getenv("APS5_NO_COLLECT_EPOCH_FLOOR") == nullptr;
    Reason reason = Reason::Disabled;
    const auto proof = [&]() -> std::uint64_t {
        if (!enabled) return 0;
        std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::try_to_lock);
        if (!gpuLock.owns_lock()) { reason = Reason::LockBusy; return 0; }
        const auto bytes = Pm4::WaitAwaitedBytes(packet);
        const auto address = (packet[2] & (bytes == 8 ? ~7u : ~3u)) | (static_cast<std::uint64_t>(packet[3]) << 32u);
        const auto hit = Graphics::Recorder::LookupEntryLabel(address, bytes, received, queue, reason);
        if (!hit.has_value()) return 0;
        if (!Pm4::WaitComparesValue(packet, hit->value)) { reason = Reason::ValueMismatch; return 0; }
        const auto keysMatch = [&] {
            return hit->collectUnwatched == GuestMemory::UnwatchSerial() && hit->collectMapping == GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        };
        if (!keysMatch()) { reason = Reason::ChangedKey; return 0; }
        const std::uint64_t current = bytes == 8 ? *reinterpret_cast<const volatile std::uint64_t*>(address) : *reinterpret_cast<const volatile std::uint32_t*>(address);
        if (current != hit->value) { reason = Reason::ValueMismatch; return 0; }
        bool collected = false;
        const bool unchanged = GuestMemory::UnchangedSinceCollected(address, bytes, hit->generation, &collected);
        if (!keysMatch()) { reason = Reason::ChangedKey; return 0; }
        if (!unchanged) { reason = collected ? Reason::CpuStore : Reason::Unwatched; return 0; }
        return hit->collectFloor;
    }();
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (profile) GuestMemory::CountCollectEntryReason(reason);
    return proof;
}

bool Driver::skipHeldWaitEpoch(const Submission& submission, std::size_t cursor) {
    static const bool enabled = std::getenv("APS5_NO_HELD_WAIT_EPOCH") == nullptr;
    if (!enabled || !submission.heldAtSubmit.contains(cursor) || submission.heldAtSubmitAt == std::chrono::steady_clock::time_point{} || submission.heldAtSubmitAt >= lastEpochBump()) return false;
    ++waitOutcomes().heldEpochsSkipped;
    return true;
}

bool Driver::packetEpoch() {
    static const bool packet = std::getenv("APS5_PACKET_EPOCH") != nullptr;
    return packet;
}

}
