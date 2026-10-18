#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
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
};

MockDevice mock;
std::mutex mockMutex;

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
VKAPI_ATTR void VKAPI_CALL mockCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBeginQuery(VkCommandBuffer, VkQueryPool, std::uint32_t, VkQueryControlFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdEndQuery(VkCommandBuffer, VkQueryPool, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdResetQueryPool(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL mockCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t, const void*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdDispatch(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t) {}

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

}

int main(int argc, char** argv) {
    try {
#if defined(ANYPS5_AFTERMATH_TESTING)
        aftermathRecorderReleaseTest();
        std::puts("Aftermath capture precedes failed recorder batch release");
        return 0;
#endif
        if (argc == 2 && std::string_view(argv[1]) == "submit-worker") {
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
