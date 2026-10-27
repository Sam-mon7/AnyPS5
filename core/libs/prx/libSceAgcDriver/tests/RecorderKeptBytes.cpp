#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

constexpr std::size_t Budget = Recorder::KeptBytesBudget;

struct MockDevice {
    std::uintptr_t next = 1;
    std::map<VkFence, bool> signaled;
    std::uint64_t submits = 0;
    std::uint64_t fenceWaits = 0;
    std::uint64_t allocatedCommands = 0;
    std::uint64_t freedCommands = 0;
    VkResult endResult = VK_SUCCESS;
    VkResult submitResult = VK_SUCCESS;
    bool lostSubmissionPending = false;
    std::uint64_t idleWaits = 0;
    VkResult idleResult = VK_ERROR_DEVICE_LOST;
    std::map<VkFence, std::uint64_t> serials;
    std::thread::id waitingThread;
    std::function<void()> duringWait;
    VkResult timelineResult = VK_SUCCESS;
    std::uint64_t timelineWaits = 0;
    std::string commands;
    VkPipelineStageFlags barrierSource = 0, barrierDestination = 0;
    VkAccessFlags barrierWrites = 0, barrierReads = 0;
};

MockDevice mock;
std::atomic<bool> timelineWaitInProgress{false};

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* commands) {
    *commands = reinterpret_cast<VkCommandBuffer>(mock.next++);
    ++mock.allocatedCommands;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockFreeCommandBuffers(VkDevice, VkCommandPool, std::uint32_t count, const VkCommandBuffer*) {
    Expect(!mock.lostSubmissionPending, "command buffers were freed before the lost submission finished");
    mock.freedCommands += count;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence) {
    *fence = reinterpret_cast<VkFence>(mock.next++);
    mock.signaled[*fence] = false;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*) {
    Expect(!mock.lostSubmissionPending, "a fence was destroyed before the lost submission finished");
    mock.signaled.erase(fence);
    mock.serials.erase(fence);
}

VKAPI_ATTR VkResult VKAPI_CALL mockGetFenceStatus(VkDevice, VkFence fence) {
    return mock.signaled.at(fence) ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL mockWaitForFences(VkDevice, std::uint32_t count, const VkFence* fences, VkBool32, std::uint64_t) {
    for (std::uint32_t i = 0; i < count; ++i) mock.signaled.at(fences[i]) = true;
    ++mock.fenceWaits;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockResetFences(VkDevice, std::uint32_t count, const VkFence* fences) {
    Expect(!mock.lostSubmissionPending, "a fence was reset before the lost submission finished");
    for (std::uint32_t i = 0; i < count; ++i) mock.signaled.at(fences[i]) = false;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockEndCommandBuffer(VkCommandBuffer) {
    return mock.endResult;
}

VKAPI_ATTR VkResult VKAPI_CALL mockQueueSubmit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence fence) {
    ++mock.submits;
    mock.serials[fence] = mock.submits;
    if (mock.submitResult == VK_ERROR_DEVICE_LOST) mock.lostSubmissionPending = true;
    return mock.submitResult;
}

VKAPI_ATTR VkResult VKAPI_CALL mockDeviceWaitIdle(VkDevice) {
    ++mock.idleWaits;
    mock.lostSubmissionPending = false;
    return mock.idleResult;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* semaphore) {
    *semaphore = reinterpret_cast<VkSemaphore>(mock.next++);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {
    Expect(!timelineWaitInProgress.load(), "a timeline was destroyed during its registered wait");
}

VKAPI_ATTR VkResult VKAPI_CALL mockCounterValue(VkDevice, VkSemaphore, std::uint64_t* value) {
    *value = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateQueryPool(VkDevice, const VkQueryPoolCreateInfo*, const VkAllocationCallbacks*, VkQueryPool*) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL mockWaitSemaphores(VkDevice, const VkSemaphoreWaitInfo* info, std::uint64_t) {
    if (std::this_thread::get_id() != mock.waitingThread) return VK_SUCCESS;
    Expect(!AgcDriver::GuestMemory::GpuMutex().HeldByThisThread(), "the timeline wait retained the GPU mutex");
    if (AgcDriver::GuestMemory::GpuMutex().HeldByThisThread()) return VK_ERROR_UNKNOWN;
    ++mock.timelineWaits;
    timelineWaitInProgress.store(true);
    struct EndWait {
        ~EndWait() { timelineWaitInProgress.store(false); }
    } endWait;
    for (const auto& [fence, serial] : mock.serials) {
        if (serial <= info->pValues[0]) mock.signaled.at(fence) = true;
    }
    if (auto action = std::exchange(mock.duringWait, {})) action();
    return mock.timelineResult;
}

VKAPI_ATTR void VKAPI_CALL mockCmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) { mock.commands += 'U'; }
VKAPI_ATTR void VKAPI_CALL mockCmdEndRenderPass(VkCommandBuffer) { mock.commands += 'E'; }
VKAPI_ATTR void VKAPI_CALL mockCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags source, VkPipelineStageFlags destination, VkDependencyFlags, std::uint32_t count, const VkMemoryBarrier* barriers, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {
    mock.commands += 'B';
    mock.barrierSource = source;
    mock.barrierDestination = destination;
    if (count != 0) {
        mock.barrierWrites = barriers[0].srcAccessMask;
        mock.barrierReads = barriers[0].dstAccessMask;
    }
}
VKAPI_ATTR void VKAPI_CALL mockCmdBeginQuery(VkCommandBuffer, VkQueryPool, std::uint32_t, VkQueryControlFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdEndQuery(VkCommandBuffer, VkQueryPool, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdResetQueryPool(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL mockCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t, const void*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdDispatch(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t) { mock.commands += 'D'; }

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateCommandBuffers)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(mockFreeCommandBuffers)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(mockCreateFence)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyFence)},
        {"vkGetFenceStatus", reinterpret_cast<PFN_vkVoidFunction>(mockGetFenceStatus)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(mockWaitForFences)},
        {"vkResetFences", reinterpret_cast<PFN_vkVoidFunction>(mockResetFences)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockBeginCommandBuffer)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockEndCommandBuffer)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(mockQueueSubmit)},
        {"vkDeviceWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(mockDeviceWaitIdle)},
        {"vkCreateSemaphore", reinterpret_cast<PFN_vkVoidFunction>(mockCreateSemaphore)},
        {"vkDestroySemaphore", reinterpret_cast<PFN_vkVoidFunction>(mockDestroySemaphore)},
        {"vkWaitSemaphoresKHR", reinterpret_cast<PFN_vkVoidFunction>(mockWaitSemaphores)},
        {"vkGetSemaphoreCounterValueKHR", reinterpret_cast<PFN_vkVoidFunction>(mockCounterValue)},
        {"vkCreateQueryPool", reinterpret_cast<PFN_vkVoidFunction>(mockCreateQueryPool)},
        {"vkCmdUpdateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCmdUpdateBuffer)},
        {"vkCmdEndRenderPass", reinterpret_cast<PFN_vkVoidFunction>(mockCmdEndRenderPass)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPipelineBarrier)},
        {"vkCmdBeginQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBeginQuery)},
        {"vkCmdEndQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdEndQuery)},
        {"vkCmdResetQueryPool", reinterpret_cast<PFN_vkVoidFunction>(mockCmdResetQueryPool)},
        {"vkCmdCopyQueryPoolResults", reinterpret_cast<PFN_vkVoidFunction>(mockCmdCopyQueryPoolResults)},
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBindPipeline)},
        {"vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPushConstants)},
        {"vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(mockCmdDispatch)},
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

Context mockContext() {
    Context context{};
    context.device = reinterpret_cast<VkDevice>(mock.next++);
    context.queue = reinterpret_cast<VkQueue>(mock.next++);
    context.deviceProc = mockProc;
    return context;
}

void ComputeChainTests() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    AgcDriver::GuestMemory::SetCurrentPacket(0x15, 0);
    const auto context = mockContext();
    Recorder recorder(context);
    const auto* flag = std::getenv("APS5_COMPUTE_CHAINS");
    const bool tracked = Recorder::ComputeChainsEnabled() && flag != nullptr && std::string_view(flag) == "1";
    const bool ds = Recorder::DsComputeChains("PPSA01341");
    Expect(!Recorder::DsComputeChains("") && !Recorder::DsComputeChains("PPSA00000"), "an unknown title enabled unchecked chains");
    Expect(Recorder::DsComputeChains("PPSA01340") == ds, "the second supported title did not follow the experiment switch");
    const auto dispatch = [&](const PassAccess& access, bool eligible = true, std::string_view title = {}) {
        const auto start = recorder.BeginComputeDispatch(access, eligible, title);
        constexpr VkAccessFlags shader = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        if (!start.continued && (!Recorder::MergeBarriers() || (start.covered & shader) != shader)) RecordMemoryBarrier(context, start.commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, shader);
        mockCmdDispatch(start.commands, 1, 1, 1);
        if (start.deferred) recorder.DeferComputeBarrier(access);
        else {
            RecordMemoryBarrier(context, start.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, shader | VK_ACCESS_HOST_READ_BIT);
            recorder.MarkCovered(shader | VK_ACCESS_HOST_READ_BIT);
        }
        return start;
    };
    const std::pair<std::uint64_t, std::uint64_t> a{0x1000, 0x1100}, b{0x2000, 0x2100}, overlap{0x1080, 0x1200};
    const PassAccess writeA{{}, std::span(&a, 1), {}, {}}, writeB{{}, std::span(&b, 1), {}, {}}, readA{std::span(&a, 1), {}, {}, {}};
    const auto reset = [&] { recorder.Sync(); mock.commands.clear(); };
    Expect(dispatch(writeA).deferred == tracked, "tracked mode admission did not match its switch");
    Expect(dispatch(writeB).continued == tracked, "disjoint dispatches did not form a chain");
    const auto beforeDependent = mock.commands.rfind('D');
    Expect(!dispatch(readA).continued, "a dependent dispatch pair dropped its RAW barrier across a disjoint dispatch");
    Expect(mock.commands.find('B', beforeDependent + 1) < mock.commands.rfind('D'), "a dependent dispatch was recorded before its barrier");
    reset();
    for (const auto& earlier : {readA, writeA}) {
        dispatch(earlier);
        const PassAccess later{{}, std::span(&overlap, 1), {}, {}};
        Expect(!dispatch(later).continued, "a WAR or WAW overlap lost its barrier");
        reset();
    }
    dispatch(readA);
    Expect(dispatch(readA).continued == tracked, "read/read accesses closed a tracked chain");
    reset();
    const auto image = reinterpret_cast<VkImage>(std::uintptr_t{0x1234});
    const std::pair<VkImage, bool> imageWrite{image, true}, imageRead{image, false};
    dispatch({{}, {}, std::span(&imageWrite, 1), {}});
    dispatch(writeB);
    Expect(!dispatch({{}, {}, std::span(&imageRead, 1), {}}).continued, "an image RAW hazard crossed the chain");
    reset();
    dispatch({{}, {}, std::span(&imageRead, 1), {}});
    Expect(!dispatch({{}, {}, std::span(&imageWrite, 1), {}}).continued, "an image WAR hazard crossed the chain");
    reset();
    dispatch({{}, {}, std::span(&imageWrite, 1), {}});
    Expect(!dispatch({{}, {}, std::span(&imageWrite, 1), {}}).continued, "an image WAW hazard crossed the chain");
    reset();
    dispatch({{}, std::span(&a, 1), std::span(&imageWrite, 1), {}});
    Expect(!dispatch(readA).continued, "an image backing range aliased by a buffer lost its barrier");
    reset();
    const std::pair<std::uint64_t, std::uint64_t> arguments{0x1008, 0x1014};
    dispatch(writeA);
    Expect(!dispatch({std::span(&arguments, 1), {}, {}, {}}).continued, "indirect arguments written in the chain lost their RAW barrier");
    reset();
    dispatch({std::span(&arguments, 1), {}, {}, {}});
    Expect(!dispatch(writeA).continued, "a host-visible argument range lost its WAR barrier");
    reset();
    dispatch(writeA);
    Recorder::NoteComputeHostAccess();
    Expect(!dispatch(writeB).continued, "a host access continued a chain");
    reset();
    dispatch(writeA);
    AgcDriver::GuestMemory::SetCurrentPacket(0x15, 1);
    Expect(!dispatch(writeB).continued, "another queue continued a chain");
    reset();
    dispatch(writeA);
    recorder.Commands();
    Expect(!dispatch(writeB).continued, "another command continued a chain");
    reset();
    dispatch(writeA);
    Expect(!dispatch(writeB, false).deferred, "an unsupported dispatch deferred its trailing barrier");
    Expect(!dispatch(writeA).continued, "an unsupported dispatch preserved the previous chain");
    reset();
    dispatch(writeA);
    const PassAccess unknown{{}, {}, {}, {}, true, false};
    Expect(!dispatch(unknown).deferred, "unknown accesses were admitted to a tracked chain");
    reset();
    const std::pair<std::uint64_t, std::uint64_t> invalid{0x2000, 0x1000};
    Expect(!dispatch({std::span(&invalid, 1), {}, {}, {}}).deferred, "a malformed range was admitted to a chain");
    reset();
    dispatch(writeA);
    const std::uint32_t label = 7;
    recorder.RecordStore(reinterpret_cast<VkBuffer>(std::uintptr_t{0x1235}), 0, std::as_bytes(std::span(&label, 1)), 0x3000);
    Expect(!dispatch(writeB).continued, "a queued label continued a chain");
    recorder.EndComputeChain(true);
    Expect(mock.commands.find('U') < mock.commands.rfind('B'), "a PM4 sync barrier preceded its queued label store");
    reset();
    recorder.Commands();
    recorder.LeaveRenderPassOpen(1, Recorder::NoTiming, false, {});
    recorder.EndComputeChain(true);
    Expect(mock.commands.find('E') < mock.commands.rfind('B'), "a global sync barrier was recorded inside a render pass");
    reset();
    for (std::uint32_t opcode = 0; opcode != 256; ++opcode) Expect(Recorder::ComputeChainPacketBoundary(0xc0000000u | (opcode << 8u)) == (opcode != 0x15 && opcode != 0x16), "a non-dispatch PM4 packet did not close the chain");
    Expect(Recorder::ComputeChainPacketBoundary(0xc0001501u) && Recorder::ComputeChainPacketBoundary(0x00001500u), "a predicated or non-type-3 packet continued a chain");
    dispatch(writeA, true, "PPSA01341");
    const auto upper = dispatch(readA, true, "PPSA01341");
    Expect(upper.continued == ds && upper.unchecked == ds, "the title-gated experiment did not bypass its inter-dispatch hazard");
    recorder.EndComputeChain();
    if (upper.deferred) {
        Expect((mock.barrierSource & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) != 0 && (mock.barrierDestination & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) != 0 && (mock.barrierDestination & VK_PIPELINE_STAGE_HOST_BIT) != 0, "chain end did not emit a full barrier to GPU and host readers");
        Expect((mock.barrierWrites & VK_ACCESS_MEMORY_WRITE_BIT) != 0 && (mock.barrierReads & VK_ACCESS_HOST_READ_BIT) != 0, "chain end did not publish writes to host readers");
    }
    reset();
    for (std::uint32_t i = 0; i != 129; ++i) {
        const auto start = dispatch({});
        Expect(start.continued == (tracked && i != 0 && i != 128), "the bounded chain did not close after 128 dispatches");
    }
    reset();
    AgcDriver::GuestMemory::SetCurrentPacket(AgcDriver::GuestMemory::NoPacket, 0xffffffffu);
}

void OpenBatchUnderBudget() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    recorder.Keep(std::make_shared<int>(0));
    recorder.Keep(std::make_shared<int>(1), Budget - 1);
    recorder.BoundKeptBytes();
    Expect(recorder.Recording() && recorder.Submissions() == 0 && mock.submits == 0, "a batch keeping one byte less than the budget was submitted");
    recorder.Keep(std::make_shared<int>(2), 1);
    recorder.BoundKeptBytes();
    Expect(!recorder.Recording() && recorder.Submissions() == 1 && mock.submits == 1, "the open batch was not submitted once its kept bytes reached the budget");
    Expect(recorder.InFlightKeptBytes() == Budget, "the submitted batch counts " + std::to_string(recorder.InFlightKeptBytes()) + " kept bytes in flight, not the budget");
    Expect(mock.fenceWaits == 0, "the recorder waited for a batch with one budget in flight");
    recorder.Sync();
    Expect(recorder.InFlightKeptBytes() == 0, "a synced recorder still counts " + std::to_string(recorder.InFlightKeptBytes()) + " kept bytes in flight");
}

void InFlightWithinTwiceTheBudget() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    std::vector<std::weak_ptr<int>> batches;
    for (int batch = 0; batch < 6; ++batch) {
        auto object = std::make_shared<int>(batch);
        batches.push_back(object);
        recorder.Keep(std::move(object), Budget / 2);
        recorder.Keep(std::make_shared<int>(batch), Budget / 2);
        recorder.BoundKeptBytes();
        Expect(recorder.InFlightKeptBytes() <= 2 * Budget, "batch " + std::to_string(batch) + " left " + std::to_string(recorder.InFlightKeptBytes()) + " kept bytes in flight, more than twice the budget");
    }
    Expect(recorder.Submissions() == 6, "six budget batches made " + std::to_string(recorder.Submissions()) + " submissions");
    Expect(recorder.InFlightBatches() == 2 && recorder.InFlightKeptBytes() == 2 * Budget, "the newest two batches are not the ones in flight");
    Expect(mock.fenceWaits == 4, "the recorder waited for " + std::to_string(mock.fenceWaits) + " batches, not the four oldest");
    for (std::size_t batch = 0; batch < batches.size(); ++batch) {
        const bool released = batches[batch].expired();
        Expect(released == (batch < 4), "batch " + std::to_string(batch) + (released ? " released its kept objects while in flight" : " still holds its kept objects under the GPU mutex after the wait for it"));
    }
}

void SyncedWithinTwiceTheBudget() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    std::vector<std::weak_ptr<int>> batches;
    const auto alive = [&] { return std::count_if(batches.begin(), batches.end(), [](const auto& batch) { return !batch.expired(); }); };
    for (int batch = 0; batch < 6; ++batch) {
        auto object = std::make_shared<int>(batch);
        batches.push_back(object);
        recorder.Keep(std::move(object), Budget);
        recorder.BoundKeptBytes();
        recorder.Sync();
        Expect(alive() <= 2, "after batch " + std::to_string(batch) + ", " + std::to_string(alive()) + " synced budget batches still hold their kept objects under the GPU mutex");
    }
    Expect(recorder.Submissions() == 6 && recorder.InFlightKeptBytes() == 0, "six synced budget batches made " + std::to_string(recorder.Submissions()) + " submissions and left " + std::to_string(recorder.InFlightKeptBytes()) + " kept bytes in flight");
    Expect(!batches[4].expired() && !batches[5].expired(), "the newest two synced batches released their kept objects before the unlock");
}

void CountFollowsEveryPath() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    recorder.Keep(std::make_shared<int>(0), 3 * Budget);
    recorder.BoundKeptBytes();
    Expect(recorder.Submissions() == 1 && recorder.InFlightBatches() == 0 && recorder.InFlightKeptBytes() == 0, "a batch keeping three budgets was not submitted, waited for and released");
    recorder.Keep(std::make_shared<int>(1), Budget);
    recorder.Submit();
    Expect(recorder.InFlightKeptBytes() == Budget, "a plain Submit did not count the batch's kept bytes in flight");
    for (auto& [fence, signaled] : mock.signaled) signaled = true;
    Expect(recorder.Reap() && recorder.InFlightKeptBytes() == 0, "a reaped batch still counts " + std::to_string(recorder.InFlightKeptBytes()) + " kept bytes in flight");
}

