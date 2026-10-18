#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_SUBMISSIONCURSOR_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_SUBMISSIONCURSOR_HPP

#include "Scheduler.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"

namespace AgcDriver::DriverDetail::Lanes {

struct SubmissionCursor {
    Submission submission;
    std::vector<std::uint64_t> writes;
    std::size_t cursor = 0;
    std::optional<std::size_t> predicateChecked;
    bool started = false;

    std::span<const std::uint32_t> Packet() const {
        return std::span(submission.commands).subspan(cursor, Pm4::PacketWords(submission.commands.at(cursor)));
    }

    std::size_t End() const {
        std::size_t end = cursor, draws = 0;
        while (end < submission.commands.size()) {
            const auto header = submission.commands[end];
            const auto opcode = (header >> 8u) & 0xffu;
            const auto count = Pm4::PacketWords(header);
            if (!Pm4::FillerPacket(header) && (opcode == 0x3c || opcode == 0x93 || opcode == 0x22 || opcode == 0x3f)) break;
            end += count;
            if (Pm4::FillerPacket(header)) continue;
            if (Pm4::DrawOpcode(opcode) && ++draws == 128) break;
            if (header == FlipPacketHeader || header == RenderingWaitPacketHeader || opcode == 0x42 || opcode == 0x46 || opcode == 0x49 || opcode == 0x58) break;
        }
        return end;
    }

    Submission Slice(std::size_t end) const {
        Submission result{};
        result.serial = submission.serial;
        result.queue = submission.queue;
        result.shaders = submission.shaders;
        result.received = submission.received;
        result.receivedAt = submission.receivedAt;
        result.copiedAt = submission.copiedAt;
        result.validatedAt = submission.validatedAt;
        result.roomReadyAt = submission.roomReadyAt;
        result.enqueuedAt = submission.enqueuedAt;
        result.dequeuedAt = submission.dequeuedAt;
        result.orderedAt = submission.orderedAt;
        result.commands.assign(submission.commands.begin() + cursor, submission.commands.begin() + end);
        for (const auto& [at, flip] : submission.flips) if (at >= cursor && at < end) result.flips.emplace(at - cursor, flip);
        for (const auto& [at, wait] : submission.renderingWaits) if (at >= cursor && at < end) result.renderingWaits.emplace(at - cursor, wait);
        for (const auto& [at, registers] : submission.registerLists) if (at >= cursor && at < end) result.registerLists.emplace(at - cursor, registers);
        result.holdsFlip = submission.holdsFlip && !result.flips.empty();
        return result;
    }
};

}
#endif
