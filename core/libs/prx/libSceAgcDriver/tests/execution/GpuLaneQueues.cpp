#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Lanes/VulkanQueues.hpp"
#include <array>
#include <iostream>
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

using namespace AgcDriver::DriverDetail::Lanes;

namespace {

void Check(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("GPU lane Vulkan test failed: " + std::to_string(result));
}

void LiveDispatch(AgcDriver::VulkanDevice& device, const std::shared_ptr<VulkanQueues>& queues, bool recorders) {
    constexpr std::size_t bytes = 65536;
#ifdef _WIN32
    auto* output = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    auto* output = static_cast<std::uint32_t*>(std::aligned_alloc(bytes, bytes));
#endif
    if (output == nullptr) throw std::runtime_error("cannot allocate live lane buffer");
    GuestAllocations::Mutation().Add(output, bytes, true, true, true);
    auto release = [&] {
        GuestAllocations::Mutation().Remove(output);
#ifdef _WIN32
        VirtualFree(output, 0, MEM_RELEASE);
#else
        std::free(output);
#endif
    };
    try {
        std::memset(output, 0, bytes);
        {
            const auto range = GuestAllocations::GuestAllocationsLookup_nid_postfix(reinterpret_cast<std::uintptr_t>(output), false);
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            device.ImportGuestMemory(GuestAllocations::Mapped{range}, GuestAllocations::GuestAllocationsGeneration_nid_postfix(), true);
        }
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        const bool imported = AgcDriver::Graphics::HostImportLiveBytes() != 0;
        alignas(256) constexpr std::array<std::uint32_t, 4> code{0x7e000281u, 0xe0c80000u, 0x80000000u, 0xbf810000u};
        const auto address = reinterpret_cast<std::uintptr_t>(output);
        const std::array<std::uint32_t, 4> data{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) & 0xffffu, 4, 0x31016facu};
        const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
        const ShaderRecompiler::ShaderComputeStageInfo compute{{1, 1, 1}, 0u, {false, false, false}, false, 1};
        ShaderRecompiler::RecompileRequest request{
            {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
            {64, 0, data, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}
        };
        request.useCache = false;
        const auto shader = ShaderRecompiler::Recompile(request);
        const auto before = queues ? queues->RoutedDispatches() : 0;
        const auto recordedBefore = AgcDriver::Graphics::Recorder::LaneRecordCounts();
        for (const auto queue : {0u, 0x20u, 0x21u, 0u}) {
            const QueueScope scope(queue);
            device.Dispatch(shader, 1, 1, 1);
        }
        if (recorders) {
            const QueueScope scope(0);
            for (unsigned dispatch = 0; dispatch < 512; ++dispatch) device.Dispatch(shader, 1, 1, 1);
        }
        const std::uint32_t label = 0x12345678u;
        const auto labelPath = device.WriteLabelOnGpu(address + 256, std::as_bytes(std::span(&label, 1)), 1, 0x21);
        if (imported && !AgcDriver::Graphics::ResidentHeapMirrors() && labelPath != 0) throw std::runtime_error("lane pool retirement forced a GPU label onto CPU completion");
        device.WaitIdle();
        const auto expected = recorders ? 516u : 4u;
        if (output[0] != expected) throw std::runtime_error("live lane dispatch ownership chain returned " + std::to_string(output[0]) + ", expected " + std::to_string(expected));
        const auto recordedAfter = AgcDriver::Graphics::Recorder::LaneRecordCounts();
        if (recorders && AgcDriver::Graphics::Recorder::LaneRecordersEnabled() && recordedAfter.commands - recordedBefore.commands < 512) throw std::runtime_error("live dispatches bypassed the lane recorders");
        const auto after = queues ? queues->RoutedDispatches() : 0;
        if (AsyncComputeEnabled() && queues && queues->Dedicated() && queues->Timeline() && imported && !AgcDriver::Graphics::ResidentHeapMirrors()) {
            if (after - before != 2) throw std::runtime_error("eligible live dispatches did not use compute queues");
            if (output[64] != label) throw std::runtime_error("the routed dispatch label did not reach guest memory");
            queues->WaitLabel(0x20, address + 256, sizeof(label));
            if (queues->LabelDependencies(0x20).size() != 1) throw std::runtime_error("the live GPU label did not contribute a timeline dependency");
        } else if (after != before) throw std::runtime_error("fallback dispatches used compute queues");
        std::cout << "Live dispatch ownership chain passed; routed=" << after - before << ", imported=" << imported << '\n';
    } catch (...) {
        device.WaitIdle();
        release();
        throw;
    }
    release();
}

}

int main(int argc, char** argv) {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        auto queues = FindQueues(device->Device());
        LiveDispatch(*device, queues, argc == 2 && std::string_view(argv[1]) == "--lane-recorders");
        if (!Enabled()) {
            if (queues) throw std::runtime_error("compute queues were published with the switch off");
            return 0;
        }
        if (!queues) throw std::runtime_error("compute queues were not published");
        if (!AsyncComputeEnabled() && queues->Dedicated()) throw std::runtime_error("compute-only queues were selected with the async switch off");
        if (!queues->Timeline()) return VulkanTestSkipped;
        const auto allocate = reinterpret_cast<PFN_vkAllocateCommandBuffers>(queues->Function("vkAllocateCommandBuffers"));
        const auto begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(queues->Function("vkBeginCommandBuffer"));
        const auto end = reinterpret_cast<PFN_vkEndCommandBuffer>(queues->Function("vkEndCommandBuffer"));
        const auto createFence = reinterpret_cast<PFN_vkCreateFence>(queues->Function("vkCreateFence"));
        const auto wait = reinterpret_cast<PFN_vkWaitForFences>(queues->Function("vkWaitForFences"));
        const auto destroyFence = reinterpret_cast<PFN_vkDestroyFence>(queues->Function("vkDestroyFence"));
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(createFence(device->Device(), &fenceInfo, nullptr, &fence));
        {
            WorkerPool pool;
            Dependency previous;
            std::array<std::future<void>, 8> work;
            for (std::size_t index = 0; index < work.size(); ++index) {
                const auto guest = index % 2 == 0 ? 0u : 0x20u + static_cast<std::uint32_t>(index / 2);
                work[index] = pool.Enqueue({guest, index, [&, guest, index](std::size_t worker) -> Commit {
                    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                    info.commandPool = queues->Pools(guest).Get(worker);
                    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    info.commandBufferCount = 1;
                    VkCommandBuffer command = VK_NULL_HANDLE;
                    Check(allocate(device->Device(), &info, &command));
                    VkCommandBufferBeginInfo start{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    Check(begin(command, &start));
                    Check(end(command));
                    return [&, guest, command, index] {
                        const auto dependencies = previous.semaphore == VK_NULL_HANDLE ? std::span<const Dependency>{} : std::span<const Dependency>(&previous, 1);
                        previous = queues->Submit(guest, std::span(&command, 1), dependencies, index + 1 == work.size() ? fence : VK_NULL_HANDLE);
                    };
                }});
            }
            for (auto& result : work) result.get();
            Check(wait(device->Device(), 1, &fence, VK_TRUE, 5'000'000'000ull));
        }
        destroyFence(device->Device(), fence, nullptr);
        std::cout << "GPU lane semaphore chain passed; dedicated compute=" << queues->Dedicated() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