void SharedOwnerCountsOncePerBatch() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    auto shared = std::make_shared<int>(0);
    for (int keep = 0; keep < 3; ++keep) {
        recorder.Keep(shared);
        recorder.KeepBytes(shared.get(), Budget / 2);
        recorder.BoundKeptBytes();
    }
    Expect(recorder.Recording() && recorder.OpenKeptBytes() == Budget / 2 && mock.submits == 0, "three keeps of one owner's half budget counted " + std::to_string(recorder.OpenKeptBytes()) + " kept bytes, not half the budget once");
    auto other = std::make_shared<int>(1);
    recorder.Keep(other);
    recorder.KeepBytes(other.get(), Budget / 2);
    recorder.BoundKeptBytes();
    Expect(!recorder.Recording() && recorder.Submissions() == 1 && recorder.InFlightKeptBytes() == Budget, "a second owner's half budget did not complete the batch's budget");
    recorder.Keep(shared);
    recorder.KeepBytes(shared.get(), Budget / 2);
    Expect(recorder.OpenKeptBytes() == Budget / 2, "the next batch did not count an owner the submitted batch counted");
    recorder.Sync();
}

void FailedSubmission(VkResult endResult, VkResult submitResult, VkResult idleResult = VK_ERROR_DEVICE_LOST) {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    alignas(8) std::array<std::byte, 128> memory{};
    const auto address = reinterpret_cast<std::uint64_t>(memory.data());
    bool completed = false;
    bool earlierCompleted = false;
    bool earlierWrittenBack = false;
    {
        Recorder recorder(mockContext());
        recorder.Activate();
        recorder.Keep(std::make_shared<int>(0), 17);
        recorder.NoteLabel(address, std::span<const std::byte>(memory).first(4), 1, 0);
        recorder.OnComplete([&] { earlierWrittenBack = true; });
        Expect(recorder.AfterRecordedWork([&] { earlierCompleted = true; }), "the earlier batch did not accept its completion");
        recorder.Submit();
        auto kept = std::shared_ptr<int>(new int(1), [](int* object) {
            Expect(!mock.lostSubmissionPending, "kept resources were destroyed before the lost submission finished");
            delete object;
        });
        std::weak_ptr<int> released = kept;
        recorder.Keep(std::move(kept), Budget);
        recorder.NoteLabel(address + 8, std::span<const std::byte>(memory).first(4), 2, 0);
        recorder.NoteLabel(address + 16, std::span<const std::byte>(memory).first(Recorder::LabelTableBytes + 4), 3, 0);
        recorder.AfterCompletions(address + 96, std::span<const std::byte>(memory).first(4), 4, 0, false);
        recorder.OnComplete([&] { completed = true; });
        Expect(recorder.AfterRecordedWork([&] { completed = true; }), "the failed batch did not accept its completion");
        mock.endResult = endResult;
        mock.submitResult = submitResult;
        mock.idleResult = idleResult;
        bool failed = false;
        try {
            recorder.Submit();
        } catch (const std::exception& error) {
            const auto operation = endResult != VK_SUCCESS ? "vkEndCommandBuffer recorder" : "vkQueueSubmit recorder";
            failed = std::string_view(error.what()).find(operation) != std::string_view::npos;
        }
        Expect(failed, "submission did not report its Vulkan failure");
        Expect(released.expired() && !completed, "a failed batch retained objects or ran its completions");
        Expect(!recorder.Recording() && recorder.Submissions() == 1 && recorder.InFlightBatches() == 1 && recorder.InFlightKeptBytes() == 17,
            "the failed batch changed the earlier in-flight submission");
        Expect(recorder.PendingLabels() == 1 && recorder.PendingLabelIn(address, 4), "a failed batch retained labels or removed the earlier batch's label");
        Expect(!recorder.PendingLabelIn(address + 8, 4) && !recorder.PendingLabelIn(address + 16, Recorder::LabelTableBytes + 4),
            "a failed batch retained a narrow or wide label");
        Expect(!recorder.CompletionLabelIn(address + 96, 4) && Recorder::PendingCompletionLabels() == 1 && Recorder::PendingWriteBackCompletions() == 1,
            "a failed batch retained pending completions or removed earlier ones");
        Expect(!Recorder::PendingLabelSince().has_value(), "a failed batch left its label flush deadline pending");
        Expect(Recorder::SnapshotWriteOverlaps(address, 4) && !Recorder::SnapshotWriteOverlaps(address + 8, 100),
            "a failed batch left stale writes in the pending snapshot or removed an earlier write");
        Expect(mock.fenceWaits == 0, "a failed submission waited for an unsignaled fence");
        Expect(mock.idleWaits == (submitResult == VK_ERROR_DEVICE_LOST ? 1u : 0u), "a failed submission did not settle device loss before releasing its resources");
        mock.lostSubmissionPending = false;
        mock.endResult = mock.submitResult = VK_SUCCESS;
        recorder.Sync();
        Expect(recorder.Idle() && !completed && earlierCompleted && earlierWrittenBack,
            "sync ran the failed batch's completions or lost the earlier batch's completions");
        Expect(Recorder::PendingCompletionLabels() == 0 && Recorder::PendingWriteBackCompletions() == 0,
            "sync left stale completion counts after submission failure");
    }
    Expect(mock.signaled.empty() && mock.allocatedCommands == mock.freedCommands, "a failed submission leaked its fence or command buffer");
}

