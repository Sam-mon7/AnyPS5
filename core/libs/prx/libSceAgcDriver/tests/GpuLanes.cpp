#include "prx/libSceAgcDriver/Execution/include/Driver/Lanes/Scheduler.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Lanes/VulkanQueues.hpp"
#include <atomic>
#include <barrier>
#include <iostream>
#include <string>

using namespace AgcDriver::DriverDetail::Lanes;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Ordering() {
    WorkerPool pool(2);
    std::promise<void> release, secondReady;
    auto gate = release.get_future().share();
    auto ready = secondReady.get_future();
    std::vector<int> committed;
    int target = 0, sampled = 0;
    auto first = pool.Enqueue({0, 1, [&](std::size_t) -> Commit {
        gate.wait();
        return [&] { target = 42; committed.push_back(1); };
    }});
    auto second = pool.Enqueue({0, 2, [&](std::size_t) -> Commit {
        secondReady.set_value();
        return [&] { sampled = target; committed.push_back(2); };
    }});
    const auto status = ready.wait_for(std::chrono::seconds(2));
    release.set_value();
    first.get();
    second.get();
    Require(status == std::future_status::ready, "preparation did not run in parallel");
    Require(committed == std::vector<int>({1, 2}) && sampled == 42, "producer/consumer commits were reordered");
}

void Failure() {
    WorkerPool pool(2);
    bool committed = false;
    auto first = pool.Enqueue({0, 1, [](std::size_t) -> Commit { throw std::runtime_error("prepare failure"); }});
    auto second = pool.Enqueue({0, 2, [&](std::size_t) -> Commit { return [&] { committed = true; }; }});
    int failed = 0;
    for (auto* result : {&first, &second}) try { result->get(); } catch (const std::runtime_error&) { ++failed; }
    Require(failed == 2 && !committed, "a failed predecessor did not cancel later commits");
}

void Suspension() {
    Scheduler scheduler;
    std::atomic<int> label{0};
    int consumer = 0, producer = 0, finished = 0, polls = 0, suspended = 0;
    bool producerFinished = false;
    const auto parserThread = std::this_thread::get_id();
    auto finish = [&](std::exception_ptr error) { if (error) std::rethrow_exception(error); ++finished; };
    scheduler.Enqueue(0, {[&]() -> Step {
        Require(std::this_thread::get_id() == parserThread, "parser migrated to a queue thread");
        if (consumer == 1) return {{}, true};
        if (label.load(std::memory_order_acquire) != 42) { ++suspended; return {}; }
        ++consumer;
        return {Chunk{0, 0, [&](std::size_t) -> Commit { return [&] { Require(label.load() == 42, "wait resumed before its producer"); }; }}, false};
    }, finish});
    scheduler.Enqueue(0x20, {[&]() -> Step {
        if (producer++ != 0) return {{}, true};
        return {Chunk{0x20, 0, [&](std::size_t) -> Commit { return [&] { label.store(42, std::memory_order_release); }; }}, false};
    }, [&](std::exception_ptr error) { finish(error); producerFinished = true; }});
    scheduler.Enqueue(0x20, {[&]() -> Step { Require(producerFinished, "same-queue submissions overlapped"); return {{}, true}; }, finish});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    scheduler.Run([&] { return finished == 3 || std::chrono::steady_clock::now() >= deadline; }, [&] { ++polls; }, 2);
    Require(finished == 3 && consumer == 1 && suspended != 0 && polls != 0, "suspended queue prevented another queue from making progress");
}

void Cancellation() {
    Scheduler scheduler;
    int polls = 0, cancelled = 0;
    for (int index = 0; index < 2; ++index) scheduler.Enqueue(0, {[] { return Step{}; }, [&](std::exception_ptr error) {
        Require(error != nullptr, "shutdown completed a suspended cursor successfully");
        ++cancelled;
    }});
    scheduler.Run([&] { return polls == 3; }, [&] { ++polls; }, 1);
    Require(cancelled == 2, "shutdown left active or pending cursors behind");
}

std::atomic<std::uintptr_t> nextHandle{100};
std::atomic<int> poolsCreated{0}, poolsDestroyed{0};
std::vector<Dependency> submittedWaits;
VkQueue lastQueue = VK_NULL_HANDLE;

