#include "prx/libSceAgcDriver/Execution/include/Timeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"
#include "ThreadOwned.hpp"
#include <algorithm>
#include <cstdlib>

namespace AgcDriver::DriverDetail {

DeferredLabels& deferredLabels() {
    struct Slot {};
    auto* deferred = HostThreadSlot<DeferredLabels*, Slot>::Get();
    if (deferred == nullptr) {
        ShaderRecompiler::ThreadOwned(deferred);
        HostThreadSlot<DeferredLabels*, Slot>::Set(deferred);
    }
    return *deferred;
}

std::vector<std::uint32_t>& deferredLabelOpcodes() {
    static thread_local std::vector<std::uint32_t>* opcodes = nullptr;
    return ShaderRecompiler::ThreadOwned(opcodes);
}

bool DeferLabels() {

    static const bool defer = std::getenv("APS5_LABEL_LOCK_EACH") == nullptr && std::getenv("APS5_DRAIN_COMPLETION_LABELS") == nullptr;
    return defer;
}

bool QueuedLabelTable() {
    static const bool enabled = std::getenv("APS5_NO_QUEUED_LABELS") == nullptr && std::getenv("APS5_NO_LABEL_BATCHING") == nullptr;
    return enabled;
}

bool LabelBatchSubmit() {
    static const bool enabled = std::getenv("APS5_NO_LABEL_BATCH_SUBMIT") == nullptr && std::getenv("APS5_NO_LABEL_BATCHING") == nullptr;
    return enabled;
}

std::size_t& queuedLabelsNoted() {
    static thread_local std::size_t noted = 0;
    return noted;
}

bool NeedsRecordedLabels(std::uint32_t header) {
    if (header == FlipPacketHeader) return true;
    switch ((header >> 8u) & 0xffu) {
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x26: case 0x28: case 0x2a: case 0x2f:
        case 0x42: case 0x46: case 0x58: case 0x68: case 0x69: case 0x76: case 0x78: case 0x79: case 0x7a: case 0x81:
            return false;
        default: return true;
    }
}

bool PacketLocksItself(std::uint32_t header) {
    if (header == FlipPacketHeader) return true;
    switch ((header >> 8u) & 0xffu) {
        case 0x15: case 0x16: case 0x27: case 0x2d: case 0x35: case 0x24: case 0x25: case 0x2c: case 0x38: return true;
        default: return false;
    }
}

bool Driver::labelTryEachPacket() {
    static const bool each = std::getenv("APS5_LABEL_TRY_EACH_PACKET") != nullptr;
    return each;
}

std::chrono::microseconds Driver::labelFlushDeadline() {
    static const std::chrono::microseconds value = [] {
        const char* text = std::getenv("APS5_LABEL_FLUSH_US");
        return std::chrono::microseconds(text ? std::atoi(text) : (LabelBatchSubmit() ? 1000 : 250));
    }();
    return value;
}

bool Driver::labelFlushDue() {
    if (QueuedLabelTable() && !deferredLabels().labels.empty()) Get().recordQueuedLabelsByTry(GuestMemory::GpuLockThreadTag(), pollLabelRecords);
    const auto since = Graphics::Recorder::PendingLabelSince();
    return since.has_value() && std::chrono::steady_clock::now() - *since >= labelFlushDeadline();
}

bool Driver::completionsPending() {
    return Graphics::Recorder::PendingCompletionLabels() != 0 || Graphics::Recorder::PendingWriteBackCompletions() != 0;
}

bool Driver::reapEachPacket() {

    static const bool each = std::getenv("APS5_REAP_EACH_PACKET") != nullptr;
    return each;
}

std::uint64_t Driver::batchCap() {
    static const std::uint64_t value = [] {
        const char* text = std::getenv("APS5_BATCH_CAP");
        return text ? std::strtoull(text, nullptr, 10) : 64ull;
    }();
    return value;
}

void Driver::flushAwaitedLabels(std::uint32_t queue) {
    static const bool enabled = std::getenv("APS5_NO_AWAITED_LABEL_FLUSH") == nullptr;
    if (!enabled) return;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto ownSlot = queue == 0 ? 0 : queue - 0x20 + 1;
    const auto& labels = deferredLabels().labels;
    for (std::size_t index = 0; index < awaitedSlots.size(); ++index) {
        if (index == ownSlot) continue;
        const auto& slot = awaitedSlots[index];
        const auto address = slot.address.load(std::memory_order_acquire);
        if (address == 0) continue;
        const auto bytes = slot.shape.load(std::memory_order_relaxed) >> 32u;
        if (bytes != 4 && bytes != 8) continue;
        const bool overlaps = std::any_of(labels.begin(), labels.end(), [&](const DeferredLabel& label) {
            return address >= label.address ? address - label.address < label.size : label.address - address < bytes;
        });
        if (!overlaps) continue;
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
        std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::try_to_lock);
        if (!gpuLock.owns_lock()) {
            if (profile) ++waitOutcomes().awaitedLabelBusy;
            return;
        }
        const auto localDevice = device.Load();
        if (localDevice == nullptr) return;
        recordDeferredLabels(localDevice.get(), queue);
        localDevice->SubmitRecorded(false);
        if (profile) ++waitOutcomes().awaitedLabelFlushes;
        return;
    }
}

