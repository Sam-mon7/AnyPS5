#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramBudget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GpuCopyCheckpoints.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RebarBudget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

std::vector<int> copyEvents;
const void* expectedCopyRegions = nullptr;
std::uint32_t expectedCopyCount = 0;

template<typename TSource, typename TDestination, typename TRegion>
void ExpectCopy(VkCommandBuffer commands, TSource source, TDestination destination, std::uint32_t count, const TRegion* regions) {
    Expect(GpuCopyCheckpoints::Handle(commands) == 0x30 && GpuCopyCheckpoints::Handle(source) == 0x10 && GpuCopyCheckpoints::Handle(destination) == 0x20, "copy wrapper changed a command or resource handle");
    Expect(count == expectedCopyCount && (expectedCopyRegions == nullptr || regions == expectedCopyRegions), "copy wrapper split or replaced its region array");
    copyEvents.push_back(0);
}

VKAPI_ATTR void VKAPI_CALL mockCopyMarker(VkCommandBuffer commands, const void* marker) {
    const auto value = reinterpret_cast<std::uintptr_t>(marker);
    Expect(GpuCopyCheckpoints::Handle(commands) == 0x30 && GpuCopyCheckpoints::IsCopy(value), "copy wrapper emitted an invalid marker");
    copyEvents.push_back(GpuCopyCheckpoints::IsAfter(value) ? 1 : -1);
}

VKAPI_ATTR void VKAPI_CALL mockCopyBuffers(VkCommandBuffer commands, VkBuffer source, VkBuffer destination, std::uint32_t count, const VkBufferCopy* regions) {
    ExpectCopy(commands, source, destination, count, regions);
    if (expectedCopyRegions == nullptr) Expect(regions[0].srcOffset == 3 && regions[0].dstOffset == 7 && regions[0].size == 64, "single-range copy wrapper changed its offsets or length");
}

VKAPI_ATTR void VKAPI_CALL mockCopyBufferToImage(VkCommandBuffer commands, VkBuffer source, VkImage destination, VkImageLayout layout, std::uint32_t count, const VkBufferImageCopy* regions) {
    ExpectCopy(commands, source, destination, count, regions);
    Expect(layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, "buffer-to-image wrapper changed its layout");
}

VKAPI_ATTR void VKAPI_CALL mockCopyImageToBuffer(VkCommandBuffer commands, VkImage source, VkImageLayout layout, VkBuffer destination, std::uint32_t count, const VkBufferImageCopy* regions) {
    ExpectCopy(commands, source, destination, count, regions);
    Expect(layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, "image-to-buffer wrapper changed its layout");
}

VKAPI_ATTR void VKAPI_CALL mockCopyImages(VkCommandBuffer commands, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, std::uint32_t count, const VkImageCopy* regions) {
    ExpectCopy(commands, source, destination, count, regions);
    Expect(sourceLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && destinationLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, "image copy wrapper changed a layout");
}

VKAPI_ATTR void VKAPI_CALL mockBlitImages(VkCommandBuffer commands, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, std::uint32_t count, const VkImageBlit* regions, VkFilter filter) {
    ExpectCopy(commands, source, destination, count, regions);
    Expect(sourceLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && destinationLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && filter == VK_FILTER_LINEAR, "blit wrapper changed a layout or filter");
}