template<class T> T Handle(std::uintptr_t value) { return reinterpret_cast<T>(value); }
VKAPI_ATTR void VKAPI_CALL GetQueue(VkDevice, std::uint32_t family, std::uint32_t index, VkQueue* queue) { *queue = Handle<VkQueue>(1 + family * 64 + index); }
VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice, const VkSemaphoreCreateInfo* info, const VkAllocationCallbacks*, VkSemaphore* semaphore) {
    Require(static_cast<const VkSemaphoreTypeCreateInfo*>(info->pNext)->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE, "dependency semaphore is not a timeline");
    *semaphore = Handle<VkSemaphore>(nextHandle.fetch_add(1));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* info, VkFence) {
    Require(count == 1 && info->signalSemaphoreCount == 1, "missing queue completion signal");
    const auto* timeline = static_cast<const VkTimelineSemaphoreSubmitInfo*>(info->pNext);
    Require(timeline->waitSemaphoreValueCount == info->waitSemaphoreCount, "timeline wait count differs");
    submittedWaits.clear();
    for (std::uint32_t index = 0; index < info->waitSemaphoreCount; ++index) {
        Require(info->pWaitDstStageMask[index] == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, "dependency does not cover all commands");
        submittedWaits.push_back({info->pWaitSemaphores[index], timeline->pWaitSemaphoreValues[index]});
    }
    lastQueue = queue;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL CreatePool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* pool) {
    ++poolsCreated;
    *pool = Handle<VkCommandPool>(nextHandle.fetch_add(1));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) { ++poolsDestroyed; }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Proc(VkDevice, const char* name) {
    if (std::strcmp(name, "vkGetDeviceQueue") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&GetQueue);
    if (std::strcmp(name, "vkCreateSemaphore") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CreateSemaphore);
    if (std::strcmp(name, "vkDestroySemaphore") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&DestroySemaphore);
    if (std::strcmp(name, "vkQueueSubmit") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Submit);
    if (std::strcmp(name, "vkCreateCommandPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CreatePool);
    if (std::strcmp(name, "vkDestroyCommandPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&DestroyPool);
    throw std::runtime_error(name);
}

void Queues() {
    std::array<VkQueueFamilyProperties, 3> families{};
    families[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    families[0].queueCount = 1;
    families[1].queueFlags = VK_QUEUE_COMPUTE_BIT;
    families[1].queueCount = 2;
    auto off = SelectQueues(families, 0, false);
    auto on = SelectQueues(families, 0, true);
    Require(!off.Dedicated() && on.compute == 1 && on.count == 2, "compute family selection changed the off path");
    auto ordered = [](const std::function<VkResult()>& work) { return work(); };
    {
        VulkanQueues queues(Handle<VkDevice>(1), on, &Proc, true, ordered);
        Require(queues.Queue(0x20) != queues.Queue(0) && queues.Queue(0x20) != queues.Queue(0x21), "guest compute queues did not get compute VkQueues");
        Require(queues.Queue(0x22) == queues.Queue(0x20), "limited compute queues did not share deterministically");
        const auto first = queues.Submit(0x20, {});
        const auto second = queues.Submit(0x20, {});
        const std::array<Dependency, 2> dependencies{first, second};
        const auto graphics = queues.Submit(0, {}, dependencies);
        Require(lastQueue == queues.Queue(0) && submittedWaits.size() == 1 && submittedWaits[0].value == 2 && submittedWaits[0].semaphore == first.semaphore, "compute-to-graphics dependencies were lost or duplicated");
        queues.Submit(0x21, {}, std::span(&graphics, 1));
        Require(lastQueue == queues.Queue(0x21) && submittedWaits[0].semaphore == graphics.semaphore, "graphics-to-compute dependency was lost");
        std::barrier rendezvous(2);
        WorkerPool pool(2);
        std::array<std::future<void>, 2> work;
        std::array<VkCommandPool, 2> handles{};
        for (std::size_t index = 0; index < 2; ++index) work[index] = pool.Enqueue({0x20, index, [&](std::size_t worker) -> Commit {
            auto& pools = queues.Pools(0x20);
            handles[worker] = pools.Get(worker);
            Require(handles[worker] == pools.Get(worker), "worker did not reuse its command pool");
            rendezvous.arrive_and_wait();
            return {};
        }});
        for (auto& result : work) result.get();
        Require(handles[0] != VK_NULL_HANDLE && handles[1] != VK_NULL_HANDLE && handles[0] != handles[1], "workers shared a command pool");
        bool refused = false;
        try { queues.Pools(0x20).Get(0); } catch (const std::runtime_error&) { refused = true; }
        Require(refused, "a command pool accepted another worker thread");
    }
    Require(poolsCreated == 2 && poolsDestroyed == 2, "worker command pools leaked");
    families[1].queueCount = 0;
    Require(!SelectQueues(families, 0, true).Dedicated(), "empty compute family was selected");
    VulkanQueues fallback(Handle<VkDevice>(1), off, &Proc, false, ordered);
    Require(fallback.Queue(0x20) == fallback.Queue(0), "fallback queue changed");
    bool refused = false;
    try { fallback.Submit(0, {}); } catch (const std::runtime_error&) { refused = true; }
    Require(refused, "timeline-free dependencies were silently accepted");
    fallback.Close();
    refused = false;
    try { fallback.Pools(0).Get(0); } catch (const std::runtime_error&) { refused = true; }
    Require(refused, "closed command pools remained usable");
}

}

int main() {
    try {
        const auto* value = std::getenv("APS5_GPU_LANES");
        Require(Enabled() == (value != nullptr && std::strcmp(value, "1") == 0), "lane switch is not exact/default-off");
        const auto* async = std::getenv("APS5_ASYNC_COMPUTE_QUEUES");
        Require(AsyncComputeEnabled() == (Enabled() && async != nullptr && std::strcmp(async, "1") == 0), "async switch is not exact/default-off");
        Ordering();
        Failure();
        Suspension();
        Cancellation();
        Queues();
        std::cout << "GPU lanes tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