void Driver::recordDeferredLabels(VulkanDevice* localDevice, std::uint32_t queue) {
    auto& deferred = deferredLabels();
    if (deferred.labels.empty()) return;

    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto* opcodes = profile ? &deferredLabelOpcodes() : nullptr;
    static thread_local std::vector<DeferredLabel>* recording = nullptr;
    auto& labels = ShaderRecompiler::ThreadOwned(recording);
    labels.clear();
    labels.swap(deferred.labels);
    Timeline::DeferredScope timelineOrigins;
    struct Clear {
        std::vector<DeferredLabel>& labels;
        std::uint32_t queue;
        std::vector<std::uint32_t>* opcodes;
        std::chrono::steady_clock::time_point start;
        double historyUs = 0;
        ~Clear() {
            const auto closeStart = opcodes != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const auto count = labels.size();
            const bool failed = std::uncaught_exceptions() != 0;
            if (failed) std::fprintf(stderr, "[gpu] queue 0x%x dropped %zu queued labels: their record failed\n", queue, labels.size());
            labels.clear();
            if (opcodes != nullptr) opcodes->clear();

            Graphics::Recorder::CloseLabelGroup(failed ? 0 : GuestMemory::TrackerGeneration());

            Graphics::Recorder::ForgetQueuedLabels();
            queuedLabelsNoted() = 0;
            if (opcodes != nullptr) {
                struct Totals {
                    std::uint64_t groups = 0, labels = 0;
                    double totalUs = 0, historyUs = 0, closeUs = 0;
                    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
                };
                static thread_local Totals totals;
                const auto now = std::chrono::steady_clock::now();
                ++totals.groups;
                totals.labels += count;
                totals.totalUs += std::chrono::duration<double, std::micro>(now - start).count();
                totals.historyUs += historyUs;
                totals.closeUs += std::chrono::duration<double, std::micro>(now - closeStart).count();
                if (now - totals.since < std::chrono::seconds(10)) return;
                AgcDriver::ProfilePrint_nid_no_patch("[label-groups] thread 0x%x: %llu groups, %llu labels, %.2f labels/group, total %.0f ms, history %.0f ms, close/forget %.0f ms (%.0f s)\n", GuestMemory::GpuLockThreadTag(), static_cast<unsigned long long>(totals.groups), static_cast<unsigned long long>(totals.labels), static_cast<double>(totals.labels) / totals.groups, totals.totalUs / 1000.0, totals.historyUs / 1000.0, totals.closeUs / 1000.0, std::chrono::duration<double>(now - totals.since).count());
                totals = {};
            }
        }
    } clear{labels, queue, opcodes, profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
    bool first = true;
    for (const auto& label : labels) {
        auto timelineContext = timelineOrigins.At(static_cast<std::size_t>(&label - labels.data()));
        const auto packet = profile ? GuestMemory::CurrentPacket() : GuestMemory::PacketTag{};
        if (profile) GuestMemory::SetCurrentPacket(opcodes->at(static_cast<std::size_t>(&label - labels.data())), queue);
        struct RestorePacket {
            bool enabled;
            GuestMemory::PacketTag packet;
            ~RestorePacket() { if (enabled) GuestMemory::SetCurrentPacket(packet.opcode, packet.queue); }
        } restorePacket{profile, packet};
        const auto bytes = std::span<const std::byte>(label.bytes).first(label.size);
        const auto stamp = ++eventSerial;
        const int reason = localDevice != nullptr ? localDevice->WriteLabelOnGpu(label.address, bytes, stamp, queue, first) : 4;
        first = false;
        countLabelOutcome(reason);
        if (reason != 0 && reason != 5) {
            if (reason != 1 && localDevice != nullptr) localDevice->WaitIdle();
            GuestMemory::Write(label.address, bytes, 4);
        }
        const auto historyStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        noteLabelStore(label.address, bytes, stamp, queue);
        if (profile) clear.historyUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - historyStart).count();
    }
    ++labelGroups;
    if (profile && (labelGroups.load(std::memory_order_relaxed) & 63u) == 0 && std::chrono::steady_clock::now() - lastSyncReport > std::chrono::seconds(10)) {
        lastSyncReport = std::chrono::steady_clock::now();
        reportSync();
    }
}