void UnlockedWaitMutation(bool retire, bool submitNew) {
    mock = MockDevice{};
    mock.waitingThread = std::this_thread::get_id();
    AgcDriver::GuestMemory::TagGpuLockSite(AgcDriver::GuestMemory::GpuLockSite::Label);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext(), true);
    std::vector<int> completions;
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto epoch = AgcDriver::GuestMemory::ThreadCollectEpoch();
    recorder.Keep(std::make_shared<int>(1));
    recorder.NotePendingWrite(0x1000, 4);
    recorder.OnComplete([&] {
        if (!retire && epoch != 0) Expect(AgcDriver::GuestMemory::ThreadCollectEpoch() > epoch, "the waiter's completion reused collections from before the unlocked wait");
        completions.push_back(1);
    });
    const auto target = recorder.OpenSerial();
    const auto allocated = mock.allocatedCommands;
    mock.duringWait = [&] {
        std::thread producer([&] {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            Expect(mock.submits == 1, "the unlocked waiter did not submit its open batch first");
            if (retire) recorder.Reap();
            recorder.Keep(std::make_shared<int>(2));
            recorder.NotePendingWrite(0x1000, 4);
            recorder.OnComplete([&] { completions.push_back(2); });
            if (retire) Expect(mock.allocatedCommands == allocated, "the retired batch's command buffer was not reused during the wait");
            if (submitNew) {
                recorder.Submit();
                recorder.Keep(std::make_shared<int>(3));
                recorder.NotePendingWrite(0x2000, 4);
                recorder.OnComplete([&] { completions.push_back(3); });
                recorder.Submit();
            }
        });
        producer.join();
    };
    Expect(recorder.FinishUpToUnlocked(target), "an eligible wait did not unlock");
    Expect(AgcDriver::GuestMemory::GpuMutex().DepthOnThisThread() == 1, "the unlocked wait did not restore the outer hold");
    if (epoch != 0) Expect(AgcDriver::GuestMemory::ThreadCollectEpoch() > epoch, "the unlocked wait reused its pre-wait collect epoch");
    if (std::getenv("APS5_PROFILE_DRAW") != nullptr) Expect(AgcDriver::GuestMemory::GpuLockHoldSite() == AgcDriver::GuestMemory::GpuLockSite::Label, "the unlocked wait changed its profile site");
    Expect(mock.timelineWaits == 1 && mock.submits == (submitNew ? 3u : 1u), "the unlocked wait did not allow another producer or submitted new work");
    Expect(completions == std::vector<int>{1}, "the waiter reran an old completion or finished newly submitted work");
    Expect(recorder.InFlightBatches() == (submitNew ? 2u : 0u) && recorder.PendingWriterSerial(0x1000, 4) == 2, "the waiter lost the new writer of the same range");
    recorder.SyncThrough(0x1000, 4);
    Expect(completions == std::vector<int>({1, 2}), "revalidation did not finish the new overlapping writer before publication");
    Expect(recorder.PendingWriterSerial(0x1000, 4) == 0, "revalidation left the new overlapping writer pending");
    Expect(recorder.InFlightBatches() == (submitNew ? 1u : 0u), "revalidation finished unrelated later work");
    recorder.Sync();
    Expect(completions == (submitNew ? std::vector<int>{1, 2, 3} : std::vector<int>{1, 2}), "completion order changed across the unlocked wait");
}

