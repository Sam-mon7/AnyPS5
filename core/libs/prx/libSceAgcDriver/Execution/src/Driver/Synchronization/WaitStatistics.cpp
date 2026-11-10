#include "prx/libSceAgcDriver/Execution/include/ThreadScratch.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

PollStats& Driver::pollStats() {
    struct StatsStorage {};
    return AgcDriver::ThreadScratch<PollStats, StatsStorage>();
}

WaitOutcomes& Driver::waitOutcomes() {
    struct OutcomesStorage {};
    return AgcDriver::ThreadScratch<WaitOutcomes, OutcomesStorage>();
}

Graphics::Recorder::LateStatistics& Driver::lateCountsSeen() {
    struct SeenStorage {};
    return AgcDriver::ThreadScratch<Graphics::Recorder::LateStatistics, SeenStorage>();
}

EpochBumps& Driver::epochBumps() {
    struct BumpsStorage {};
    return AgcDriver::ThreadScratch<EpochBumps, BumpsStorage>();
}

std::chrono::steady_clock::time_point& Driver::lastEpochBump() {
    struct AtStorage {};
    return AgcDriver::ThreadScratch<std::chrono::steady_clock::time_point, AtStorage>();
}

void Driver::bumpEpoch(std::uint64_t EpochBumps::*counter, GuestMemory::Gen::TicketReason reason) {
    lastEpochBump() = std::chrono::steady_clock::now();
    using GuestMemory::Gen::TicketReason;
    if (reason == TicketReason::Other) {
        reason = counter == &EpochBumps::submissions ? TicketReason::Submission : counter == &EpochBumps::packets ? TicketReason::Packet : counter == &EpochBumps::drains ? TicketReason::Drain : counter == &EpochBumps::reaps ? TicketReason::Reap : TicketReason::WaitCpu;
    }
    GuestMemory::BumpCollectEpoch(reason);
    if (DrawPipeline::Active()) {
        const auto token = DrawPipeline::EpochToken().fetch_add(1, std::memory_order_relaxed) + 1;
        DrawPipeline::NoteEpochClock(token, GuestMemory::ThreadCollectEpoch());
        DrawPipeline::CountEpochBump(counter == &EpochBumps::submissions ? DrawPipeline::BumpSubmission : counter == &EpochBumps::waits ? DrawPipeline::BumpWait : counter == &EpochBumps::drains ? DrawPipeline::BumpDrain : counter == &EpochBumps::reaps ? DrawPipeline::BumpReap : DrawPipeline::BumpPacket);
    }
    ++(epochBumps().*counter);
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
