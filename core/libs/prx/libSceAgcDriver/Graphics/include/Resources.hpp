#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCES_HPP

#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <span>

namespace AgcDriver::Graphics {

class Buffer {
public:
    enum class Access { Default, UploadOnly, ResidentMirror };
    Buffer(const Context& context, std::size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, Access access = Access::Default);
    Buffer(const Context& context, std::size_t size, VkBufferUsageFlags usage, Access access);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    VkBuffer Handle() const;
    VkDeviceAddress DeviceAddress() const;
    // Host-visible buffers only: a device-local one (properties without HOST_VISIBLE, the staging
    // shadows of GuestBufferMemory) has no mapping and its bytes move by GPU copies alone.
    std::span<std::byte> Bytes();
    std::span<std::byte> WriteBytes();
    std::span<const std::byte> ReadBytes(std::size_t offset, std::size_t bytes);
    std::size_t Size() const { return size; }
    bool UploadOnly() const { return uploadOnly; }
    bool Mapped() const { return mapping != nullptr; }
    bool DeviceLocal() const;
    void Invalidate();

private:
    void initializeAddress(VkBufferUsageFlags usage);
    void release() noexcept;
    Context context;
    VkDeviceAddress deviceAddress = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapping = nullptr;
    std::size_t size;
    std::size_t capacity;
    // Fully made (or taken from the pool), so release returns it to the pool instead of destroying
    // the handles a failed construction left.
    bool ready = false;
    VkDeviceSize allocationBytes = 0;
    VkDeviceSize offset = 0;
    bool slab = false;
    bool uploadOnly = false;
    bool residentMirror = false;
    std::uint32_t memoryType = UINT32_MAX;
    VkDeviceSize rebarBytes = 0;
    VkBufferUsageFlags usage;
    VkMemoryPropertyFlags properties;
    std::shared_ptr<BufferPool> cache;
};

// Device-local scratch memory for GPU-side layout conversion. The detiler reads and writes scattered
// elements, which crawls across PCIe, so guest bytes move between host and device buffers with DMA
// copies and are only swizzled in video memory.
class DeviceBuffer {
public:
    DeviceBuffer(const Context& context, std::size_t size, VkBufferUsageFlags usage);
    ~DeviceBuffer();
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    VkBuffer Handle() const;
    std::size_t Size() const;

private:
    void release() noexcept;
    Context context;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::size_t size;
    std::size_t capacity;
    VkDeviceSize allocationBytes = 0;
    VkBufferUsageFlags usage;
    std::shared_ptr<BufferPool> cache;
};

// Records a whole-range buffer copy.
void CopyBuffer(const Context& context, VkCommandBuffer commands, VkBuffer source, VkDeviceSize sourceOffset, VkBuffer destination, VkDeviceSize destinationOffset, VkDeviceSize bytes);
void CopyBuffers(const Context& context, VkCommandBuffer commands, VkBuffer source, VkBuffer destination, std::uint32_t count, const VkBufferCopy* regions);
void CopyBufferToImage(const Context& context, VkCommandBuffer commands, VkBuffer source, VkImage destination, VkImageLayout layout, std::uint32_t count, const VkBufferImageCopy* regions);
void CopyImageToBuffer(const Context& context, VkCommandBuffer commands, VkImage source, VkImageLayout layout, VkBuffer destination, std::uint32_t count, const VkBufferImageCopy* regions);
void CopyImage(const Context& context, VkCommandBuffer commands, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, std::uint32_t count, const VkImageCopy* regions);
void BlitImage(const Context& context, VkCommandBuffer commands, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, std::uint32_t count, const VkImageBlit* regions, VkFilter filter);
void CopyQueryResults(const Context& context, VkCommandBuffer commands, VkQueryPool pool, std::uint32_t first, std::uint32_t count, VkBuffer destination, VkDeviceSize offset, VkDeviceSize stride, VkQueryResultFlags flags);

// Records a global memory barrier.
void RecordMemoryBarrier(const Context& context, VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess);

// Resolves every DeviceFunctions entry point of the context's device (once, at device setup).
void FillDeviceFunctions(const Context& context, DeviceFunctions& functions);

class RenderTarget {
public:
    RenderTarget(const Context& context, const ColorTarget& target, bool blending);
    ~RenderTarget();
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;
    VkImage Image() const;
    VkImageView View() const;

private:
    void release() noexcept;
    Context context;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

class CommandBatch {
public:
    enum class Mode { Default, Independent };
    explicit CommandBatch(const Context& context, Mode mode = Mode::Default);
    ~CommandBatch();
    CommandBatch(const CommandBatch&) = delete;
    CommandBatch& operator=(const CommandBatch&) = delete;
    VkCommandBuffer Handle() const;
    void SubmitAndWait();
    void Submit();
    void Wait();
    std::shared_ptr<const bool> TrackSubmission();

private:
    void release() noexcept;
    Context context;
    Mode mode;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    bool submitted = false;
    std::shared_ptr<bool> submissionState;
};

}

#endif