VKAPI_ATTR void VKAPI_CALL mockCopyQueries(VkCommandBuffer commands, VkQueryPool pool, std::uint32_t first, std::uint32_t count, VkBuffer destination, VkDeviceSize offset, VkDeviceSize stride, VkQueryResultFlags flags) {
    ExpectCopy(commands, pool, destination, count, static_cast<const VkBufferCopy*>(nullptr));
    Expect(first == 9 && offset == 0x100000008ull && stride == 8 && flags == (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "query copy wrapper changed its range, offset or flags");
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mockCopyProc(VkDevice, const char* name) {
    if (std::strcmp(name, "vkCmdCopyBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(mockCopyBuffers);
    if (std::strcmp(name, "vkCmdCopyBufferToImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(mockCopyBufferToImage);
    if (std::strcmp(name, "vkCmdCopyImageToBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(mockCopyImageToBuffer);
    if (std::strcmp(name, "vkCmdCopyImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(mockCopyImages);
    if (std::strcmp(name, "vkCmdBlitImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(mockBlitImages);
    if (std::strcmp(name, "vkCmdCopyQueryPoolResults") == 0) return reinterpret_cast<PFN_vkVoidFunction>(mockCopyQueries);
    return nullptr;
}

void CopyDiagnostics() {
    const auto commands = reinterpret_cast<VkCommandBuffer>(std::uintptr_t{0x30});
    const auto sourceBuffer = reinterpret_cast<VkBuffer>(std::uintptr_t{0x10});
    const auto destinationBuffer = reinterpret_cast<VkBuffer>(std::uintptr_t{0x20});
    const auto sourceImage = reinterpret_cast<VkImage>(std::uintptr_t{0x10});
    const auto destinationImage = reinterpret_cast<VkImage>(std::uintptr_t{0x20});
    const auto pool = reinterpret_cast<VkQueryPool>(std::uintptr_t{0x10});
    const VkBufferCopy buffers[]{{3, 7, 64}, {100, 200, 300}};
    const VkBufferImageCopy bufferImages[2]{{.bufferOffset = 0x100000008ull, .imageExtent = {9, 7, 3}}, {.bufferOffset = 256}};
    const VkImageCopy images[2]{{.extent = {9, 7, 3}}, {.extent = {4, 2, 1}}};
    const VkImageBlit blits[2]{{.srcOffsets = {{1, 2, 3}, {9, 7, 4}}}, {}};
    for (const bool enabled : {false, true}) for (const bool cached : {false, true}) {
        Context context{};
        context.deviceProc = mockCopyProc;
        DeviceFunctions functions{};
        functions.cmdCopyBuffer = mockCopyBuffers;
        functions.cmdCopyBufferToImage = mockCopyBufferToImage;
        functions.cmdCopyImageToBuffer = mockCopyImageToBuffer;
        functions.cmdCopyQueryPoolResults = mockCopyQueries;
        context.functions = cached ? &functions : nullptr;
        if (enabled) context.copyCheckpoints = std::make_shared<GpuCopyCheckpoints>(VK_NULL_HANDLE, mockCopyMarker);
        const auto check = [&](const void* regions, std::uint32_t count, auto invoke) {
            copyEvents.clear();
            expectedCopyRegions = regions;
            expectedCopyCount = count;
            invoke();
            Expect(copyEvents == (enabled ? std::vector<int>{-1, 0, 1} : std::vector<int>{0}), "copy diagnostics changed call order, duplicated a copy or emitted disabled markers");
        };
        check(nullptr, 1, [&] { CopyBuffer(context, commands, sourceBuffer, 3, destinationBuffer, 7, 64); });
        check(buffers, 2, [&] { CopyBuffers(context, commands, sourceBuffer, destinationBuffer, 2, buffers); });
        check(bufferImages, 2, [&] { CopyBufferToImage(context, commands, sourceBuffer, destinationImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, bufferImages); });
        check(bufferImages, 2, [&] { CopyImageToBuffer(context, commands, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destinationBuffer, 2, bufferImages); });
        check(images, 2, [&] { CopyImage(context, commands, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destinationImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, images); });
        check(blits, 2, [&] { BlitImage(context, commands, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destinationImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, blits, VK_FILTER_LINEAR); });
        check(nullptr, 3, [&] { CopyQueryResults(context, commands, pool, 9, 3, destinationBuffer, 0x100000008ull, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT); });
        if (enabled) {
            GpuCopyCheckpoints::Entry entry;
            Expect(context.copyCheckpoints->Find(4, entry) && entry.kind == GpuCopyCheckpoints::Kind::ImageToBuffer && std::memcmp(entry.firstRegion.data(), bufferImages, sizeof(bufferImages[0])) == 0, "image copy diagnostic did not retain its first region");
            Expect(context.copyCheckpoints->Find(7, entry) && entry.kind == GpuCopyCheckpoints::Kind::QueryResults && entry.regionWords == 10, "query copy diagnostic did not retain its argument tuple");
        }
    }
}

constexpr std::size_t MiB = std::size_t{1} << 20u;
constexpr VkMemoryPropertyFlags HostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

struct MockDevice {
    std::uint64_t next = 1;
    std::map<VkBuffer, VkDeviceSize> sizes;
    std::map<VkBuffer, VkBufferUsageFlags> usages;
    std::map<VkDeviceMemory, std::vector<std::byte>> hostMemory;
    std::uint64_t allocations = 0;
    std::uint64_t frees = 0;
    std::uint64_t destroyedBuffers = 0;
    VkDeviceSize liveBytes = 0;
    std::map<VkDeviceMemory, VkDeviceSize> memoryBytes;
    std::map<VkDeviceMemory, std::uint32_t> memoryTypes;
    std::map<VkBuffer, std::pair<VkDeviceMemory, VkDeviceSize>> bound;
    std::set<VkBuffer> liveBuffers;
    std::deque<VkResult> results;
    VkDeviceSize limit = 0;
    VkResult pressureResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    std::uint64_t attempts = 0;
    VkDeviceSize heapBudget = 0;
    VkDeviceSize otherUsage = 0;
    std::uint64_t budgetQueries = 0;
};

MockDevice mock;

VKAPI_ATTR VkResult VKAPI_CALL mockCreateBuffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* buffer) {
    *buffer = reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(mock.next++));
    mock.sizes[*buffer] = info->size;
    mock.usages[*buffer] = info->usage;
    mock.liveBuffers.insert(*buffer);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockGetBufferMemoryRequirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* requirements) {
    *requirements = {mock.sizes.at(buffer), 256, 7};
}

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    ++mock.attempts;
    auto result = VK_SUCCESS;
    if (!mock.results.empty()) {
        result = mock.results.front();
        mock.results.pop_front();
    }
    if (result == VK_SUCCESS && mock.limit != 0 && mock.liveBytes + info->allocationSize > mock.limit) result = mock.pressureResult;
    if (result != VK_SUCCESS) {
        *memory = reinterpret_cast<VkDeviceMemory>(std::uintptr_t{0xdeadbeef});
        return result;
    }
    *memory = reinterpret_cast<VkDeviceMemory>(static_cast<std::uintptr_t>(mock.next++));
    if (info->memoryTypeIndex != 0) mock.hostMemory[*memory].resize(info->allocationSize);
    mock.memoryTypes[*memory] = info->memoryTypeIndex;
    mock.memoryBytes[*memory] = info->allocationSize;
    mock.liveBytes += info->allocationSize;
    ++mock.allocations;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBindBufferMemory(VkDevice, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    if (offset + mock.sizes.at(buffer) > mock.memoryBytes.at(memory)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    mock.bound[buffer] = {memory, offset};
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockMapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void** data) {
    *data = mock.hostMemory.at(memory).data();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockUnmapMemory(VkDevice, VkDeviceMemory) {}

VKAPI_ATTR void VKAPI_CALL mockDestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) {
    Expect(mock.liveBuffers.erase(buffer) == 1, "an unknown buffer was destroyed");
    ++mock.destroyedBuffers;
}

VKAPI_ATTR void VKAPI_CALL mockFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    if (!mock.memoryBytes.contains(memory)) {
        Expect(false, "an undefined failed allocation output was freed");
        return;
    }
    mock.liveBytes -= mock.memoryBytes.at(memory);
    mock.memoryBytes.erase(memory);
    mock.hostMemory.erase(memory);
    ++mock.frees;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL mockGetBufferDeviceAddress(VkDevice, const VkBufferDeviceAddressInfo* info) {
    return 0x100000000000ULL + reinterpret_cast<std::uintptr_t>(info->buffer) * 0x10000;
}

VKAPI_ATTR void VKAPI_CALL mockMemoryProperties2(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2* properties) {
    ++mock.budgetQueries;
    for (auto* next = static_cast<VkBaseOutStructure*>(properties->pNext); next != nullptr; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) continue;
        auto* reported = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(next);
        reported->heapBudget[0] = mock.heapBudget;
        reported->heapUsage[0] = mock.otherUsage + mock.liveBytes;
    }
}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkCreateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCreateBuffer)},
        {"vkGetBufferMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(mockGetBufferMemoryRequirements)},
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateMemory)},
        {"vkBindBufferMemory", reinterpret_cast<PFN_vkVoidFunction>(mockBindBufferMemory)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(mockMapMemory)},
        {"vkUnmapMemory", reinterpret_cast<PFN_vkVoidFunction>(mockUnmapMemory)},
        {"vkDestroyBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyBuffer)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(mockFreeMemory)},
        {"vkGetBufferDeviceAddressKHR", reinterpret_cast<PFN_vkVoidFunction>(mockGetBufferDeviceAddress)},
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

VkDeviceMemory MemoryOf(VkBuffer buffer) {
    return mock.bound.at(buffer).first;
}

VkDeviceSize ClassOf(VkBuffer buffer) {
    return mock.memoryBytes.at(MemoryOf(buffer));
}

Context mockContext() {
    Context context{};
    context.deviceProc = mockProc;
    context.memory.memoryTypeCount = 2;
    context.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    context.memory.memoryTypes[1].propertyFlags = HostVisible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    context.limits.maxMemoryAllocationCount = 1u << 20u;
    context.bufferDeviceAddress = true;
    return context;
}

constexpr VkDeviceSize HeapBytes = VkDeviceSize{8} << 30u;
constexpr VkDeviceSize HeapTarget = HeapBytes - 512 * MiB;
constexpr VkBufferUsageFlags BudgetUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

Context budgetContext(bool integrated = false) {
    auto context = mockContext();
    context.memory.memoryHeapCount = 2;
    context.memory.memoryHeaps[0] = {HeapBytes, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    context.memory.memoryHeaps[1] = {HeapBytes * 2, 0};
    context.memory.memoryTypes[1].heapIndex = 1;
    context.physical = reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{1});
    context.memoryProperties2 = mockMemoryProperties2;
    context.integratedGpu = integrated;
    return context;
}

void releaseFour(const Context& context) {
    std::vector<std::unique_ptr<DeviceBuffer>> live;
    for (int i = 0; i < 4; ++i) live.push_back(std::make_unique<DeviceBuffer>(context, 64 * MiB, BudgetUsage));
    live.clear();
}

void VramTargets() {
    Expect(VramBudget::HeadroomFor(VkDeviceSize{16} << 30u) == 1024 * MiB, "a 16 GiB heap does not keep 1 GiB of headroom");
    Expect(VramBudget::HeadroomFor(HeapBytes) == 512 * MiB, "an 8 GiB heap does not keep a sixteenth of it as headroom");
    Expect(VramBudget::HeadroomFor(VkDeviceSize{2} << 30u) == 256 * MiB, "a 2 GiB heap keeps less than 256 MiB of headroom");
    Expect(VramBudget::TargetFor(HeapBytes, HeapBytes, 512 * MiB) == HeapTarget, "the target is not the budget less the headroom");
    Expect(VramBudget::TargetFor(HeapBytes, 1024 * MiB, 512 * MiB) == 1024 * MiB, "a small budget took the target of an 8 GiB heap below 1 GiB");
    Expect(VramBudget::TargetFor(VkDeviceSize{2} << 30u, 256 * MiB, 256 * MiB) == 512 * MiB, "the target fell below a quarter of a 2 GiB heap");
}

void VramUnderTargetPools() {
    mock = MockDevice{};
    mock.heapBudget = HeapBytes;
    mock.otherUsage = 1024 * MiB;
    auto context = budgetContext();
    releaseFour(context);
    Expect(mock.budgetQueries != 0, "the pool did not read the device-local heap's budget");
    Expect(mock.frees == 0 && mock.liveBytes == 256 * MiB, "released device buffers under the budget target were not pooled (" + std::to_string(mock.frees) + " freed)");
}

void VramOverTargetReleases() {
    mock = MockDevice{};
    mock.heapBudget = HeapBytes;
    mock.otherUsage = HeapTarget;
    auto context = budgetContext();
    releaseFour(context);
    Expect(mock.frees == 4 && mock.liveBytes == 0, "released device buffers were pooled over the budget target (" + std::to_string(mock.liveBytes / MiB) + " MiB retained)");
    const auto made = mock.allocations;
    DeviceBuffer again(context, 64 * MiB, BudgetUsage);
    Expect(mock.allocations == made + 1, "a device buffer was served from a pool that released everything over the target");
}

void VramTrimsPool() {
    mock = MockDevice{};
    mock.heapBudget = HeapBytes;
    mock.otherUsage = HeapTarget - 512 * MiB;
    auto context = budgetContext();
    releaseFour(context);
    Expect(mock.frees == 0 && mock.liveBytes == 256 * MiB, "released device buffers under the budget target were not pooled");
    mock.otherUsage = HeapTarget - 64 * MiB;
    GetBufferPool(context)->Vram().Refresh();
    const auto made = mock.allocations;
    {
        DeviceBuffer reused(context, 64 * MiB, BudgetUsage);
        Expect(mock.allocations == made, "a pooled device buffer was not reused");
    }
    Expect(mock.frees == 3 && mock.liveBytes == 64 * MiB, "the pool did not release its oldest device buffers down to the target (" + std::to_string(mock.frees) + " freed, " + std::to_string(mock.liveBytes / MiB) + " MiB left)");
}

void VramIntegratedKeepsPool() {
    mock = MockDevice{};
    mock.heapBudget = HeapBytes;
    mock.otherUsage = HeapTarget;
    auto context = budgetContext(true);
    releaseFour(context);
    Expect(mock.budgetQueries == 0 && mock.frees == 0 && mock.liveBytes == 256 * MiB, "an integrated GPU's pool followed the device-local heap's budget");
}

void SizeClassesAndDirections() {
    mock = MockDevice{};
    auto context = mockContext();
    VkDeviceMemory first = VK_NULL_HANDLE;
    {
        DeviceBuffer upload(context, 10 * MiB + 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        first = MemoryOf(upload.Handle());
        Expect(mock.sizes.at(upload.Handle()) == 10 * MiB + 4096, "a 10 MiB + 4 KiB device buffer was created with " + std::to_string(mock.sizes.at(upload.Handle())) + " bytes, not the requested size");
        Expect(ClassOf(upload.Handle()) == 11 * MiB, "a 10 MiB + 4 KiB device buffer has " + std::to_string(ClassOf(upload.Handle())) + " bytes of memory, not its 11 MiB class");
        constexpr VkBufferUsageFlags all = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Expect((mock.usages.at(upload.Handle()) & all) == all, "a device buffer was created without storage and both transfer directions");
        Expect(upload.Size() == 10 * MiB + 4096, "a device buffer reports its class instead of the requested size");
    }
    const auto made = mock.allocations;
    DeviceBuffer writeBack(context, 10 * MiB + 512 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Expect(MemoryOf(writeBack.Handle()) == first && mock.allocations == made, "a retained 11 MiB device buffer did not serve a write-back of another size and direction in its class");
    Expect(writeBack.Size() == 10 * MiB + 512 * 1024, "a reused device buffer reports the retained size");
    Expect(mock.sizes.at(writeBack.Handle()) == 10 * MiB + 512 * 1024, "a reused device buffer kept the VkBuffer of the earlier size");
}

void LargerClass() {
    mock = MockDevice{};
    auto context = mockContext();
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkDeviceMemory twelve = VK_NULL_HANDLE;
    {
        DeviceBuffer large(context, 12 * MiB, usage);
        twelve = MemoryOf(large.Handle());
    }
    {
        DeviceBuffer smaller(context, 7 * MiB, usage);
        Expect(MemoryOf(smaller.Handle()) == twelve, "a 7 MiB request did not take the retained 12 MiB buffer");
    }
    const auto made = mock.allocations;
    {
        DeviceBuffer tooSmall(context, 5 * MiB, usage);
        Expect(MemoryOf(tooSmall.Handle()) != twelve && mock.allocations == made + 1, "a 5 MiB request took a retained buffer of more than twice its size");
        Expect(mock.sizes.at(tooSmall.Handle()) == 5 * MiB, "a 5 MiB device buffer was not created at its own class");
    }
    DeviceBuffer again(context, 12 * MiB, usage);
    Expect(MemoryOf(again.Handle()) == twelve, "a buffer served to a smaller request did not return to its own 12 MiB class");
    DeviceBuffer best(context, 4 * MiB + 1, usage);
    Expect(MemoryOf(best.Handle()) != twelve && ClassOf(best.Handle()) == 5 * MiB, "a 4 MiB + 1 request did not take the smallest retained class that fits (5 MiB)");
}

void KeptUntilFence() {
    mock = MockDevice{};
    auto context = mockContext();
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    std::vector<std::shared_ptr<DeviceBuffer>> batch;
    for (int i = 0; i < 3; ++i) batch.push_back(std::make_shared<DeviceBuffer>(context, 6 * MiB + static_cast<std::size_t>(i + 1) * 4096, usage));
    for (std::size_t i = 0; i < batch.size(); ++i) {
        for (std::size_t j = i + 1; j < batch.size(); ++j) Expect(batch[i]->Handle() != batch[j]->Handle(), "two buffers of one unfinished batch share a VkBuffer");
    }
    const auto kept = batch.front()->Handle();
    const auto made = mock.allocations;
    {
        DeviceBuffer during(context, 6 * MiB, usage);
        Expect(during.Handle() != kept && mock.allocations == made + 1, "a buffer kept by an unfinished batch was handed out again");
    }
    batch.clear();
    std::vector<std::unique_ptr<DeviceBuffer>> after;
    for (int i = 0; i < 3; ++i) after.push_back(std::make_unique<DeviceBuffer>(context, 6 * MiB + 100, usage));
    after.push_back(std::make_unique<DeviceBuffer>(context, 6 * MiB, usage));
    Expect(mock.allocations == made + 1, "the buffers released after the batch fence were not reused (" + std::to_string(mock.allocations - made - 1) + " new allocations)");
}

void Budget() {
    mock = MockDevice{};
    auto context = mockContext();
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    std::vector<std::unique_ptr<DeviceBuffer>> live;
    for (int i = 0; i < 12; ++i) live.push_back(std::make_unique<DeviceBuffer>(context, (48 + static_cast<std::size_t>(i) * 3) * MiB, usage));
    const auto first = MemoryOf(live.front()->Handle());
    VkDeviceSize madeBytes = 0;
    for (const auto& buffer : live) madeBytes += ClassOf(buffer->Handle());
    Expect(madeBytes > 512 * MiB, "the budget case does not exceed the budget");
    for (auto& buffer : live) buffer.reset();
    Expect(mock.frees != 0 && mock.liveBytes <= 512 * MiB, "the device tier retains " + std::to_string(mock.liveBytes / MiB) + " MiB after " + std::to_string(mock.frees) + " evictions, over its 512 MiB budget");
    DeviceBuffer probe(context, 48 * MiB, usage);
    Expect(MemoryOf(probe.Handle()) != first, "the least recently used device buffer survived the eviction");
}

void AddressAndHostUnchanged() {
    mock = MockDevice{};
    auto context = mockContext();
    VkDeviceMemory scratch = VK_NULL_HANDLE;
    {
        DeviceBuffer buffer(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        scratch = MemoryOf(buffer.Handle());
    }
    Buffer addressed(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Expect(MemoryOf(addressed.Handle()) != scratch && addressed.DeviceAddress() != 0, "an addressable device-local buffer took a scratch buffer without a device address");
    Buffer shadow(context, 2 * MiB - 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Expect(MemoryOf(shadow.Handle()) == scratch, "a device-local staging buffer did not share the scratch buffers' class");
    VkBuffer host = VK_NULL_HANDLE;
    {
        Buffer copy(context, 10 * MiB + 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HostVisible);
        host = copy.Handle();
        Expect(mock.sizes.at(host) == 10 * MiB + 4096, "a large host buffer was rounded to a class");
        Expect(mock.usages.at(host) == VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "a host buffer was created with more usages than asked");
    }
    Buffer other(context, 10 * MiB + 8192, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HostVisible);
    Expect(other.Handle() != host, "a large host buffer of another exact size was reused");
    Buffer same(context, 10 * MiB + 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HostVisible);
    Expect(same.Handle() == host, "a large host buffer of the same exact size was not reused");
}

void SmallDeviceClasses() {
    mock = MockDevice{};
    auto context = mockContext();
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkDeviceMemory mebibyte = VK_NULL_HANDLE;
    VkDeviceMemory half = VK_NULL_HANDLE;
    {
        DeviceBuffer one(context, MiB, usage);
        mebibyte = MemoryOf(one.Handle());
    }
    {
        DeviceBuffer smaller(context, 300 * 1024, usage);
        Expect(MemoryOf(smaller.Handle()) != mebibyte && ClassOf(smaller.Handle()) == 512 * 1024, "a 300 KiB request took a 1 MiB buffer or skipped its power-of-two class");
        half = MemoryOf(smaller.Handle());
    }
    DeviceBuffer again(context, 260 * 1024, usage);
    Expect(MemoryOf(again.Handle()) == half, "a 260 KiB request did not take the retained 512 KiB buffer");
    constexpr auto indirect = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer tiny = VK_NULL_HANDLE;
    {
        DeviceBuffer args(context, 20, indirect);
        tiny = args.Handle();
    }
    const auto made = mock.allocations;
    DeviceBuffer nextArgs(context, 20, indirect);
    Expect(nextArgs.Handle() == tiny && mock.allocations == made, "a 20-byte device request did not reuse the retained 256-byte buffer of its class");
}

void Slabs() {
    Expect(BufferPool::SlabEligible(512, 256, 512, 64), "a size class with a smaller alignment and atom was refused a slab");
    Expect(!BufferPool::SlabEligible(512, 1024, 512, 64), "a size class below its alignment was given a slab");
    Expect(!BufferPool::SlabEligible(512, 256, 640, 64), "a buffer needing more than its class was given a slab");
    Expect(!BufferPool::SlabEligible(512, 256, 512, 1024), "a size class below the non-coherent atom was given a slab");
    Expect(!BufferPool::SlabEligible(MiB, 256, MiB, 64), "an exact-size buffer was given a slab");
    mock = MockDevice{};
    {
        auto context = mockContext();
        context.limits.nonCoherentAtomSize = 64;
        constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        {
            Buffer a(context, 300, storage);
            Buffer b(context, 300, storage);
            Buffer c(context, 260, storage);
            Expect(mock.allocations == 1, "buffers of one size class did not share one memory block");
            const auto block = mock.bound.at(a.Handle()).first;
            std::set<VkDeviceSize> offsets;
            for (auto* buffer : {&a, &b, &c}) {
                const auto [memory, offset] = mock.bound.at(buffer->Handle());
                Expect(memory == block, "buffers of one size class were bound to different blocks");
                Expect(offset % 512 == 0, "a slab slot is not aligned to its size class");
                Expect(offsets.insert(offset).second, "two live buffers share a slab slot");
                Expect(buffer->Bytes().data() == mock.hostMemory.at(memory).data() + offset, "a slab buffer's mapping is not its slot");
            }
            std::memset(a.Bytes().data(), 0x11, a.Bytes().size());
            std::memset(b.Bytes().data(), 0x22, b.Bytes().size());
            std::memset(c.Bytes().data(), 0x33, c.Bytes().size());
            Expect(std::all_of(a.Bytes().begin(), a.Bytes().end(), [](std::byte value) { return value == std::byte{0x11}; }), "a neighbouring slab buffer overwrote another");
            Expect(std::all_of(b.Bytes().begin(), b.Bytes().end(), [](std::byte value) { return value == std::byte{0x22}; }), "a neighbouring slab buffer overwrote another");
            Buffer larger(context, 600, storage);
            Expect(mock.allocations == 2 && mock.bound.at(larger.Handle()).first != block, "another size class did not get a block of its own");
        }
        Expect(mock.frees == 0, "releasing slab buffers freed device memory");
        Buffer again(context, 300, storage);
        Expect(mock.allocations == 2, "a released slab buffer was not reused");
    }
    Expect(mock.liveBytes == 0, "slab blocks outlived their pool");
    mock = MockDevice{};
    {
        auto context = mockContext();
        BufferPool pool(context);
        constexpr std::size_t slot = 512 * 1024;
        const auto perBlock = static_cast<std::size_t>(BufferPool::SlabBlockBytes(slot) / slot);
        std::vector<SlabSlot> slots;
        for (std::size_t i = 0; i <= perBlock; ++i) slots.push_back(pool.TakeSlot(context, 1, slot, false));
        Expect(mock.allocations == 2 && pool.SlabBlocks() == 2, "a full block did not open a second one");
        std::set<std::pair<VkDeviceMemory, VkDeviceSize>> distinct;
        for (const auto& taken : slots) distinct.insert({taken.memory, taken.offset});
        Expect(distinct.size() == slots.size(), "a slab handed out one slot twice");
        for (const auto& taken : slots) pool.PutSlot(taken.memory, taken.offset);
        Expect(mock.frees == (VramCleanup() ? 2u : 1u) && pool.SlabBlocks() == (VramCleanup() ? 0u : 1u), "emptied blocks did not follow the configured spare retention");
        const auto reused = pool.TakeSlot(context, 1, slot, false);
        Expect(mock.allocations == (VramCleanup() ? 3u : 2u), "a slab allocation did not follow the configured spare retention");
        pool.PutSlot(reused.memory, reused.offset);
    }
    Expect(mock.liveBytes == 0, "slab blocks outlived their pool");
}

void AllocationRecovery(VkResult pressureResult, bool host) {
    mock = MockDevice{};
    {
        auto context = mockContext();
        const auto make = [&](std::size_t bytes) -> std::shared_ptr<void> {
            if (host) return std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            return std::make_shared<DeviceBuffer>(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        };
        auto active = make(2 * MiB);
        const auto activeBuffer = *mock.liveBuffers.begin();
        const auto activeMemory = mock.memoryBytes.begin()->first;
        { auto idle = make(2 * MiB); }
        mock.limit = 6 * MiB;
        mock.pressureResult = pressureResult;
        const auto before = mock.attempts;
        try {
            auto recovered = make(4 * MiB);
            Expect(mock.attempts == before + 2, "pool pressure did not trigger exactly one allocation retry");
            Expect(mock.frees == 1 && mock.liveBytes == 6 * MiB, "recovery did not release the idle allocation");
            Expect(mock.liveBuffers.contains(activeBuffer) && mock.memoryBytes.contains(activeMemory), "recovery destroyed an active allocation");
        } catch (const std::exception& error) {
            Expect(false, std::string("idle memory did not permit recovery: ") + error.what());
        }
    }
    Expect(mock.liveBytes == 0 && mock.liveBuffers.empty(), "recovery leaked a resource");
}

void SlabAllocationRecovery() {
    mock = MockDevice{};
    {
        auto context = mockContext();
        Buffer active(context, 300, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        const auto activeMemory = mock.bound.at(active.Handle()).first;
        { DeviceBuffer idle(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); }
        mock.limit = 4 * MiB;
        const auto before = mock.attempts;
        try {
            Buffer recovered(context, 600, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            Expect(mock.attempts == before + 2 && mock.frees == 1, "a new slab block did not retry after reclaiming an idle allocation");
            Expect(context.bufferPool->SlabBlocks() == 2 && mock.memoryBytes.contains(activeMemory), "slab allocation recovery discarded its live block");
        } catch (const std::exception& error) {
            Expect(false, std::string("slab allocation did not recover: ") + error.what());
        }
    }
    Expect(mock.liveBytes == 0 && mock.liveBuffers.empty(), "slab recovery leaked a resource");
}

void ReclaimSlabs() {
    mock = MockDevice{};
    {
        auto context = mockContext();
        Buffer active(context, 300, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        const auto activeMemory = mock.bound.at(active.Handle()).first;
        std::memset(active.Bytes().data(), 0x5a, active.Bytes().size());
        {
            Buffer sameBlock(context, 260, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            Buffer otherBlock(context, 600, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
        mock.limit = 6 * MiB;
        const auto before = mock.attempts;
        try {
            Buffer recovered(context, 4 * MiB, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            Expect(mock.attempts == before + 2 && mock.frees == 1, "reclaim did not free the unused slab block");
            Expect(context.bufferPool->SlabBlocks() == 1 && mock.memoryBytes.contains(activeMemory), "reclaim freed a slab with a live slot");
            Expect(std::all_of(active.Bytes().begin(), active.Bytes().end(), [](std::byte value) { return value == std::byte{0x5a}; }), "reclaim changed a live slab buffer");
        } catch (const std::exception& error) {
            Expect(false, std::string("empty slab did not permit recovery: ") + error.what());
        }
    }
    Expect(mock.liveBytes == 0 && mock.liveBuffers.empty(), "slab reclaim leaked a resource");
    mock = MockDevice{};
    {
        auto context = mockContext();
        BufferPool pool(context);
        const auto idle = pool.TakeSlot(context, 1, 512 * 1024, false);
        pool.PutSlot(idle.memory, idle.offset);
        mock.limit = 4 * MiB;
        const auto before = mock.attempts;
        try {
            const auto recovered = pool.TakeSlot(context, 1, 256 * 1024, false);
            Expect(mock.attempts == before + (VramCleanup() ? 1u : 2u) && mock.frees == 1 && pool.SlabBlocks() == 1, "an empty slab was neither released immediately nor reclaimed on demand");
            pool.PutSlot(recovered.memory, recovered.offset);
        } catch (const std::exception& error) {
            Expect(false, std::string("spare slab did not permit recovery: ") + error.what());
        }
    }
    Expect(mock.liveBytes == 0, "spare slab recovery leaked device memory");
}

void AllocationFailure(VkResult initial, VkResult final, bool cached) {
    mock = MockDevice{};
    {
        auto context = mockContext();
        if (cached) { DeviceBuffer idle(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); }
        mock.results = {initial, final};
        const auto before = mock.attempts;
        std::string failure;
        try {
            DeviceBuffer buffer(context, 4 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        } catch (const std::exception& error) {
            failure = error.what();
        }
        const bool reclaimed = cached && (initial == VK_ERROR_OUT_OF_DEVICE_MEMORY || initial == VK_ERROR_OUT_OF_HOST_MEMORY);
        Expect(mock.attempts == before + (reclaimed ? 2 : 1), "allocation retries were not bounded or a non-memory failure was retried");
        Expect(failure.find("Vulkan result " + std::to_string(reclaimed ? final : initial)) != std::string::npos, "allocation failure lost its final Vulkan result");
        Expect(mock.liveBuffers.size() == (cached && !reclaimed ? 1u : 0u), "a failed allocation retained its buffer");
        Expect(mock.liveBytes == (cached && !reclaimed ? 2 * MiB : 0), "a failed allocation mishandled cached memory");
    }
    Expect(mock.liveBytes == 0 && mock.liveBuffers.empty(), "failure cleanup leaked a resource");
}

void SuccessfulAllocationKeepsPool() {
    mock = MockDevice{};
    {
        auto context = mockContext();
        { DeviceBuffer idle(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); }
        DeviceBuffer buffer(context, 4 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Expect(mock.attempts == 2 && mock.frees == 0 && mock.liveBytes == 6 * MiB, "a successful first allocation reclaimed its cache");
    }
    Expect(mock.liveBytes == 0 && mock.liveBuffers.empty(), "normal allocation leaked a resource");
}

void PartialSlabDoesNotRetry() {
    mock = MockDevice{};
    {
        auto context = mockContext();
        Buffer active(context, 300, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        { Buffer idle(context, 260, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); }
        mock.limit = 2 * MiB;
        const auto before = mock.attempts;
        std::string failure;
        try {
            Buffer buffer(context, 2 * MiB, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        } catch (const std::exception& error) {
            failure = error.what();
        }
        Expect(failure.find("Vulkan result -2") != std::string::npos, "a live slab's memory was treated as reclaimable");
        Expect(mock.attempts == before + 1 && mock.frees == 0, "an allocation retried without freeing device memory");
        Expect(mock.liveBuffers.size() == 1 && mock.liveBuffers.contains(active.Handle()) && context.bufferPool->SlabBlocks() == 1, "reclaim changed a live slab allocation");
    }
    Expect(mock.liveBytes == 0 && mock.liveBuffers.empty(), "partial slab failure leaked a resource");
}

void CleanupBudgetPolicy() {
    VramReport report{0, 8 * 1024 * MiB, 8 * 1024 * MiB, 0, 7 * 1024 * MiB};
    report.usage = report.target - report.target / 10u - 1;
    Expect(VramBudget::PressureFor(report).victims == 0, "mirror collection started below its trigger");
    ++report.usage;
    auto pressure = VramBudget::PressureFor(report);
    Expect(pressure.age == 160 && pressure.victims == 4, "trigger pressure did not retain young mirrors or bound its pass");
    report.usage = report.target;
    pressure = VramBudget::PressureFor(report);
    Expect(pressure.age == 80 && pressure.victims == 8, "target pressure used the wrong mirror age or pass bound");
    report.usage = report.target + (report.budget - report.target) / 2;
    pressure = VramBudget::PressureFor(report);
    Expect(pressure.age == 16 && pressure.victims == 16, "critical pressure did not use the bounded oldest-mirror pass");
    report.usage = 6 * 1024 * MiB;
    Expect(VramBudget::RebarAllowance(report, 1024 * MiB, UINT64_MAX) == 2 * 1024 * MiB, "adaptive ReBAR allowance counted its own live bytes as other usage");
    Expect(VramBudget::RebarAllowance(report, 1024 * MiB, 256 * MiB) == 256 * MiB, "adaptive ReBAR allowance exceeded its configured ceiling");
    Expect(VramBudget::RebarAllowance(report, 1024 * MiB, 0) == 0, "zero ReBAR ceiling permitted an allocation");
    report.usage = 9 * 1024 * MiB;
    Expect(VramBudget::RebarAllowance(report, 1024 * MiB, UINT64_MAX) == 0, "adaptive ReBAR allowance underflowed under external pressure");
    mock = MockDevice{};
    mock.heapBudget = HeapBytes;
    mock.otherUsage = 1024 * MiB;
    auto context = budgetContext();
    auto& vram = GetBufferPool(context)->Vram();
    const auto first = vram.Current();
    vram.Released(64 * MiB);
    const auto released = vram.Current();
    Expect(first.sampleId != 0 && released.sampleId == first.sampleId && released.usage + 64 * MiB == first.usage, "released allocations were not deducted from the current VRAM sample");
    mock.otherUsage = 2 * 1024 * MiB;
    vram.Refresh();
    const auto next = vram.Current();
    Expect(next.sampleId > first.sampleId && next.usage == mock.otherUsage, "a new VRAM sample retained old release deductions");
    const auto fixed = RebarMemory().limit.load();
    const auto limit = RebarLimit(context);
    Expect(limit == (VramCleanup() ? HeapTarget - mock.otherUsage : fixed), "ReBAR allowance did not follow the cleanup switch");
    if (!VramCleanup()) return;
    Require(RebarMemory().Reserve(64 * MiB), "cannot reserve ReBAR bytes for sample-pinning test");
    Expect(RebarLimit(context) == limit, "allocations enlarged ReBAR allowance within one stale usage sample");
    mock.otherUsage += 64 * MiB;
    vram.Refresh();
    Expect(RebarLimit(context) == limit, "a fresh usage sample charged live ReBAR bytes twice");
    RebarMemory().Release(64 * MiB);
    mock.otherUsage = HeapTarget;
    vram.Refresh();
    Expect(RebarLimit(context) == (VramCleanup() ? 0 : fixed), "rising external pressure did not shrink the ReBAR allowance");
    mock.otherUsage = 1024 * MiB;
    vram.Refresh();
    Expect(RebarLimit(context) == (VramCleanup() ? HeapTarget - mock.otherUsage : fixed), "falling external pressure did not restore the ReBAR allowance");
    RebarMemory().limit.store(fixed);
}

void CleanupRetention() {
    mock = MockDevice{};
    {
        auto context = mockContext();
        const auto pool = GetBufferPool(context);
        const auto initial = pool->Stats();
        Expect(initial.smallLimit == (VramCleanup() ? 16u : 64u) * MiB, "small-buffer retention budget did not follow cleanup mode");
        Expect(initial.largeLimit == (VramCleanup() ? 128u : 512u) * MiB, "large-buffer retention budget did not follow cleanup mode");
        Expect(initial.deviceLimit == (VramCleanup() ? 256u : 512u) * MiB, "staging retention budget did not follow cleanup mode");
        std::vector<std::unique_ptr<Buffer>> small;
        for (int i = 0; i < 40; ++i) small.push_back(std::make_unique<Buffer>(context, 512 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        Expect(pool->Stats().slabBytes >= 20 * MiB, "pool statistics omitted physical slab bytes");
        small.clear();
        const auto smallStats = pool->Stats();
        Expect(smallStats.smallRetained <= initial.smallLimit, "small-buffer retention exceeded its cleanup ceiling");
        std::vector<std::unique_ptr<Buffer>> large;
        for (int i = 0; i < 3; ++i) large.push_back(std::make_unique<Buffer>(context, 64 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        large.clear();
        Expect(pool->Stats().largeRetained == (VramCleanup() ? 128u : 192u) * MiB, "large-buffer retention did not release memory at its cleanup ceiling");
        const auto before = mock.liveBytes;
        { Buffer mirror(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer::Access::ResidentMirror); }
        Expect(mock.liveBytes == before + (VramCleanup() ? 0 : 2 * MiB), "retired resident mirrors did not follow the cleanup pool bypass");
    }
    Expect(mock.liveBytes == 0 && mock.liveBuffers.empty(), "cleanup retention tests leaked allocations");
}

void CleanupPhysicalRetention() {
    mock = MockDevice{};
    {
        auto context = mockContext();
        const auto pool = GetBufferPool(context);
        std::vector<std::unique_ptr<Buffer>> live;
        for (const bool addressable : {false, true}) {
            const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (addressable ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0u);
            for (std::size_t bytes = 256; bytes <= 512 * 1024; bytes *= 2) live.push_back(std::make_unique<Buffer>(context, bytes, usage));
        }
        Expect(mock.liveBytes > 16 * MiB, "physical retention fixture did not exceed the cleanup cap");
        live.clear();
        const auto stats = pool->Stats();
        Expect(stats.smallRetained < 4 * MiB, "physical retention fixture did not leave sparsely occupied slabs");
        if (VramCleanup()) {
            Expect(stats.smallPhysicalRetained <= 16 * MiB && mock.liveBytes <= 16 * MiB, "small logical slots retained physical slabs beyond the cleanup cap");
            Expect(stats.smallPhysicalRetained == mock.liveBytes && stats.slabBytes == mock.liveBytes, "retention statistics did not report the actual idle slab allocations");
        } else Expect(mock.liveBytes > 16 * MiB, "cleanup-off mode unexpectedly applied the new physical slab cap");
    }
    Expect(mock.liveBytes == 0 && mock.liveBuffers.empty(), "physical retention tests leaked allocations");
}

void CleanupPressureAccounting() {
    mock = MockDevice{};
    mock.heapBudget = HeapBytes;
    mock.otherUsage = 1024 * MiB;
    {
        auto context = budgetContext();
        context.memory.memoryTypeCount = 3;
        context.memory.memoryTypes[2] = {HostVisible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0};
        const auto pool = GetBufferPool(context);
        { Buffer host(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); }
        auto device = std::make_unique<Buffer>(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, HostVisible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const auto type = mock.memoryTypes.at(MemoryOf(device->Handle()));
        Expect((context.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 && mock.liveBytes == 4 * MiB, "mixed retention fixture did not allocate separate host and device memory");
        mock.otherUsage = HeapTarget + 8 * MiB;
        pool->Vram().Refresh();
        const auto before = pool->Vram().Current();
        device.reset();
        const auto after = pool->Vram().Current();
        if (VramCleanup()) {
            Expect(mock.liveBytes == 0 && mock.frees == 2, "pressure did not release the mixed host and device cache tier");
            Expect(after.sampleId == before.sampleId && after.usage + 2 * MiB == before.usage, "host RAM eviction was incorrectly deducted from device-local VRAM usage");
        } else Expect(mock.liveBytes == 4 * MiB && after.usage == before.usage, "cleanup-off mode changed host-visible device memory retention");
    }
    Expect(mock.liveBytes == 0 && mock.liveBuffers.empty(), "mixed pressure accounting tests leaked allocations");
}

void CleanupStagingZero() {
    const auto* setting = std::getenv("APS5_STAGING_POOL_MIB");
    Require(setting != nullptr && std::strcmp(setting, "0") == 0, "zero staging fixture requires APS5_STAGING_POOL_MIB=0");
    mock = MockDevice{};
    {
        auto context = mockContext();
        const auto pool = GetBufferPool(context);
        VkBuffer small = VK_NULL_HANDLE;
        VkBuffer large = VK_NULL_HANDLE;
        {
            Buffer hostSmall(context, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            Buffer hostLarge(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            small = hostSmall.Handle();
            large = hostLarge.Handle();
        }
        const auto before = pool->Stats();
        const auto hostBytes = mock.liveBytes;
        {
            DeviceBuffer device(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            Expect(mock.liveBytes == hostBytes + 2 * MiB, "zero staging fixture did not make its device allocation");
        }
        const auto after = pool->Stats();
        Expect(after.deviceLimit == 0 && after.deviceRetained == 0, "zero staging budget retained allocations in the device tier");
        Expect(mock.liveBytes == hostBytes + (VramCleanup() ? 0 : 2 * MiB), "zero staging retention did not follow the cleanup switch");
        Expect(after.smallRetained == before.smallRetained && after.largeRetained == before.largeRetained + (VramCleanup() ? 0 : 2 * MiB), "zero staging budget changed host retention or moved device allocations to a host tier");
        Buffer reusedSmall(context, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Buffer reusedLarge(context, 2 * MiB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Expect(reusedSmall.Handle() == small && reusedLarge.Handle() == large, "zero staging retention discarded reusable host buffers");
    }
    Expect(mock.liveBytes == 0 && mock.liveBuffers.empty(), "zero staging retention tests leaked allocations");
}

void CleanupCopyLimits() {
    mock = MockDevice{};
    auto context = mockContext();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto before = VramCleanupCounters();
    const std::vector<std::byte> source(32 * MiB);
    GuestBufferMemory memory(context);
    memory.AddSnapshot({0x10000000, source});
    memory.AddSnapshot({0x20000000, source});
    bool refused = false;
    try { memory.AddSnapshot({0x30000000, std::span<const std::byte>(source).first(1)}); }
    catch (const std::runtime_error&) { refused = true; }
    Expect(refused == VramCleanup(), "snapshot copies did not enforce the per-build cleanup boundary");
    const auto after = VramCleanupCounters();
    Expect(!VramCleanup() || (after.snapshotBytes - before.snapshotBytes == 64 * MiB && after.snapshotRefusals == before.snapshotRefusals + 1), "snapshot cleanup counters did not report accepted and refused bytes");
    Expect(mock.allocations == 0, "snapshot-cap validation allocated GPU memory");
    const std::vector<std::byte> first(33 * MiB, std::byte{0x11});
    const std::vector<std::byte> second(33 * MiB, std::byte{0x22});
    GuestBufferMemory copied(context);
    copied.AddReadable(reinterpret_cast<std::uint64_t>(first.data()), first.size());
    copied.AddReadable(reinterpret_cast<std::uint64_t>(second.data()), second.size());
    const auto copiesBefore = VramCleanupCounters();
    refused = false;
    try { copied.Upload(false); }
    catch (const std::runtime_error& error) { refused = std::string(error.what()).find("per-build fallback copy limit") != std::string::npos; }
    Expect(refused == VramCleanup(), "CPU fallback copies did not enforce their aggregate per-build cleanup boundary");
    const auto copiesAfter = VramCleanupCounters();
    Expect(!VramCleanup() || (copiesAfter.copyBytes - copiesBefore.copyBytes == first.size() && copiesAfter.copyRefusals == copiesBefore.copyRefusals + 1 && mock.allocations == 1), "fallback copy limit allocated or copied the refused range");
    SetHostImportBudget(2 * 1024 * MiB);
    Expect(EffectiveHostImportBudget(context) == (VramCleanup() ? 1024u : 2048u) * MiB, "cleanup did not cap host imports at 1 GiB");
    SetHostImportBudget(128 * MiB);
    Expect(EffectiveHostImportBudget(context) == 128 * MiB, "cleanup enlarged a smaller configured host import budget");
    SetHostImportBudget(0);
}

}

void RebarEligibilityAndBudget() {
    auto context = mockContext();
    context.memory.memoryHeapCount = 1;
    context.memory.memoryHeaps[0].size = 256 * MiB;
    context.memory.memoryTypes[1].propertyFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    Expect(!context.RebarMemoryType(3), "a 256 MiB BAR was eligible");
    context.memory.memoryHeaps[0].size += 1;
    Expect(context.RebarMemoryType(3) == 1, "large coherent visible device heap was rejected");
    Expect(!context.RebarMemoryType(1), "memory type mask was ignored");
    context.memory.memoryTypes[1].propertyFlags &= ~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    Expect(!context.RebarMemoryType(3), "noncoherent device heap was eligible");
    RebarBudget budget(1024);
    Expect(budget.Reserve(768) && !budget.Reserve(257) && budget.Reserve(256), "ReBAR budget did not enforce the allocation boundary");
    Expect(!budget.Reserve(1) && budget.live == 1024 && budget.peak == 1024 && budget.allocations == 2 && budget.overBudget == 2, "ReBAR counters differ from allocations");
    budget.Release(768);
    Expect(budget.Reserve(768) && budget.live == 1024, "freed ReBAR budget was not reusable");
}

void RebarUploadReuse() {
    if (!RebarUploads()) return;
    mock = MockDevice{};
    {
        auto context = mockContext();
        context.limits.minStorageBufferOffsetAlignment = 256;
        context.limits.maxStorageBufferRange = 1u << 27;
        struct Cleanup { VkDevice device; ~Cleanup() { ClearSnapshotCopies(device); } } cleanup{context.device};
        context.memory.memoryHeapCount = 2;
        context.memory.memoryHeaps[0].size = 512 * MiB;
        context.memory.memoryHeaps[1].size = 512 * MiB;
        context.memory.memoryTypeCount = 3;
        context.memory.memoryTypes[1] = {HostVisible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
        context.memory.memoryTypes[2] = {HostVisible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1};
        Buffer ordinary(context, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Buffer upload(context, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer::Access::UploadOnly);
        const auto rebar = mock.memoryTypes.at(MemoryOf(upload.Handle()));
        Expect(rebar == (RebarMemory().limit == 0 ? 2u : 1u), "upload did not resolve to ReBAR or budget fallback");
        Expect(rebar != 1 || MemoryOf(ordinary.Handle()) != MemoryOf(upload.Handle()), "host and ReBAR allocations mixed");
        if (rebar == 1) {
            const auto allocations = RebarMemory().allocations.load();
            { Buffer another(context, 4000, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Access::UploadOnly);
              Expect(MemoryOf(another.Handle()) == MemoryOf(upload.Handle()), "ReBAR slots did not share their resolved slab"); }
            Expect(RebarMemory().allocations.load() == allocations && RebarMemory().live.load() == 2 * MiB, "ReBAR slab was charged per slot or released while pooled");
        }
        std::memset(upload.WriteBytes().data(), 0x55, upload.Size());
        bool rejected = false;
        try { upload.Bytes(); } catch (const std::runtime_error&) { rejected = true; }
        const auto* check = std::getenv("APS5_REBAR_CHECK");
        Expect(rejected == (check != nullptr && std::strcmp(check, "1") == 0), "upload-only CPU read did not follow the debug check switch");
        const auto missingBefore = RebarMemory().ineligible.load();
        {
            auto host = mockContext();
            Buffer fallback(host, 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Access::UploadOnly);
            Expect(mock.memoryTypes.at(MemoryOf(fallback.Handle())) == 1 && RebarMemory().ineligible.load() == missingBefore + 1, "missing ReBAR type did not select host fallback");
        }
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        const auto snapshot = [&](std::byte value) {
            auto memory = std::make_shared<GuestBufferMemory>(context);
            const std::array<std::byte, 16> data{value};
            memory->AddSnapshot({0x100000, data});
            memory->Upload(false);
            return memory;
        };
        const auto descriptor = [](const auto& memory) {
            std::uint32_t adjustment = 0;
            return memory->Descriptor(0x100000, 16, adjustment).buffer;
        };
        const auto first = snapshot(std::byte{0x11});
        Expect(descriptor(first) == descriptor(snapshot(std::byte{0x11})), "same snapshot bytes were not reused");
        const auto changed = snapshot(std::byte{0x22});
        Expect(descriptor(first) != descriptor(changed), "changed snapshot bytes reused the old upload");
        ClearSnapshotCopies(context.device);
    }
    Expect(RebarMemory().live == 0 && mock.liveBytes == 0, "ReBAR allocations stayed charged after the pool was freed");
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--staging-zero-only") {
            CleanupStagingZero();
            if (failures != 0) return 1;
            std::puts("zero staging retention tests passed");
            return 0;
        }
        RebarEligibilityAndBudget();
        RebarUploadReuse();
        CopyDiagnostics();
        SizeClassesAndDirections();
        LargerClass();
        KeptUntilFence();
        Budget();
        AddressAndHostUnchanged();
        SmallDeviceClasses();
        Slabs();
        for (const auto result : {VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_OUT_OF_HOST_MEMORY}) {
            AllocationRecovery(result, false);
            AllocationRecovery(result, true);
            AllocationFailure(result, result, true);
            AllocationFailure(result, result, false);
        }
        SlabAllocationRecovery();
        ReclaimSlabs();
        AllocationFailure(VK_ERROR_DEVICE_LOST, VK_ERROR_DEVICE_LOST, true);
        AllocationFailure(VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST, true);
        SuccessfulAllocationKeepsPool();
        PartialSlabDoesNotRetry();
        VramTargets();
        VramUnderTargetPools();
        VramOverTargetReleases();
        VramTrimsPool();
        VramIntegratedKeepsPool();
        CleanupBudgetPolicy();
        CleanupRetention();
        CleanupPhysicalRetention();
        CleanupPressureAccounting();
        CleanupCopyLimits();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    if (failures != 0) return 1;
    std::puts("buffer pool tests passed");
    return 0;
}