void UnlockedWaitReplacement() {
    mock = MockDevice{};
    mock.waitingThread = std::this_thread::get_id();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto context = mockContext();
    std::optional<Recorder> recorder;
    recorder.emplace(context, true);
    recorder->Activate();
    recorder->Keep(std::make_shared<int>(1));
    const auto target = recorder->OpenSerial();
    std::thread replacement;
    std::atomic<bool> teardown{false};
    recorder->OnComplete([&] { teardown.store(true); });
    bool newCompletion = false;
    mock.duringWait = [&] {
        replacement = std::thread([&] {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            recorder.reset();
            recorder.emplace(context, true);
            recorder->Activate();
            recorder->Keep(std::make_shared<int>(2));
            recorder->OnComplete([&] { newCompletion = true; });
            recorder->Submit();
        });
        while (!teardown.load()) std::this_thread::yield();
    };
    auto* original = &*recorder;
    Expect(original->FinishUpToUnlocked(target), "the teardown wait did not unlock");
    replacement.join();
    Expect(AgcDriver::GuestMemory::GpuMutex().DepthOnThisThread() == 1, "replacement lost the waiter's mutex hold");
    Expect(Recorder::Active() == &*recorder && recorder->InFlightBatches() == 1 && !newCompletion, "the waiter touched the replacement recorder at the same address");
    recorder->Sync();
    Expect(newCompletion, "the replacement's completion was lost");
}

