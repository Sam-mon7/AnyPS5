#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#if defined(ANYPS5_AFTERMATH_TESTING)
#include <filesystem>
#endif

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
    std::vector<VkCommandBuffer> ended, order;
    std::vector<VkFence> submitted;
    std::map<VkCommandBuffer, VkCommandPool> commandPools;
    std::map<VkCommandPool, std::uint32_t> pools;
    std::thread::id endThread, submitThread;
    bool endLocked = false, submitLocked = false, waitedBeforeSubmit = false;
    VkResult endResult = VK_SUCCESS, submitResult = VK_SUCCESS;
    std::uint64_t failAt = 1;
    std::vector<std::uint32_t> dispatches, pushes;
    std::vector<char> recording;
    std::vector<std::array<std::uint32_t, 4>> barriers;
    std::thread::id recordThread;
    bool recordLocked = false;
    std::vector<std::thread::id> recordThreads;
    std::vector<bool> recordLocks;
};

MockDevice mock;
std::mutex mockMutex;
std::promise<void>* recordEntered = nullptr;
std::shared_future<void> recordAllowed;
std::atomic<bool> recordBlocked{false};

std::uint64_t SubmitCount() {
    std::lock_guard lock(mockMutex);
    return mock.submits;
}

void DrainSubmits() {
    if (SubmitWorkerEnabled()) OrderedQueueCall([] { return VK_SUCCESS; });
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo* info, const VkAllocationCallbacks*, VkCommandPool* pool) {
    std::lock_guard lock(mockMutex);
    *pool = reinterpret_cast<VkCommandPool>(mock.next++);
    mock.pools[*pool] = info->queueFamilyIndex;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyCommandPool(VkDevice, VkCommandPool pool, const VkAllocationCallbacks*) {
    std::lock_guard lock(mockMutex);
    mock.pools.erase(pool);
}

#if defined(ANYPS5_AFTERMATH_TESTING)
bool aftermathFenceLoss = false;
bool aftermathFinished = false;
bool aftermathResetTooEarly = false;
unsigned aftermathResets = 0, aftermathQueries = 0, aftermathDisables = 0;
long long aftermathMilliseconds = 0;

GFSDK_Aftermath_Result GFSDK_AFTERMATH_CALL mockAftermathEnable(GFSDK_Aftermath_Version, uint32_t, uint32_t,
        PFN_GFSDK_Aftermath_GpuCrashDumpCb, PFN_GFSDK_Aftermath_ShaderDebugInfoCb,
        PFN_GFSDK_Aftermath_GpuCrashDumpDescriptionCb, PFN_GFSDK_Aftermath_ResolveMarkerCb, void*) {
    return GFSDK_Aftermath_Result_Success;
}
GFSDK_Aftermath_Result GFSDK_AFTERMATH_CALL mockAftermathDisable() { ++aftermathDisables; return GFSDK_Aftermath_Result_Success; }
GFSDK_Aftermath_Result GFSDK_AFTERMATH_CALL mockAftermathStatus(GFSDK_Aftermath_CrashDump_Status* status) {
    aftermathFinished = ++aftermathQueries == 2;
    *status = aftermathFinished ? GFSDK_Aftermath_CrashDump_Status_Finished : GFSDK_Aftermath_CrashDump_Status_CollectingData;
    return GFSDK_Aftermath_Result_Success;
}
bool mockAftermathReady() { return true; }
void mockAftermathNoop() {}
auto mockAftermathNow() -> std::chrono::steady_clock::time_point { return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(aftermathMilliseconds); }
void mockAftermathSleep() { aftermathMilliseconds += 50; }
#endif

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* commands) {
    std::lock_guard lock(mockMutex);
    *commands = reinterpret_cast<VkCommandBuffer>(mock.next++);
    mock.commandPools[*commands] = info->commandPool;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockFreeCommandBuffers(VkDevice, VkCommandPool, std::uint32_t count, const VkCommandBuffer* commands) {
    std::lock_guard lock(mockMutex);
    for (std::uint32_t i = 0; i < count; ++i) mock.commandPools.erase(commands[i]);
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence) {
    std::lock_guard lock(mockMutex);
    *fence = reinterpret_cast<VkFence>(mock.next++);
    mock.signaled[*fence] = false;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*) {
    std::lock_guard lock(mockMutex);
    mock.signaled.erase(fence);
}

VKAPI_ATTR VkResult VKAPI_CALL mockGetFenceStatus(VkDevice, VkFence fence) {
    std::lock_guard lock(mockMutex);
    return mock.signaled.at(fence) ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL mockWaitForFences(VkDevice, std::uint32_t count, const VkFence* fences, VkBool32, std::uint64_t) {
#if defined(ANYPS5_AFTERMATH_TESTING)
    if (aftermathFenceLoss) return VK_ERROR_DEVICE_LOST;
#endif
    std::lock_guard lock(mockMutex);
    for (std::uint32_t i = 0; i < count; ++i) {
        mock.waitedBeforeSubmit |= std::find(mock.submitted.begin(), mock.submitted.end(), fences[i]) == mock.submitted.end();
        mock.signaled.at(fences[i]) = true;
    }
    ++mock.fenceWaits;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockResetFences(VkDevice, std::uint32_t count, const VkFence* fences) {
#if defined(ANYPS5_AFTERMATH_TESTING)
    if (aftermathFenceLoss) {
        ++aftermathResets;
        aftermathResetTooEarly |= !aftermathFinished;
    }
#endif
    std::lock_guard lock(mockMutex);
    for (std::uint32_t i = 0; i < count; ++i) {
        mock.signaled.at(fences[i]) = false;
        std::erase(mock.submitted, fences[i]);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockEndCommandBuffer(VkCommandBuffer commands) {
    std::lock_guard lock(mockMutex);
    mock.ended.push_back(commands);
    mock.endThread = std::this_thread::get_id();
    mock.endLocked = AgcDriver::GuestMemory::GpuMutex().HeldByThisThread();
    return mock.ended.size() >= mock.failAt ? mock.endResult : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockQueueSubmit(VkQueue, std::uint32_t count, const VkSubmitInfo* info, VkFence fence) {
    std::lock_guard lock(mockMutex);
    ++mock.submits;
    mock.submitThread = std::this_thread::get_id();
    mock.submitLocked = AgcDriver::GuestMemory::GpuMutex().HeldByThisThread();
    for (std::uint32_t i = 0; i < count; ++i) {
        mock.order.insert(mock.order.end(), info[i].pCommandBuffers, info[i].pCommandBuffers + info[i].commandBufferCount);
    }
    const auto result = mock.submits >= mock.failAt ? mock.submitResult : VK_SUCCESS;
    if (result == VK_SUCCESS) mock.submitted.push_back(fence);
    return result;
}

VKAPI_ATTR void VKAPI_CALL mockCmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags source, VkPipelineStageFlags destination, VkDependencyFlags, std::uint32_t count, const VkMemoryBarrier* barriers, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {
    std::lock_guard lock(mockMutex);
    mock.recording.push_back('B');
    mock.barriers.push_back({source, destination, count != 0 ? barriers[0].srcAccessMask : 0u, count != 0 ? barriers[0].dstAccessMask : 0u});
}
VKAPI_ATTR void VKAPI_CALL mockCmdBeginQuery(VkCommandBuffer, VkQueryPool, std::uint32_t, VkQueryControlFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdEndQuery(VkCommandBuffer, VkQueryPool, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdResetQueryPool(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {
    std::lock_guard lock(mockMutex);
    mock.recording.push_back('P');
}
VKAPI_ATTR void VKAPI_CALL mockCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t bytes, const void* data) {
    std::lock_guard lock(mockMutex);
    std::uint32_t value = 0;
    if (bytes >= sizeof(value)) std::memcpy(&value, data, sizeof(value));
    mock.pushes.push_back(value);
    mock.recording.push_back('C');
}
VKAPI_ATTR void VKAPI_CALL mockCmdDispatch(VkCommandBuffer, std::uint32_t x, std::uint32_t, std::uint32_t) {
    if (recordEntered != nullptr && !recordBlocked.exchange(true)) {
        recordEntered->set_value();
        recordAllowed.wait();
    }
    std::lock_guard lock(mockMutex);
    mock.dispatches.push_back(x);
    mock.recording.push_back('D');
    mock.recordThread = std::this_thread::get_id();
    mock.recordLocked = AgcDriver::GuestMemory::GpuMutex().HeldByThisThread();
    mock.recordThreads.push_back(mock.recordThread);
    mock.recordLocks.push_back(mock.recordLocked);
}
VKAPI_ATTR void VKAPI_CALL mockCmdDispatchIndirect(VkCommandBuffer commands, VkBuffer, VkDeviceSize offset) {
    mockCmdDispatch(commands, static_cast<std::uint32_t>(offset), 1, 1);
}

VKAPI_ATTR void VKAPI_CALL mockCmdEndRenderPass(VkCommandBuffer) {
    std::lock_guard lock(mockMutex);
    mock.recording.push_back('E');
    mock.recordThreads.push_back(std::this_thread::get_id());
    mock.recordLocks.push_back(AgcDriver::GuestMemory::GpuMutex().HeldByThisThread());
}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkCreateCommandPool", reinterpret_cast<PFN_vkVoidFunction>(mockCreateCommandPool)},
        {"vkDestroyCommandPool", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyCommandPool)},
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
        {"vkCmdUpdateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCmdUpdateBuffer)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPipelineBarrier)},
        {"vkCmdBeginQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBeginQuery)},
        {"vkCmdEndQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdEndQuery)},
        {"vkCmdResetQueryPool", reinterpret_cast<PFN_vkVoidFunction>(mockCmdResetQueryPool)},
        {"vkCmdCopyQueryPoolResults", reinterpret_cast<PFN_vkVoidFunction>(mockCmdCopyQueryPoolResults)},
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBindPipeline)},
        {"vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPushConstants)},
        {"vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(mockCmdDispatch)},
        {"vkCmdDispatchIndirect", reinterpret_cast<PFN_vkVoidFunction>(mockCmdDispatchIndirect)},
        {"vkCmdEndRenderPass", reinterpret_cast<PFN_vkVoidFunction>(mockCmdEndRenderPass)},
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

Context mockContext() {
    Context context{};
    context.device = reinterpret_cast<VkDevice>(mock.next++);
    context.queue = reinterpret_cast<VkQueue>(mock.next++);
    context.deviceProc = mockProc;
    context.queueFamilyIndex = 7;
    return context;
}

#if defined(ANYPS5_AFTERMATH_TESTING)
void aftermathRecorderReleaseTest() {
    namespace Capture = AgcDriver::Graphics::Aftermath;
    const auto output = std::filesystem::absolute(std::filesystem::temp_directory_path()) /
        ("anyps5-aftermath-recorder-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(!std::filesystem::exists(output), "Aftermath recorder test output already exists");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{output};
#ifdef _WIN32
    _putenv_s("APS5_AFTERMATH", "1");
    _putenv_s("APS5_AFTERMATH_DIR", output.string().c_str());
    _putenv_s("APS5_PROFILE_DRAW", "");
    _putenv_s("APS5_PROFILE_GPU", "");
    _putenv_s("APS5_TRACE_DIR", "");
#else
    setenv("APS5_AFTERMATH", "1", 1);
    setenv("APS5_AFTERMATH_DIR", output.string().c_str(), 1);
    unsetenv("APS5_PROFILE_DRAW");
    unsetenv("APS5_PROFILE_GPU");
    unsetenv("APS5_TRACE_DIR");
#endif
    Capture::Testing::Hooks hooks;
    hooks.enable = mockAftermathEnable;
    hooks.disable = mockAftermathDisable;
    hooks.status = mockAftermathStatus;
    hooks.load = mockAftermathReady;
    hooks.pin = mockAftermathReady;
    hooks.unload = mockAftermathNoop;
    hooks.now = mockAftermathNow;
    hooks.sleep = mockAftermathSleep;
    hooks.beforeWrite = mockAftermathNoop;
    hooks.destroyed = mockAftermathNoop;
    Capture::Testing::Install(hooks);
    mock = MockDevice{};
    bool preservedError = false;
    {
        Capture::DeviceLease lease;
        Require(static_cast<bool>(lease), "Aftermath recorder mock lease is disabled");
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::Graphics::Recorder recorder(mockContext(), false);
        recorder.Commands();
        recorder.Submit();
        aftermathFenceLoss = true;
        try { recorder.Sync(); }
        catch (const std::runtime_error& error) { preservedError = std::string_view(error.what()).find("Vulkan result -4") != std::string_view::npos; }
        aftermathFenceLoss = false;
    }
    Require(preservedError, "Aftermath changed the recorder's original device-loss exception");
    Require(aftermathQueries == 2 && aftermathMilliseconds == 50, "Recorder did not finish capture before propagating loss");
    Require(aftermathResets == 1 && !aftermathResetTooEarly, "Recorder released its failed batch before Aftermath capture finished");
    Require(aftermathDisables == 1 && mock.signaled.empty(), "Aftermath recorder fixture leaked session or Vulkan mock objects");
}
#endif

void OpenBatchUnderBudget() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    recorder.Keep(std::make_shared<int>(0));
    recorder.Keep(std::make_shared<int>(1), Budget - 1);
    recorder.BoundKeptBytes();
    Expect(recorder.Recording() && recorder.Submissions() == 0 && SubmitCount() == 0, "a batch keeping one byte less than the budget was submitted");
    recorder.Keep(std::make_shared<int>(2), 1);
    recorder.BoundKeptBytes();
    DrainSubmits();
    Expect(!recorder.Recording() && recorder.Submissions() == 1 && SubmitCount() == 1, "the open batch was not submitted once its kept bytes reached the budget");
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
    DrainSubmits();
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
    Expect(recorder.Recording() && recorder.OpenKeptBytes() == Budget / 2 && SubmitCount() == 0, "three keeps of one owner's half budget counted " + std::to_string(recorder.OpenKeptBytes()) + " kept bytes, not half the budget once");
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

void BackgroundRelease(bool teardown) {
    mock = MockDevice{};
    std::promise<void> entered, allow, completed;
    auto enteredFuture = entered.get_future();
    auto allowFuture = allow.get_future().share();
    auto completedFuture = completed.get_future();
    std::thread::id caller, destroyer;
    std::atomic<bool> writeBack{false}, destroyed{false};
    bool ordered = false;
    std::exception_ptr failure;
    auto object = std::shared_ptr<int>(new int(0), [&](int* value) {
        destroyer = std::this_thread::get_id();
        ordered = writeBack.load(std::memory_order_acquire);
        entered.set_value();
        allowFuture.wait();
        delete value;
        destroyed.store(true, std::memory_order_release);
    });
    std::thread worker([&, object = std::move(object)]() mutable {
        try {
            caller = std::this_thread::get_id();
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            auto recorder = std::make_unique<Recorder>(mockContext());
            recorder->Keep(object, Budget);
            recorder->OnComplete([&, capture = std::move(object)] {
                *capture = 42;
                writeBack.store(true, std::memory_order_release);
            });
            recorder->Sync();
            if (teardown) {
                recorder.reset();
            } else {
                recorder->Keep(std::make_shared<int>(1), Budget);
                recorder->BoundKeptBytes();
                recorder->Keep(std::make_shared<int>(2), Budget);
                recorder->BoundKeptBytes();
            }
            completed.set_value();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    const bool started = enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    Expect(started, "completed kept objects never reached a destructor");
    if (started) {
        Expect(destroyer != caller, "kept object destruction ran on the recorder caller");
        Expect(ordered, "kept object destruction preceded its guest write-back");
    }
    Expect(completedFuture.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
        teardown ? "recorder teardown returned before destruction finished" : "the byte bound ignored objects being destroyed on the release thread");
    allow.set_value();
    worker.join();
    if (failure) std::rethrow_exception(failure);
    Expect(destroyed.load(std::memory_order_acquire), "kept object outlived the recorder");
    Expect(mock.signaled.empty(), "recorder leaked mock Vulkan fences");
}

void BackgroundForcedRelease() {
    mock = MockDevice{};
    const auto caller = std::this_thread::get_id();
    std::thread::id destroyer;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(mockContext());
        recorder.Keep(std::shared_ptr<int>(new int(0), [&](int* value) {
            destroyer = std::this_thread::get_id();
            delete value;
        }), 3 * Budget);
        recorder.BoundKeptBytes();
        Expect(recorder.InFlightKeptBytes() == 0, "oversized completed batch remains in flight");
    }
    Expect(destroyer != std::thread::id{} && destroyer != caller, "forced budget retirement destroyed objects on the recorder caller");
}

void SubmitWorkerOrderAndSync() {
    mock = MockDevice{};
    const bool enabled = SubmitWorkerEnabled();
    std::promise<void> blocked, allowSubmit, handed, synced, allowTeardown;
    auto blockedFuture = blocked.get_future();
    auto allowSubmitFuture = allowSubmit.get_future().share();
    auto handedFuture = handed.get_future();
    auto syncedFuture = synced.get_future();
    auto teardownFuture = allowTeardown.get_future();
    if (enabled) {
        EnqueueSubmitWork([&] { blocked.set_value(); allowSubmitFuture.wait(); });
        blockedFuture.wait();
    }
    std::vector<VkCommandBuffer> commands;
    std::vector<unsigned> completions;
    std::weak_ptr<int> kept, held;
    std::atomic<unsigned> label{0};
    bool aliveAtCompletion = false;
    std::thread::id producer;
    std::exception_ptr failure;
    std::thread caller([&] {
        try {
            producer = std::this_thread::get_id();
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            Recorder recorder(mockContext());
            for (unsigned i = 0; i < 4; ++i) {
                commands.push_back(recorder.Commands());
                if (i == 0) {
                    auto object = std::make_shared<int>(1);
                    kept = object;
                    recorder.Keep(std::move(object));
                    object = std::make_shared<int>(2);
                    held = object;
                    recorder.KeepThroughHold(std::move(object));
                }
                recorder.OnComplete([&, i] {
                    if (i == 0) aliveAtCompletion = !kept.expired() && !held.expired();
                    completions.push_back(i);
                    label.store(i + 1);
                });
                recorder.Submit();
            }
            if (enabled) Require(!recorder.Reap(), "an unsubmitted batch was reaped");
            handed.set_value();
            recorder.Sync();
            synced.set_value();
            teardownFuture.wait();
        } catch (...) {
            failure = std::current_exception();
            try { handed.set_value(); } catch (...) {}
            try { synced.set_value(); } catch (...) {}
        }
    });
    handedFuture.wait();
    if (enabled) {
        Expect(SubmitCount() == 0 && label.load() == 0, "work or completions escaped the blocked submit worker");
        Expect(!kept.expired() && !held.expired(), "an unsubmitted batch lost kept objects");
        Expect(syncedFuture.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout, "Sync returned for an unsubmitted batch");
        {
            std::lock_guard lock(mockMutex);
            Expect(mock.ended.empty() && mock.fenceWaits == 0, "producer ended or waited for an unsubmitted command buffer");
            Expect(mock.pools.size() == 4, "pending command buffers share a command pool");
            for (const auto& [pool, family] : mock.pools) Expect(family == 7, "worker pool uses the wrong queue family");
        }
    }
    std::thread direct([&] {
        OrderedQueueCall([&] {
            std::lock_guard lock(mockMutex);
            mock.order.push_back(VK_NULL_HANDLE);
            return VK_SUCCESS;
        });
    });
    allowSubmit.set_value();
    const bool completed = syncedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    Expect(completed, "Sync did not complete after submitting its batches");
    if (completed && !failure) {
        Expect(aliveAtCompletion && label.load() == 4, "completion actions lost objects or ran out of order");
        Expect(!held.expired(), "KeepThroughHold ended at batch completion");
    }
    allowTeardown.set_value();
    caller.join();
    direct.join();
    if (failure) std::rethrow_exception(failure);
    commands.push_back(VK_NULL_HANDLE);
    Expect(mock.order == commands, "queue calls overtook handed-off recorder batches");
    Expect(mock.ended == std::vector<VkCommandBuffer>(commands.begin(), commands.end() - 1), "command buffers ended out of order");
    Expect(completions == std::vector<unsigned>({0, 1, 2, 3}), "batch completions ran out of order");
    Expect(!mock.waitedBeforeSubmit, "a fence wait preceded vkQueueSubmit");
    Expect((mock.endThread == producer) == !enabled && (mock.submitThread == producer) == !enabled, "submit switch selected the wrong thread");
    Expect(mock.endLocked == !enabled && mock.submitLocked == !enabled, "submit worker used the GPU mutex");
    Expect(kept.expired() && held.expired(), "kept objects outlived recorder teardown");
    Expect(mock.signaled.empty() && mock.pools.empty() && mock.commandPools.empty(), "recorder leaked mock Vulkan objects");
}

void SubmitWorkerDirectBatch() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto context = mockContext();
    Recorder recorder(context);
    recorder.Activate();
    const auto first = recorder.Commands();
    CommandBatch batch(context);
    const auto second = batch.Handle();
    batch.SubmitAndWait();
    Expect(mock.order == std::vector<VkCommandBuffer>({first, second}), "SubmitAndWait overtook recorded work");
    Expect(!mock.waitedBeforeSubmit, "SubmitAndWait waited on an unsubmitted fence");
    recorder.Sync();
}

void SubmitWorkerFailure(bool endFailure, std::uint64_t failAt = 1, bool deviceLost = false) {
    if (!SubmitWorkerEnabled()) return;
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    mock.failAt = failAt;
    if (endFailure) mock.endResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    else mock.submitResult = deviceLost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    std::promise<void> allow;
    auto allowed = allow.get_future().share();
    EnqueueSubmitWork([&] { allowed.wait(); });
    bool observed = false;
    {
        Recorder recorder(mockContext());
        for (int i = 0; i < 3; ++i) {
            recorder.Keep(std::make_shared<int>(i));
            recorder.Submit();
        }
        allow.set_value();
        try { recorder.Sync(); }
        catch (const std::runtime_error& error) {
            const auto expected = endFailure ? "vkEndCommandBuffer recorder: Vulkan result -2" : deviceLost ? "vkQueueSubmit recorder: Vulkan result -4" : "vkQueueSubmit recorder: Vulkan result -2";
            observed = std::string_view(error.what()).find(expected) != std::string_view::npos;
        }
    }
    Expect(observed, "submit failure did not reach the waiting producer unchanged");
    Expect(mock.fenceWaits == failAt - 1 && mock.submits == (endFailure ? failAt - 1 : failAt), "failed worker submitted later batches or waited on an unsignaled fence");
    Expect(mock.signaled.empty() && mock.pools.empty() && mock.commandPools.empty(), "failed submit teardown leaked mock Vulkan objects");
}

void ComputeChainTests() {
    using Reason = Recorder::ComputeChainReason;
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto context = mockContext();
    Recorder recorder(context);
    recorder.Activate();
    const bool enabled = Recorder::ComputeChainsEnabled();
    const auto* value = std::getenv("APS5_COMPUTE_CHAINS");
    Expect(enabled == (value != nullptr && std::string_view(value) == "1"), "compute chains must require the exact switch value 1");
    const auto* lanes = std::getenv("APS5_GPU_LANES");
    const bool admitted = enabled && Recorder::MergeBarriers() && !Recorder::RecordWorkerEnabled() && (lanes == nullptr || std::string_view(lanes) != "1");
    const auto reasonCount = [](const auto& counts, Reason reason) { return counts.fallbacks[static_cast<std::size_t>(reason)]; };
    const std::pair<std::uint64_t, std::uint64_t> a{0x1000, 0x1100}, b{0x2000, 0x2100};
    const PassAccess writeA{{}, std::span(&a, 1), {}, {}}, writeB{{}, std::span(&b, 1), {}, {}}, readA{std::span(&a, 1), {}, {}, {}};
    constexpr auto shaderAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    constexpr auto trailingAccess = shaderAccess | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    const auto dispatch = [&](const PassAccess& access, Reason fallback = Reason::Count) {
        const auto start = recorder.BeginComputeDispatch(access, fallback);
        if (!start.continued && (!Recorder::MergeBarriers() || (start.covered & shaderAccess) != shaderAccess)) {
            RecordMemoryBarrier(context, start.commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, shaderAccess);
        }
        mockCmdDispatch(start.commands, 1, 1, 1);
        if (start.deferred) recorder.DeferComputeBarrier(access);
        else {
            RecordMemoryBarrier(context, start.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, trailingAccess);
            recorder.MarkCovered(trailingAccess);
        }
        return start;
    };
    for (bool sync : {false, true}) {
        AgcDriver::GuestMemory::SetCurrentPacket(0x15, 0);
        const auto before = Recorder::ComputeChainCounts();
        const auto first = mock.recording.size();
        dispatch(writeA);
        Expect(dispatch(writeB).continued == admitted, "disjoint dispatch did not follow the chain policy");
        const auto beforeBoundary = mock.barriers.size();
        if (sync) recorder.EndComputeChain(Reason::Pm4);
        Expect(!dispatch(readA).continued, "RAW dispatch crossed a writer or a PM4 sync point");
        if (admitted) {
            constexpr auto stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT;
            const auto& barrier = mock.barriers.at(beforeBoundary);
            Expect(mock.barriers.size() == beforeBoundary + 1 && barrier[0] == stages && barrier[1] == stages &&
                (barrier[2] & VK_ACCESS_MEMORY_WRITE_BIT) != 0 && (barrier[3] & (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)) == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT), "sync or hazard did not record one global memory barrier");
        }
        recorder.Sync();
        const auto counts = Recorder::ComputeChainCounts();
        const auto barriers = std::count(mock.recording.begin() + first, mock.recording.end(), 'B');
        const auto expected = admitted ? 3 : Recorder::MergeBarriers() ? 4 : 6;
        Expect(barriers == expected, "dispatch barrier count changed from the enabled/disabled baseline");
        Expect(counts.chained - before.chained == (admitted ? 1u : 0u) && counts.saved - before.saved == (admitted ? 1u : 0u), "chain counters disagree with barriers saved");
        Expect(reasonCount(counts, Reason::Raw) - reasonCount(before, Reason::Raw) == (admitted && !sync ? 1u : 0u), "RAW did not force a barrier at the correct boundary");
    }
    for (const auto reason : {Reason::Waw, Reason::War}) {
        const auto before = Recorder::ComputeChainCounts();
        dispatch(reason == Reason::War ? readA : writeA);
        dispatch(writeB);
        Expect(!dispatch(writeA).continued, "buffer write hazard continued a chain");
        recorder.Sync();
        Expect(reasonCount(Recorder::ComputeChainCounts(), reason) - reasonCount(before, reason) == (admitted ? 1u : 0u), "write hazard lost its fallback reason");
    }
    for (const auto reason : {Reason::Indirect, Reason::Unknown, Reason::Refresh, Reason::Copied, Reason::Mirror}) {
        dispatch(writeA);
        Expect(!dispatch(writeB, reason).deferred, "an unsupported dispatch deferred its trailing barrier");
        recorder.Sync();
    }
    const std::pair<VkImage, bool> image{reinterpret_cast<VkImage>(1), true};
    const PassAccess imageAccess{{}, {}, std::span(&image, 1), {}};
    dispatch(writeA);
    Expect(!dispatch(imageAccess).deferred, "storage image alias uncertainty admitted a chain");
    recorder.Sync();
    dispatch(writeA);
    AgcDriver::GuestMemory::SetCurrentPacket(0x15, 1);
    const auto beforeQueue = Recorder::ComputeChainCounts();
    Expect(!dispatch(writeB).continued, "a chain crossed recording queues");
    recorder.Sync();
    Expect(reasonCount(Recorder::ComputeChainCounts(), Reason::Queue) - reasonCount(beforeQueue, Reason::Queue) == (admitted ? 1u : 0u), "queue switch did not close the chain");
    AgcDriver::GuestMemory::SetCurrentPacket(0x15, 0);
    const auto beforeLimit = Recorder::ComputeChainCounts();
    for (int i = 0; i < 129; ++i) dispatch(readA);
    recorder.Sync();
    Expect(reasonCount(Recorder::ComputeChainCounts(), Reason::Limit) - reasonCount(beforeLimit, Reason::Limit) == (admitted ? 1u : 0u), "chain tracking did not stop at its size bound");
    for (const auto boundary : {Reason::Command, Reason::Label, Reason::Batch}) {
        dispatch(writeA);
        const auto first = mock.recording.size();
        if (boundary == Reason::Command) recorder.Commands();
        else if (boundary == Reason::Batch) recorder.Submit();
        else {
            const std::uint32_t label = 1;
            recorder.RecordStore(reinterpret_cast<VkBuffer>(1), 0, std::as_bytes(std::span(&label, 1)), 0x3000);
        }
        Expect(!admitted || mock.recording.size() > first, "recorder boundary failed to emit the deferred global barrier");
        recorder.Sync();
    }
    dispatch(writeA);
    const auto beforeEop = mock.recording.size();
    bool completed = false;
    Expect(recorder.AfterRecordedWork([&] { completed = true; }), "EOP completion did not attach to the open batch");
    Expect(!admitted || mock.recording.size() == beforeEop + 1, "EOP completion without a GPU store did not close the chain");
    Expect(!completed, "EOP callback ran before submission");
    recorder.Sync();
    Expect(completed, "EOP callback was lost after the chain boundary");
    for (const auto opcode : {0x37u, 0x3cu, 0x42u, 0x46u, 0x47u, 0x48u, 0x49u, 0x58u, 0x93u}) {
        Expect(Recorder::ComputeChainPacketBoundary(0xc0000000u | (opcode << 8u)), "PM4 synchronization packet could continue a chain");
    }
    for (const auto opcode : {0x15u, 0x16u, 0x69u, 0x76u, 0x79u, 0x7au}) {
        Expect(!Recorder::ComputeChainPacketBoundary(0xc0000000u | (opcode << 8u)), "dispatch/register packet unexpectedly closes a chain");
    }
    Expect(Recorder::ComputeChainPacketBoundary(0xc0001501u), "predicated dispatch could read CPU memory inside a chain");
    if (!enabled) {
        const auto counts = Recorder::ComputeChainCounts();
        Expect(counts.deferred == 0 && counts.barriers == 0 && counts.chained == 0 && counts.saved == 0, "disabled compute chains changed counters");
    }
}

void RecordWorkerOrderAndSync() {
    mock = MockDevice{};
    const bool enabled = Recorder::RecordWorkerEnabled();
    std::promise<void> entered, allow, appended, finished;
    auto enteredFuture = entered.get_future();
    auto appendedFuture = appended.get_future();
    auto finishedFuture = finished.get_future();
    recordBlocked = false;
    recordEntered = enabled ? &entered : nullptr;
    recordAllowed = allow.get_future().share();
    std::weak_ptr<int> kept;
    std::thread::id producer;
    bool aliveAtCompletion = false;
    std::exception_ptr failure;
    const auto before = Recorder::RecordCounts();
    std::thread caller([&] {
        try {
            producer = std::this_thread::get_id();
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            Recorder recorder(mockContext());
            auto object = std::make_shared<int>(1);
            kept = object;
            recorder.Keep(std::move(object));
            for (std::uint32_t i = 0; i < 260; ++i) {
                Recorder::DispatchCommand command;
                command.pipeline = reinterpret_cast<VkPipeline>(1);
                command.layout = reinterpret_cast<VkPipelineLayout>(2);
                command.x = i;
                command.pushBytes = sizeof(i);
                std::memcpy(command.push.data(), &i, sizeof(i));
                if (i % 2 != 0) {
                    command.indirect = reinterpret_cast<VkBuffer>(3);
                    command.offset = i;
                }
                recorder.RecordDispatch(command);
                command.push.fill(std::byte{0xff});
            }
            recorder.OnComplete([&] { aliveAtCompletion = !kept.expired(); });
            appended.set_value();
            if (enabled) {
                const auto commands = recorder.Commands();
                mockCmdDispatch(commands, 260, 1, 1);
            } else {
                mockCmdDispatch(recorder.Commands(), 260, 1, 1);
            }
            recorder.Sync();
        } catch (...) {
            failure = std::current_exception();
            try { appended.set_value(); } catch (...) {}
        }
        finished.set_value();
    });
    const bool queued = appendedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    Expect(queued, "producer waited for dispatch replay before a recording sync");
    if (enabled && queued) {
        Expect(enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "128 commands did not publish a chunk");
        Expect(finishedFuture.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout, "direct recording overtook the blocked worker");
        Expect(!kept.expired(), "unreplayed dispatches lost kept objects");
        Expect(Recorder::RecordCounts().queueDepth >= 2, "worker queue depth did not include the blocked chunks");
        std::lock_guard lock(mockMutex);
        Expect(mock.submits == 0 && mock.ended.empty(), "unfinished recording reached the submitter");
    }
    allow.set_value();
    caller.join();
    recordEntered = nullptr;
    recordAllowed = {};
    if (failure) std::rethrow_exception(failure);
    Expect(aliveAtCompletion && kept.expired(), "dispatch resource lifetime missed replay or batch completion");
    Expect(mock.dispatches.size() == 261 && mock.pushes.size() == 260, "dispatch records were lost or replayed twice");
    for (std::uint32_t i = 0; i < mock.dispatches.size(); ++i) Expect(mock.dispatches[i] == i, "dispatch replay changed batch order");
    for (std::uint32_t i = 0; i < mock.pushes.size(); ++i) Expect(mock.pushes[i] == i, "push constants were not copied into the chunk");
    for (std::size_t i = 0; i < 260 && i < mock.recordThreads.size(); ++i) Expect((mock.recordThreads[i] == producer) == !enabled && mock.recordLocks[i] == !enabled, "dispatch recording used the wrong thread or the GPU mutex");
    std::size_t event = 0;
    for (unsigned i = 0; i < 260; ++i) {
        const std::string expected = i % 2 != 0 ? "BBPCDB" : "BPCDB";
        Expect(event + expected.size() <= mock.recording.size() && std::equal(expected.begin(), expected.end(), mock.recording.begin() + event), "dispatch barriers or binds were reordered");
        event += expected.size();
    }
    const auto counts = Recorder::RecordCounts();
    Expect(!enabled || (counts.commands - before.commands == 260 && counts.chunks - before.chunks == 3 && counts.queueDepth == 0), "record worker counters missed chunks or queued work");
    Expect(!mock.waitedBeforeSubmit && mock.submits == 1, "recording sync changed GPU submission order");
    Expect(mock.signaled.empty() && mock.pools.empty() && mock.commandPools.empty(), "record worker teardown leaked mock Vulkan objects");
}

void DrawWorkerPassOrder() {
    mock = MockDevice{};
    const bool enabled = Recorder::RecordWorkerEnabled();
    std::promise<void> entered, allow, appended;
    auto enteredFuture = entered.get_future();
    auto appendedFuture = appended.get_future();
    recordBlocked = false;
    recordEntered = enabled ? &entered : nullptr;
    recordAllowed = allow.get_future().share();
    std::weak_ptr<int> kept;
    bool aliveAtCompletion = false;
    std::thread::id producer;
    std::exception_ptr failure;
    std::thread caller([&] {
        try {
            producer = std::this_thread::get_id();
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            Recorder recorder(mockContext());
            auto object = std::make_shared<int>(42);
            kept = object;
            recorder.Keep(object);
            std::vector<std::pair<std::uint64_t, std::uint64_t>> writes{{0x1000, 0x1010}};
            std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
            for (unsigned i = 0; i < 260; ++i) {
                const PassAccess access{reads, i == 0 ? GuestRangeList{writes} : GuestRangeList{}, {}, {}};
                const auto start = recorder.StartDrawPass(1, false, false, access, enabled);
                Require(start.continued == (i != 0), "queued draws did not continue their pass");
                recorder.RecordCommand([i, object](VkCommandBuffer commands) {
                    Require(*object == 42, "queued draw lost its immutable object");
                    mockCmdDispatch(commands, i, 1, 1);
                });
                recorder.LeaveRenderPassOpen(1, Recorder::NoTiming, false, access);
            }
            writes.clear();
            reads = {{0x1000, 0x1010}};
            const PassAccess dependent{reads, {}, {}, {}};
            const auto start = recorder.StartDrawPass(1, false, false, dependent, enabled);
            Require(!start.continued && start.barrier, "queued RAW hazard was lost before replay");
            recorder.RecordCommand([](VkCommandBuffer commands) { mockCmdDispatch(commands, 260, 1, 1); });
            recorder.LeaveRenderPassOpen(1, Recorder::NoTiming, false, dependent);
            object.reset();
            recorder.OnComplete([&] { aliveAtCompletion = !kept.expired(); });
            appended.set_value();
            mockCmdDispatch(recorder.Commands(), 261, 1, 1);
            recorder.Sync();
        } catch (...) {
            failure = std::current_exception();
            try { appended.set_value(); } catch (...) {}
        }
    });
    const bool queued = appendedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    Expect(queued, "producer drained the worker during pass continuation or a hazard split");
    if (enabled && queued) {
        Expect(enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "draw chunk was not published");
        Expect(!kept.expired(), "unreplayed draws lost their retained objects");
        std::lock_guard lock(mockMutex);
        Expect(mock.recording.empty() && mock.submits == 0, "pass end or direct commands overtook blocked draw replay");
    }
    allow.set_value();
    caller.join();
    recordEntered = nullptr;
    recordAllowed = {};
    if (failure) std::rethrow_exception(failure);
    Expect(aliveAtCompletion && kept.expired(), "draw objects did not live through completion or escaped the chunk pool");
    Expect(mock.dispatches.size() == 262, "draw records were lost or duplicated");
    for (std::size_t i = 0; i < mock.dispatches.size(); ++i) Expect(mock.dispatches[i] == i, "draw records changed order");
    const std::string expected = std::string(260, 'D') + "ED" + "EBD" + "B";
    Expect(std::string(mock.recording.begin(), mock.recording.end()) == expected, "draw pass ends or barriers changed order");
    for (std::size_t i = 0; i < 263 && i < mock.recordThreads.size(); ++i) Expect((mock.recordThreads[i] == producer) == !enabled && mock.recordLocks[i] == !enabled, "draw replay or pass end used the wrong thread");
    Expect(mock.submits == 1 && !mock.waitedBeforeSubmit, "draw recording changed submission order");
    Expect(mock.signaled.empty() && mock.pools.empty(), "draw worker teardown leaked Vulkan objects");
}

void ParseTimeLabelTests() {
    using namespace AgcDriver;
    mock = MockDevice{};
    std::lock_guard gpu(GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    recorder.Activate();
    alignas(64) std::array<std::uint32_t, 16> memory{};
    const auto address = reinterpret_cast<std::uint64_t>(memory.data());
    const bool enabled = Recorder::ParseTimeLabels();
    const auto labelsBefore = Recorder::ParseLabelCounts();
    std::array<std::uint32_t, 5> write{0xc0033700u, 0x500u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 7};
    const std::array<std::uint32_t, 7> wait{0xc0053c00u, 0x13u, write[2], write[3], 7, UINT32_MAX, 0};
    Expect(DriverDetail::TryParseTimeLabelPacket(write) == enabled, "WRITE_DATA parse-time switch selected the wrong path");
    Expect(Pm4::WaitSatisfiedUnchecked(wait) == enabled && SubmitCount() == 0 && mock.fenceWaits == 0, "constant label wait required a GPU round trip");
    std::array<std::uint32_t, 7> eop{0xc0054900u, 0, 1u << 29u, write[2], write[3], 9, 0};
    Expect(DriverDetail::TryParseTimeLabelPacket(eop) == enabled && memory[0] == (enabled ? 9u : 0u), "constant end-of-pipe label did not publish at parse time");
    eop[2] = 3u << 29u;
    Expect(!DriverDetail::TryParseTimeLabelPacket(eop), "a GPU timestamp label published early");
    eop[2] = (1u << 29u) | (1u << 24u);
    Expect(!DriverDetail::TryParseTimeLabelPacket(eop), "an interrupt label published early");
    memory[0] = 0;
    recorder.NotePendingRead(address, 4, Recorder::ReadKind::DispatchElement);
    Expect(!DriverDetail::TryParseTimeLabelPacket(write) && memory[0] == 0, "an early label overwrote an in-place GPU reader");
    recorder.Sync();
    recorder.NotePendingRead(address + 16, 4, Recorder::ReadKind::DispatchElement);
    const auto submitsBefore = SubmitCount();
    Expect(!DriverDetail::TryParseTimeLabelPacket(write), "an early label permitted reuse of a pending reader's guest input");
    Expect(SubmitCount() == submitsBefore, "a disjoint GPU reader forced a label submit");
    recorder.Sync();
    recorder.Commands();
    Expect(DriverDetail::TryParseTimeLabelPacket(write) == enabled, "a batch without pending guest accesses refused a constant label");
    recorder.Sync();
    memory[0] = 0;
    recorder.NotePendingWrite(address + 16, 4);
    Expect(!DriverDetail::TryParseTimeLabelPacket(write) && memory[0] == 0, "an early label bypassed a pending GPU write");
    recorder.Sync();
    recorder.NotePendingWrite(address + 16, 4);
    recorder.OnComplete([&] { memory[4] = 91; });
    Expect(!DriverDetail::TryParseTimeLabelPacket(write) && !Pm4::WaitSatisfiedUnchecked(wait), "a label ordering a copied write-back published early");
    const std::uint32_t value = 7;
    recorder.AfterCompletions(address, std::as_bytes(std::span(&value, 1)), 1, 0, false);
    Recorder::LabelRefusal refusal{};
    Expect(Recorder::LookupLabel(address, 4, 0, &refusal).has_value() == !enabled, "a newly received wait bypassed a pending copied write-back");
    Expect(!enabled || refusal == Recorder::LabelRefusal::BehindCompletion, "a copied write-back wait lost its completion refusal");
    Expect(!Recorder::LookupLabel(address, 4, 2, &refusal).has_value() && refusal == Recorder::LabelRefusal::BehindCompletion, "a completion label was trusted before its copied write-back");
    Expect(memory[0] == 0 && memory[4] == 0, "copied write-back or label ran before completion");
    recorder.Sync();
    Expect(memory[4] == 91 && Pm4::WaitSatisfiedUnchecked(wait), "label wait did not converge after its copied write-back");
    const auto counts = Recorder::ParseLabelCounts();
    Expect(counts.parsed - labelsBefore.parsed == (enabled ? 3u : 0u), "parse-time label counter included a refused label");
}

}

int main(int argc, char** argv) {
    try {
#if defined(ANYPS5_AFTERMATH_TESTING)
        aftermathRecorderReleaseTest();
        std::puts("Aftermath capture precedes failed recorder batch release");
        return 0;
#endif
        if (argc == 2 && std::string_view(argv[1]) == "compute-chains") {
            ComputeChainTests();
            if (Recorder::RecordWorkerEnabled()) RecordWorkerOrderAndSync();
        } else if (argc == 2 && std::string_view(argv[1]) == "record-worker") {
            RecordWorkerOrderAndSync();
        } else if (argc == 2 && std::string_view(argv[1]) == "draw-worker") {
            DrawWorkerPassOrder();
        } else if (argc == 2 && std::string_view(argv[1]) == "parse-time-labels") {
            ParseTimeLabelTests();
        } else if (argc == 2 && std::string_view(argv[1]) == "submit-worker") {
            SubmitWorkerOrderAndSync();
            SubmitWorkerDirectBatch();
            SubmitWorkerFailure(true);
            SubmitWorkerFailure(false);
            SubmitWorkerFailure(true, 2);
            SubmitWorkerFailure(false, 2);
            SubmitWorkerFailure(false, 1, true);
        } else if (argc == 2 && std::string_view(argv[1]) == "background") {
            BackgroundRelease(false);
            BackgroundRelease(true);
            BackgroundForcedRelease();
        } else {
            OpenBatchUnderBudget();
            InFlightWithinTwiceTheBudget();
            SyncedWithinTwiceTheBudget();
            CountFollowsEveryPath();
            SharedOwnerCountsOncePerBatch();
        }
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