bool Driver::recordLabelsForPacket(VulkanDevice* localDevice, std::uint32_t queue) {
    static const bool packetFlush = !LabelBatchSubmit() && std::getenv("APS5_NO_LABEL_PACKET_FLUSH") == nullptr;

    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (!deferredLabels().labels.empty()) {
        recordDeferredLabels(localDevice, queue);
        ++packetLockRecords;
        if (profile) packetLockRecordUs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    }
    if (localDevice == nullptr || !packetFlush || !Graphics::Recorder::PendingLabelSince().has_value()) return false;

    const auto submitStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    localDevice->SubmitRecorded(queue == 0);
    ++packetLockSubmits;
    if (profile) packetLockSubmitUs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - submitStart).count());
    return true;
}

void Driver::recordQueuedLabelsBeforeRead(std::uint32_t queue) {
    if (deferredLabels().labels.empty()) return;
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    const auto localDevice = device.Load();
    recordDeferredLabels(localDevice.get(), queue);
}

bool Driver::recordQueuedLabelsAfterCapture(std::uint32_t queue, std::span<const ShaderRecompiler::MemoryRegion> regions) {
    const auto& labels = deferredLabels().labels;
    if (labels.empty()) return false;
    for (const auto& label : labels) {
        for (const auto& region : regions) {
            if (label.address < region.guestAddress + region.bytes.size() && region.guestAddress < label.address + label.size) {
                recordQueuedLabelsBeforeRead(queue);
                ++captureRetries;
                return true;
            }
        }
    }
    static const bool batch = std::getenv("APS5_NO_CAPTURE_LABEL_LOCK_BATCHING") == nullptr;
    if (batch && !DrawPipeline::Active()) {
        static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        if (profile) ++captureTriesBatched;
        return false;
    }
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
    if (!gpuLock.try_lock()) {
        ++triesFailed[TryCapture];
        return false;
    }
    const auto localDevice = device.Load();
    recordDeferredLabels(localDevice.get(), queue);
    ++captureTryRecords;
    return false;
}

void Driver::noteQueuedLabels(std::uint32_t queue) {
    static const bool installed = (Graphics::Recorder::SetQueuedLabelRecorder(&Driver::recordQueuedLabelsFromHook), true);
    (void)installed;
    const auto& labels = deferredLabels().labels;
    for (auto& noted = queuedLabelsNoted(); noted < labels.size(); ++noted) {
        const auto& label = labels[noted];
        const auto origin = Timeline::Enabled && noted < Timeline::DeferredOrigins().size() ? Timeline::DeferredOrigins()[noted] : Timeline::Origin{};
        Timeline::ContextScope timelineContext(origin.packet, origin.opcode);
        Graphics::Recorder::NoteQueuedLabel(label.address, std::span<const std::byte>(label.bytes).first(label.size), ++eventSerial, queue);
    }
}

void Driver::recordQueuedLabelsByTry(std::uint32_t queue, std::atomic<std::uint64_t>& counter) {
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
    if (!gpuLock.try_lock()) {
        ++triesFailed[TryPollRecord];
        return;
    }
    const auto localDevice = device.Load();
    recordDeferredLabels(localDevice.get(), queue);
    ++counter;
}

void Driver::recordQueuedLabelsFromHook() {
    if (deferredLabels().labels.empty()) return;
    auto& driver = Get();
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    const auto localDevice = driver.device.Load();
    driver.recordDeferredLabels(localDevice.get(), GuestMemory::GpuLockThreadTag());
    ++hookLabelRecords;
}

}