void UnlockedWaitFallbacks(bool enabled) {
    mock = MockDevice{};
    mock.waitingThread = std::this_thread::get_id();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    for (const bool timeline : {false, true}) {
        Recorder recorder(mockContext(), timeline);
        recorder.Keep(std::make_shared<int>(1));
        {
            std::lock_guard nested(AgcDriver::GuestMemory::GpuMutex());
            Expect(!recorder.FinishUpToUnlocked(recorder.OpenSerial()), "a nested hold was released");
            Expect(AgcDriver::GuestMemory::GpuMutex().DepthOnThisThread() == 2, "the nested hold's depth changed");
        }
        if (!enabled || !timeline) Expect(!recorder.FinishUpToUnlocked(recorder.OpenSerial()), "a disabled or timeline-free wait unlocked");
        Expect(recorder.Recording() && mock.timelineWaits == 0, "a fallback changed the recorder before returning");
        recorder.OnComplete([&] {
            Expect(!recorder.FinishUpToUnlocked(0), "a completion action was allowed to unlock");
            Expect(AgcDriver::GuestMemory::GpuMutex().DepthOnThisThread() == 1, "a completion action changed its hold depth");
        });
        recorder.Sync();
    }
}

void UnlockedWaitFailure(bool exception) {
    mock = MockDevice{};
    mock.waitingThread = std::this_thread::get_id();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext(), true);
    recorder.Keep(std::make_shared<int>(1));
    if (exception) mock.duringWait = [] { throw std::runtime_error("injected timeline failure"); };
    else mock.timelineResult = VK_ERROR_DEVICE_LOST;
    bool failed = false;
    try {
        recorder.FinishUpToUnlocked(recorder.OpenSerial());
    } catch (const std::runtime_error&) {
        failed = true;
    }
    Expect(failed && AgcDriver::GuestMemory::GpuMutex().DepthOnThisThread() == 1, "a failed wait did not restore the mutex before throwing");
    Expect(recorder.InFlightBatches() == 1, "a failed wait retired work before reporting the error");
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--compute-chains") {
            ComputeChainTests();
            return failures == 0 ? 0 : 1;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--unlocked-waits") {
            const bool enabled = std::getenv("APS5_UNLOCKED_LABEL_WAITS") != nullptr;
            UnlockedWaitFallbacks(enabled);
            if (enabled) {
                for (const bool retire : {false, true}) {
                    for (const bool submitNew : {false, true}) UnlockedWaitMutation(retire, submitNew);
                }
                UnlockedWaitReplacement();
                UnlockedWaitFailure(false);
                UnlockedWaitFailure(true);
            }
            return failures == 0 ? 0 : 1;
        }
        OpenBatchUnderBudget();
        InFlightWithinTwiceTheBudget();
        SyncedWithinTwiceTheBudget();
        CountFollowsEveryPath();
        SharedOwnerCountsOncePerBatch();
        FailedSubmission(VK_ERROR_OUT_OF_HOST_MEMORY, VK_SUCCESS);
        FailedSubmission(VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_SUCCESS);
        FailedSubmission(VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY);
        FailedSubmission(VK_SUCCESS, VK_ERROR_OUT_OF_DEVICE_MEMORY);
        FailedSubmission(VK_SUCCESS, VK_ERROR_DEVICE_LOST);
        FailedSubmission(VK_SUCCESS, VK_ERROR_DEVICE_LOST, VK_SUCCESS);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d recorder kept bytes checks failed\n", failures);
        return 1;
    }
    std::printf("recorder kept bytes tests passed\n");
    return 0;
}
