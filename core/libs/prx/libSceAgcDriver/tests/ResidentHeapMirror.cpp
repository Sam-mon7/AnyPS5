#include "BdaShader.hpp"
#include "BdaAbi.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BdaResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/KytyBufferCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RebarBudget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/src/Driver/Dispatch/PreparedHeapRefresh.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace AgcDriver::Graphics;
class Pipeline {
public:
    Pipeline(const Context& context, std::span<const std::uint32_t> code, const std::array<Buffer*, 3>& buffers)
        : Pipeline(context, code, std::array<VkDescriptorBufferInfo, 3>{{{buffers[0]->Handle(), 0, buffers[0]->Size()}, {buffers[1]->Handle(), 0, buffers[1]->Size()}, {buffers[2]->Handle(), 0, buffers[2]->Size()}}}) {}

    Pipeline(const Context& context, std::span<const std::uint32_t> code, const std::array<VkDescriptorBufferInfo, 3>& buffers) : context(context) {
        try {
            std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
            for (std::uint32_t i = 0; i < bindings.size(); ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            setInfo.bindingCount = bindings.size();
            setInfo.pBindings = bindings.data();
            Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &setInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &size;
            Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
            VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1, &setLayout};
            Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocation, &set), "vkAllocateDescriptorSets");
            for (std::uint32_t i = 0; i < buffers.size(); ++i) {
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = set;
                write.dstBinding = i;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &buffers[i];
                context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, 1, &write, 0, nullptr);
            }
            VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &setLayout;
            Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
            VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            moduleInfo.codeSize = code.size_bytes();
            moduleInfo.pCode = code.data();
            Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
            pipelineInfo.layout = layout;
            Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines");
        } catch (...) { release(); throw; }
    }

    ~Pipeline() { release(); }

    void Record(Recorder& recorder) {
        const auto commands = recorder.Commands();
        VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &upload, 0, nullptr, 0, nullptr);
        context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, 1, 1, 1);
        VkMemoryBarrier download{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &download, 0, nullptr, 0, nullptr);

    }

private:
    void release() noexcept {
        if (pipeline) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
        if (module) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        if (layout) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, layout, nullptr);
        if (pool) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
        if (setLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, setLayout, nullptr);
    }

    Context context;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

VkDeviceSize cleanupUsage = 1024ull << 20u;

VKAPI_ATTR void VKAPI_CALL cleanupMemoryProperties2(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2* properties) {
    for (auto* next = static_cast<VkBaseOutStructure*>(properties->pNext); next != nullptr; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) continue;
        auto& budget = *reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(next);
        for (std::size_t heap = 0; heap < VK_MAX_MEMORY_HEAPS; ++heap) {
            budget.heapBudget[heap] = 8ull << 30u;
            budget.heapUsage[heap] = cleanupUsage;
        }
    }
}
}

void RunVramCleanupTests(const AgcDriver::Graphics::Context& device, AgcDriver::Graphics::Recorder& recorder) {
#ifndef _WIN32
    throw std::runtime_error("VRAM cleanup fixture requires Windows guest page protection");
#else
    using namespace AgcDriver::Graphics;
    namespace Memory = AgcDriver::GuestMemory;
    Require(VramWritable() && VramOption("APS5_VRAM_DEFERRED"), "VRAM cleanup fixture requires writable deferred mirrors");
    recorder.Sync();
    recorder.FlushDeferredReleases();
    ClearImageMirrors(device.device);
    ClearHostImports(device.device);
    auto context = device;
    context.hostImportAlignment = 0;
    context.dmaBufImport = false;
    context.integratedGpu = false;
    context.memoryProperties2 = cleanupMemoryProperties2;
    context.bufferPool.reset();
    cleanupUsage = 1024ull << 20u;
    constexpr std::size_t block = 65536;
    auto* raw = GuestArena::GuestArenaAllocate_nid_postfix(block, block);
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, block, nullptr);
    Require(section != nullptr, "cannot create shared cleanup fixture memory");
    GuestArena::GuestArenaMap_nid_postfix(raw, block, section, 0, PAGE_READWRITE);
    std::memset(raw, 0x11, block);
    const auto address = reinterpret_cast<std::uint64_t>(raw);
    GuestAllocations::Mutation().Add(raw, block, true, true, true);
    const auto acquire = [&](bool written) {
        Memory::BumpCollectEpoch();
        auto memory = std::make_shared<GuestBufferMemory>(context);
        memory->SetAddressWrites(written);
        memory->AcquireRegistered();
        if (written) memory->AddWritable(address, sizeof(std::uint32_t));
        memory->Upload(true);
        Require(HostImportFor(context, address, block) == nullptr, "cleanup fixture unexpectedly imported its shared heap");
        return memory;
    };
    auto held = acquire(false);
    recorder.Sync();
    recorder.FlushDeferredReleases();
    {
        const auto copied = VramCleanupCounters().snapshotBytes;
        GuestBufferMemory snapshot(context);
        snapshot.AddSnapshot({address, std::span(static_cast<const std::byte*>(raw), 16)});
        snapshot.Upload(false);
        std::uint32_t snapshotAdjustment = 0;
        const auto view = snapshot.Descriptor(address, 16, snapshotAdjustment);
        std::uint32_t mirrorAdjustment = 0;
        const auto backing = held->Descriptor(address, 16, mirrorAdjustment);
        Require(view.buffer == backing.buffer, "a standalone snapshot changed its registered range's mirror backing");
        Require(!VramCleanup() || (snapshot.CopiedBytes() == 0 && VramCleanupCounters().snapshotBytes == copied), "a snapshot copied bytes already served by a mirror");
    }
    const auto first = VramCleanupCounters();
    Require(first.heapShadowBytes == 0, "writable heap mirrors retained whole-range CPU shadows");
    cleanupUsage = 8ull << 30u;
    GetBufferPool(context)->Vram().Refresh();
    const auto cached = AddressSpaceCounters();
    Require(cached.enabled, "cleanup cache retention fixture requires the address-space cache");
    Recorder::CountPresent();
    CollectVramMirrors(context);
    { auto recent = acquire(false); }
    const auto reused = AddressSpaceCounters();
    Require(reused.hits == cached.hits + 1 && reused.rebuiltFirst == cached.rebuiltFirst, "pressure discarded the cached address space when every mirror was too recent to evict");
    cleanupUsage = 1024ull << 20u;
    GetBufferPool(context)->Vram().Refresh();
    for (int i = 0; i < 160; ++i) Recorder::CountPresent();
    CollectVramMirrors(context);
    Require(VramCleanupCounters().evictedMirrors == first.evictedMirrors, "an unpressured cache evicted a mirror");
    cleanupUsage = 8ull << 30u;
    GetBufferPool(context)->Vram().Refresh();
    Recorder::CountPresent();
    CollectVramMirrors(context);
    Require(VramCleanupCounters().evictedMirrors == first.evictedMirrors, "collection retired a mirror held by a prepared build");
    held.reset();
    CollectVramMirrors(context);
    Require(VramCleanupCounters().evictedMirrors == first.evictedMirrors, "collection ran twice in one present");
    Recorder::CountPresent();
    CollectVramMirrors(context);
    const auto retired = VramCleanupCounters();
    Require(retired.evictedMirrors == first.evictedMirrors + (VramCleanup() ? 1 : 0), "old released mirror eviction did not follow the cleanup switch");
    Require(retired.evictedBytes == first.evictedBytes + (VramCleanup() ? block : 0), "mirror eviction reported the wrong released byte count");
    cleanupUsage = 1024ull << 20u;
    GetBufferPool(context)->Vram().Refresh();
    const auto rebuilt = MirrorCounters().rebuilds;
    auto writer = acquire(true);
    Require(MirrorCounters().rebuilds == rebuilt + (VramCleanup() ? 1 : 0), "using an evicted mirror did not recreate its backing");
    std::uint32_t adjustment = 0;
    const auto destination = writer->Descriptor(address, sizeof(std::uint32_t), adjustment);
    writer->BeginGpuWrites(recorder);
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, destination.buffer, destination.offset + adjustment, sizeof(std::uint32_t), 0x12345678u);
    writer->RecordCopyBacks(recorder);
    recorder.Keep(writer);
    recorder.OnComplete([writer] { writer->WriteBack(); });
    writer->ReadyForSubmit(recorder);
    static_cast<volatile std::uint8_t*>(raw)[8] = 0x55;
    recorder.Sync();
    writer.reset();
    recorder.FlushDeferredReleases();
    Require(VramWritableCounters().pendingPages != 0 && GuestArena::GuestArenaHasGpuOverlay_nid_no_patch(address, block), "deferred write results were not pending before age collection");
    const auto pending = VramCleanupCounters();
    cleanupUsage = 8ull << 30u;
    GetBufferPool(context)->Vram().Refresh();
    for (int i = 0; i < 15; ++i) Recorder::CountPresent();
    CollectVramMirrors(context);
    Require(VramCleanupCounters().evictedMirrors == pending.evictedMirrors, "critical pressure evicted a mirror before its minimum age");
    Recorder::CountPresent();
    CollectVramMirrors(context);
    Require(VramCleanupCounters().evictedMirrors == pending.evictedMirrors + (VramCleanup() ? 1 : 0), "critical age collection did not retire a pending mirror");
    if (VramCleanup()) Require(!GuestArena::GuestArenaHasGpuOverlay_nid_no_patch(address, block), "retirement did not publish pending GPU pages before releasing their mirror");
    Require(*static_cast<volatile std::uint32_t*>(raw) == 0x12345678u && static_cast<volatile std::uint8_t*>(raw)[8] == 0x55, "age retirement lost GPU bytes or disjoint CPU stores");
    cleanupUsage = 1024ull << 20u;
    GetBufferPool(context)->Vram().Refresh();
    auto reader = acquire(false);
    auto readback = std::make_shared<Buffer>(context, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto source = reader->Descriptor(address, 16, adjustment);
    const auto copyCommands = recorder.Commands();
    RecordMemoryBarrier(context, copyCommands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    CopyBuffer(context, copyCommands, source.buffer, source.offset + adjustment, readback->Handle(), 0, 16);
    RecordMemoryBarrier(context, copyCommands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder.Keep(reader);
    recorder.Keep(readback);
    recorder.Sync();
    Require(*reinterpret_cast<const std::uint32_t*>(readback->Bytes().data()) == 0x12345678u && readback->Bytes()[8] == std::byte{0x55}, "recreated mirror did not upload the published merged bytes");
    reader.reset();
    recorder.FlushDeferredReleases();
    GuestAllocations::Mutation().Remove(raw);
    ClearImageMirrors(context.device);
    ClearHostImports(context.device);
    GuestArena::GuestArenaReset_nid_postfix(raw, block);
    GuestArena::GuestArenaRelease_nid_postfix(raw, block);
    CloseHandle(section);
    std::cout << "VRAM cleanup pressure, age, held-resource, pending-publication and recreation tests passed\n";
#endif
}

void RunResidentHeapMirrorTests(const AgcDriver::Graphics::Context& device, AgcDriver::Graphics::Recorder& recorder, bool sparse, std::unique_lock<AgcDriver::GuestMemory::GpuMutexType>* gpu) {
#ifndef _WIN32
    throw std::runtime_error("partially committed resident heap fixture is not implemented on this platform");
#else
    using namespace AgcDriver::Graphics;
    namespace Memory = AgcDriver::GuestMemory;
    namespace Abi = ShaderRecompiler::BdaAbi;
    Require(GuestArena::GuestArenaAvailable_nid_postfix() && Memory::WriteWatched(), "resident heap fixture requires the watched guest arena");
    auto context = device;
    context.hostImportAlignment = 0;
    TextureDetiler detiler(context);
    struct Drain {
        Recorder& recorder;
        ~Drain() { recorder.Sync(); recorder.FlushDeferredReleases(); }
    } drain{recorder};
    constexpr std::size_t block = 65536;
    void* raw = GuestArena::GuestArenaAllocate_nid_postfix(2 * block, block);
    HANDLE section = nullptr;
    if (VramWritable()) {
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, 2 * block, nullptr);
        Require(section != nullptr, "cannot create shared resident fixture memory");
        GuestArena::GuestArenaMap_nid_postfix(raw, sparse ? block : 2 * block, section, 0, PAGE_READWRITE);
    } else GuestArena::GuestArenaCommit_nid_postfix(raw, sparse ? block : 2 * block, PAGE_READWRITE, block);
    const auto address = reinterpret_cast<std::uint64_t>(raw);
    std::memset(raw, 0x11, block);
    Require(Memory::Accessible(raw, 2 * block) != sparse, "resident heap fixture commitment differs");
    const bool resident = ResidentHeapMirrors() && !sparse;
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(raw, 2 * block, true, true, true);
    }
    const std::array<std::uint32_t, 2> values{0x12345678u, 0xabcdef09u};
    const auto rebarBefore = RebarMemory().allocations.load();
    const auto fallbackBefore = RebarMemory().overBudget.load();
    const auto liveBefore = RebarMemory().live.load();
    if (VramWritable() && !VramDeviceLocalMirrors() && RebarMemory().limit == 0) {
        const auto submissions = recorder.Submissions();
        bool refused = false;
        try {
            GuestBufferMemory memory(context);
            memory.AcquireRegistered();
            memory.Upload(true);
        } catch (const std::runtime_error& error) {
            refused = std::string(error.what()).find("shared protection or a coherent host import") != std::string::npos;
        }
        Require(refused && recorder.Submissions() == submissions, "an unsupported writable heap silently used an incoherent fallback");
        GuestAllocations::Mutation().Remove(raw);
        ClearImageMirrors(context.device);
        ClearHostImports(context.device);
        GuestArena::GuestArenaReset_nid_postfix(raw, 2 * block);
        GuestArena::GuestArenaRelease_nid_postfix(raw, 2 * block);
        CloseHandle(section);
        std::cout << "Unsupported writable heap fallback refused before recording\n";
        return;
    }
    VkDeviceAddress serving = 0;
    const auto prepare = [&] {
        if (gpu == nullptr) return std::shared_ptr<PreparedHeapRefresh>{};
        auto prepared = CaptureDispatchHeapRefresh(context, [&](std::uint64_t begin, std::size_t bytes) {
            return recorder.PendingWriteOverlaps(begin, bytes) || recorder.PendingLabelIn(begin, bytes);
        });
        if (prepared != nullptr) {
            gpu->unlock();
            try {
                StageDispatchHeapRefresh(*prepared);
            } catch (...) {
                gpu->lock();
                throw;
            }
            gpu->lock();
        }
        return prepared;
    };
    const auto recordDispatch = [&](bool written, const std::function<void()>& beforeWriteBack, const std::shared_ptr<PreparedHeapRefresh>& prepared) {
        Memory::BumpCollectEpoch();
        auto memory = std::make_shared<GuestBufferMemory>(context);
        memory->SetAddressWrites(written);
        {
            const DispatchHeapRefreshScope refresh(prepared.get());
            memory->AcquireRegistered();
        }
        if (written) memory->AddWritable(address, 16);
        memory->Upload(true);
        const auto ranges = memory->AddressRanges();
        const auto found = std::find_if(ranges.begin(), ranges.end(), [&](const auto& range) { return range.begin == address; });
        Require(found != ranges.end(), "resident heap is absent from the address table");
        if (resident && serving != 0) Require(found->deviceAddress == serving, "GPU consumer replaced its resident mirror");
        serving = found->deviceAddress;
        const Abi::Header header{Abi::Version, static_cast<std::uint32_t>(ranges.size()), sizeof(Abi::Range), 0};
        auto table = std::make_shared<Buffer>(context, sizeof(header) + ranges.size() * sizeof(Abi::Range), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        auto fault = std::make_shared<Buffer>(context, Abi::FaultBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        auto output = std::make_shared<Buffer>(context, 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(table->Bytes().data(), &header, sizeof(header));
        std::memcpy(table->Bytes().data() + sizeof(header), ranges.data(), ranges.size() * sizeof(Abi::Range));
        std::memset(fault->Bytes().data(), 0, fault->Bytes().size());
        const auto code = written ? MakeBdaDwordWriteTestShader(address, 2, values.data()) : MakeBdaDwordReadTestShader(address, 2, false, true);
        auto pipeline = std::make_shared<Pipeline>(context, code, std::array<Buffer*, 3>{table.get(), fault.get(), output.get()});
        memory->BeginGpuWrites(recorder);
        pipeline->Record(recorder);
        if (VramWritable()) memory->RecordCopyBacks(recorder);
        recorder.Keep(table);
        recorder.Keep(fault);
        recorder.Keep(output);
        recorder.Keep(pipeline);
        recorder.Keep(memory);
        recorder.NotePendingWrites(memory->Writes());
        CountLeaseOutcome(false, recorder.Submissions() + 1);
        if (VramWritable()) {
            if (beforeWriteBack) beforeWriteBack();
            recorder.OnComplete([memory] { memory->WriteBack(); });
            memory->ReadyForSubmit(recorder);
            if (beforeWriteBack) recorder.Sync();
        } else if (beforeWriteBack) {
            recorder.Sync();
            beforeWriteBack();
            memory->WriteBack();
        } else recorder.OnComplete([memory] { memory->WriteBack(); });
        return output;
    };
    const auto dispatch = [&](bool written, const std::function<void()>& beforeWriteBack = {}, std::shared_ptr<PreparedHeapRefresh> prepared = {}) {
        if (prepared == nullptr) prepared = prepare();
        return recordDispatch(written, beforeWriteBack, prepared);
    };
    if (gpu != nullptr && ResidentHeapMirrors()) Require(prepare() == nullptr, "resident heap mirrors admitted dispatch staging");
    if (gpu != nullptr && !sparse && !ResidentHeapMirrors()) {
        const auto enabled = DispatchLockDietEnabled();
        const auto initial = DispatchHeapRefreshCounters();
        const auto checkRead = [&](const std::shared_ptr<Buffer>& output, std::uint32_t expected) {
            recorder.Sync();
            std::uint32_t actual = 0;
            std::memcpy(&actual, output->Bytes().data(), sizeof(actual));
            Require(actual == expected, "staged heap refresh exposed stale bytes to a GPU consumer");
        };
        checkRead(dispatch(false), 0x11111111u);
        auto* const words = static_cast<volatile std::uint32_t*>(raw);
        const auto capture = [&] {
            auto prepared = prepare();
            Require((prepared != nullptr) == enabled, "dispatch heap staging did not follow its exact-value switch");
            return prepared;
        };
        const auto checkFallback = [&](const std::shared_ptr<PreparedHeapRefresh>& prepared, std::uint32_t expected) {
            const auto before = DispatchHeapRefreshCounters();
            checkRead(dispatch(false, {}, prepared), expected);
            Require(DispatchHeapRefreshCounters().fallback == before.fallback + (enabled ? 1 : 0), "a stale dispatch heap plan did not fall back exactly once");
        };
        const auto wait = [](const auto& ready) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!ready()) {
                Require(std::chrono::steady_clock::now() < deadline, "dispatch heap fixture timed out");
                std::this_thread::yield();
            }
        };
        words[0] = 0x10203040u;
        auto prepared = capture();
        const auto beforeApply = DispatchHeapRefreshCounters();
        checkRead(dispatch(false, {}, prepared), words[0]);
        Require(DispatchHeapRefreshCounters().applied == beforeApply.applied + (enabled ? 1 : 0), "a stable dispatch heap plan did not apply");
        words[0] = 0x20304050u;
        prepared = capture();
        words[0] = 0x30405060u;
        checkFallback(prepared, words[0]);
        words[0] = 0x40506070u;
        prepared = capture();
        auto second = capture();
        checkRead(dispatch(false, {}, prepared), words[0]);
        checkFallback(second, words[0]);
        prepared.reset();
        second.reset();
        words[0] = 0x41516171u;
        prepared = CaptureDispatchHeapRefresh(context, [](std::uint64_t, std::size_t) { return false; });
        bool mutated = false;
        gpu->unlock();
        try {
            if (prepared != nullptr) StageDispatchHeapRefresh(*prepared, [&](std::uint64_t begin, std::size_t bytes) {
                Require(begin <= address && address < begin + bytes, "during-copy fixture staged the wrong block");
                words[0] = 0x42526272u;
                mutated = true;
            });
            else words[0] = 0x42526272u;
        } catch (...) { gpu->lock(); throw; }
        gpu->lock();
        Require(mutated == enabled, "during-copy mutation hook did not follow the switch");
        checkFallback(prepared, words[0]);
        prepared.reset();
        words[0] = 0x43536373u;
        prepared = CaptureDispatchHeapRefresh(context, [](std::uint64_t, std::size_t) { return false; });
        std::atomic<bool> copying{false}, refreshed{false};
        std::exception_ptr queueError, stageError;
        gpu->unlock();
        {
            std::jthread queue([&] {
                try {
                    Memory::TagGpuLockThread(1);
                    wait([&] { return copying.load(std::memory_order_acquire); });
                    auto other = PrepareDispatchHeapRefresh(context, [](std::uint64_t, std::size_t) { return false; }, {});
                    std::lock_guard lock(Memory::GpuMutex());
                    const auto output = recordDispatch(false, {}, other);
                    recorder.Sync();
                    std::uint32_t actual = 0;
                    std::memcpy(&actual, output->Bytes().data(), sizeof(actual));
                    Require(actual == 0x43536373u, "the competing queue consumed stale heap bytes");
                } catch (...) { queueError = std::current_exception(); }
                refreshed.store(true, std::memory_order_release);
            });
            try {
                if (prepared != nullptr) StageDispatchHeapRefresh(*prepared, [&](std::uint64_t, std::size_t) {
                    copying.store(true, std::memory_order_release);
                    wait([&] { return refreshed.load(std::memory_order_acquire); });
                });
                else copying.store(true, std::memory_order_release);
                wait([&] { return refreshed.load(std::memory_order_acquire); });
            } catch (...) { copying.store(true, std::memory_order_release); stageError = std::current_exception(); }
        }
        gpu->lock();
        if (stageError) std::rethrow_exception(stageError);
        if (queueError) std::rethrow_exception(queueError);
        checkFallback(prepared, words[0]);
        prepared.reset();
        const auto pending = [&](std::uint64_t begin, std::size_t bytes) {
            return recorder.PendingWriteOverlaps(begin, bytes) || recorder.PendingLabelIn(begin, bytes);
        };
        const auto synchronize = [&](std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
            Require(!Memory::GpuMutex().HeldByThisThread() && !ranges.empty(), "heap pre-sync held the GPU mutex");
            if (!recorder.HasTimeline()) return false;
            std::uint64_t serial = 0;
            {
                std::lock_guard lock(Memory::GpuMutex());
                for (const auto& [begin, bytes] : ranges) {
                    const auto info = recorder.DescribePendingWrite(begin, bytes);
                    if (info) serial = std::max(serial, info->serial);
                }
                if (serial != 0) recorder.Submit();
            }
            if (serial == 0) return false;
            recorder.WaitSerial(serial);
            std::lock_guard lock(Memory::GpuMutex());
            recorder.FinishUpTo(serial);
            return true;
        };
        recordDispatch(true, {}, {});
        const auto beforePresync = DispatchHeapRefreshCounters();
        gpu->unlock();
        try { prepared = PrepareDispatchHeapRefresh(context, pending, synchronize); }
        catch (...) { gpu->lock(); throw; }
        gpu->lock();
        const bool presynced = enabled && recorder.HasTimeline();
        Require((prepared != nullptr) == presynced, "pending-writer heap staging admission differs");
        Require(DispatchHeapRefreshCounters().presyncs == beforePresync.presyncs + (presynced ? 1 : 0), "pending heap writer did not pre-sync exactly once");
        checkRead(dispatch(false, {}, prepared), values[0]);
        prepared.reset();
        recordDispatch(true, {}, {});
        const auto beforeRestart = DispatchHeapRefreshCounters();
        unsigned rounds = 0;
        const std::uint32_t restarted = 0x456789abu;
        gpu->unlock();
        try {
            prepared = PrepareDispatchHeapRefresh(context, pending, [&](auto ranges) {
                if (!synchronize(ranges)) return false;
                if (++rounds == 1) {
                    std::lock_guard lock(Memory::GpuMutex());
                    static_cast<void>(recorder.Commands());
                    recorder.AfterCompletions(address, std::as_bytes(std::span(&restarted, 1)), 992, 1, false);
                }
                return true;
            });
        } catch (...) { gpu->lock(); throw; }
        gpu->lock();
        Require(rounds == (presynced ? 2u : 0u), "a writer arriving during pre-sync did not restart heap capture");
        Require(DispatchHeapRefreshCounters().presyncs == beforeRestart.presyncs + rounds, "restarted heap pre-sync rounds were not counted");
        checkRead(dispatch(false, {}, prepared), presynced ? restarted : values[0]);
        prepared.reset();
        const auto beforeRetries = DispatchHeapRefreshCounters();
        unsigned retries = 0;
        gpu->unlock();
        try {
            prepared = PrepareDispatchHeapRefresh(context, [](std::uint64_t, std::size_t) { return true; }, [&](auto) {
                ++retries;
                return true;
            });
        } catch (...) { gpu->lock(); throw; }
        gpu->lock();
        Require(prepared == nullptr && retries == (enabled ? 2u : 0u), "new heap writers did not take the bounded fallback");
        Require(DispatchHeapRefreshCounters().presyncFallbacks == beforeRetries.presyncFallbacks + (enabled ? 1 : 0), "bounded heap pre-sync fallback was not counted");
        prepared = capture();
        dispatch(true);
        checkFallback(prepared, values[0]);
        prepared = capture();
        const std::uint32_t label = 0x50607080u;
        static_cast<void>(recorder.Commands());
        recorder.AfterCompletions(address, std::as_bytes(std::span(&label, 1)), 991, 0, false);
        checkFallback(prepared, label);
        prepared = capture();
        alignas(64) static std::array<std::byte, 64> metadata{};
        GuestAllocations::Mutation().Add(metadata.data(), metadata.size(), true, true, false);
        checkFallback(prepared, label);
        GuestAllocations::Mutation().Remove(metadata.data());
        checkRead(dispatch(false), label);
        prepared = capture();
        Require(HostImportsIdentity(context).device == VK_NULL_HANDLE, "dispatch heap fixture unexpectedly has host imports");
        const auto beforeImportClear = DispatchHeapRefreshCounters();
        ClearHostImports(context.device);
        checkRead(dispatch(false, {}, prepared), label);
        const auto afterImportClear = DispatchHeapRefreshCounters();
        Require(afterImportClear.applied == beforeImportClear.applied + (enabled ? 1 : 0) &&
            afterImportClear.fallback == beforeImportClear.fallback, "clearing absent imports invalidated a current dispatch heap plan");
        prepared = capture();
        ClearImageMirrors(context.device);
        checkFallback(prepared, label);
        prepared.reset();
        second.reset();
        for (const bool remap : {false, true}) {
            checkRead(dispatch(false), label);
            prepared = capture();
            recorder.FlushDeferredReleases();
            std::atomic<bool> applied{false}, finished{false};
            std::exception_ptr mutationError, waitError;
            gpu->unlock();
            {
                std::jthread mutation([&] {
                    try {
                        if (remap) {
                            {
                                GuestAllocations::Mutation change;
                                change.Unmap(raw, 2 * block, [&](const void*, std::size_t, const void*, bool) {
                                    GuestArena::GuestArenaReset_nid_postfix(raw, 2 * block);
                                    applied.store(true, std::memory_order_release);
                                });
                            }
                            GuestArena::GuestArenaCommit_nid_postfix(raw, 2 * block, PAGE_READWRITE, block);
                            words[0] = label;
                            GuestAllocations::Mutation().Add(raw, 2 * block, true, true, true);
                        } else {
                            GuestAllocations::Mutation().Protect(raw, 2 * block, true, false, true, [&] {
                                for (std::size_t offset = 0; offset < 2 * block; offset += block) {
                                    DWORD previous = 0;
                                    Require(VirtualProtect(static_cast<std::byte*>(raw) + offset, block, PAGE_READONLY, &previous) != 0, "cannot protect the staged heap");
                                }
                                applied.store(true, std::memory_order_release);
                            });
                        }
                    } catch (...) { mutationError = std::current_exception(); }
                    finished.store(true, std::memory_order_release);
                });
                try {
                    if (enabled) {
                        wait([&] {
                            const auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
                            return std::none_of(lease.begin(), lease.end(), [&](const auto& range) { return range->address == address; });
                        });
                        Require(!applied.load(std::memory_order_acquire), "mapping mutation passed a staged heap lease");
                    }
                    gpu->lock();
                    prepared.reset();
                    gpu->unlock();
                    wait([&] { return finished.load(std::memory_order_acquire); });
                } catch (...) {
                    waitError = std::current_exception();
                    std::lock_guard lock(Memory::GpuMutex());
                    prepared.reset();
                }
            }
            gpu->lock();
            if (waitError) std::rethrow_exception(waitError);
            if (mutationError) std::rethrow_exception(mutationError);
            Require(applied.load(std::memory_order_acquire), "mapping mutation never applied");
            if (!remap) GuestAllocations::Mutation().Protect(raw, 2 * block, true, true, true, [&] {
                for (std::size_t offset = 0; offset < 2 * block; offset += block) {
                    DWORD previous = 0;
                    Require(VirtualProtect(static_cast<std::byte*>(raw) + offset, block, PAGE_READWRITE, &previous) != 0, "cannot restore the staged heap protection");
                }
            });
            checkRead(dispatch(false), label);
        }
        const auto after = DispatchHeapRefreshCounters();
        Require(enabled ? after.stagedBytes > initial.stagedBytes :
            after.captured == initial.captured && after.unavailable == initial.unavailable && after.stagedBytes == initial.stagedBytes &&
            after.stageUs == initial.stageUs && after.applied == initial.applied && after.fallback == initial.fallback &&
            after.presyncs == initial.presyncs && after.presyncFallbacks == initial.presyncFallbacks,
            "dispatch heap staging counters did not follow the switch");
        std::memset(raw, 0x11, block);
    }
    dispatch(true, [&] { static_cast<volatile std::uint8_t*>(raw)[8] = 0x55; });
    const auto* vramValue = std::getenv("APS5_VRAM_MIRRORS");
    const bool vramMirrors = vramValue != nullptr && std::strcmp(vramValue, "1") == 0;
    if (VramDeviceLocalMirrors()) {
        Require(RebarMemory().allocations.load() == rebarBefore, "device-local heap mirror allocated ReBAR memory");
    } else if (resident && RebarMirrors()) {
        if (context.RebarMemoryType(UINT32_MAX) && RebarMemory().limit >= liveBefore + 2 * block) Require(RebarMemory().live.load() >= liveBefore + 2 * block, "resident heap was not allocated in budgeted ReBAR memory");
        if (RebarMemory().limit == 0) Require(RebarMemory().overBudget.load() > fallbackBefore, "resident heap did not fall back when the ReBAR budget was full");
    } else if (vramMirrors && (!sparse || VramWritable())) {
        if (context.RebarMemoryType(UINT32_MAX) && RebarMemory().limit >= liveBefore + 2 * block) Require(RebarMemory().allocations.load() > rebarBefore, "a heap mirror was not placed in ReBAR memory with APS5_VRAM_MIRRORS");
        if (RebarMemory().limit == 0) Require(RebarMemory().overBudget.load() > fallbackBefore, "a heap mirror did not fall back to host memory when the ReBAR budget was full");
    } else Require(RebarMemory().allocations.load() == rebarBefore, "sparse or nonresident heap allocated ReBAR memory");
    const bool synchronousPublished = std::memcmp(raw, values.data(), sizeof(values)) == 0;
    const bool synchronousCpuPreserved = static_cast<std::uint8_t*>(raw)[8] == 0x55;
    auto synchronousRead = dispatch(false);
    recorder.Sync();
    Require(std::memcmp(synchronousRead->Bytes().data(), values.data(), sizeof(values)) == 0 && static_cast<std::uint8_t*>(raw)[8] == 0x55, "heap reuse after explicit write-back lost the merged stores");
    const std::array<std::uint32_t, 2> initial{0x11111111u, 0x11111111u};
    Memory::Write(address, std::as_bytes(std::span(initial)));
    dispatch(true);
    const auto beforeConsumer = MirrorCounters();
    auto read = dispatch(false);
    Require(!resident || *static_cast<std::uint32_t*>(raw) == 0x11111111u, "GPU consumer forced a guest write-back");
    Require(!resident || MirrorCounters().blocksCopied == beforeConsumer.blocksCopied, "GPU consumer refilled its resident heap");
    recorder.Sync();
    std::array<std::uint32_t, 2> observed{};
    std::memcpy(observed.data(), read->Bytes().data(), sizeof(observed));
    Require(observed == values, "a recorded address consumer lost resident GPU writes");
    std::array<std::byte, sizeof(values)> cpu{};
    Memory::Read(address, cpu);
    Require(std::memcmp(cpu.data(), values.data(), sizeof(values)) == 0, "a CPU read missed resident GPU writes");
    if (resident) {
        const Memory::ReadSiteScope site(Memory::ReadSite::MirrorRefresh);
        Require(!StoreHeapMirrorResults(address + 3 * block, std::as_bytes(std::span(values)), std::as_bytes(std::span(initial))), "a range beyond the heap was accepted for resident storage");
    }
    Memory::Write(address, std::as_bytes(std::span(initial)));
    dispatch(true);
    recorder.Sync();
    const std::array<std::uint32_t, 7> wait{0xc0053c00u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), values[0], UINT32_MAX, 0};
    Require(AgcDriver::Pm4::WaitSatisfiedUnchecked(wait), "WAIT_REG_MEM did not publish completed resident results");
    const std::array<std::uint32_t, 2> cpuValues{0x55667788u, 0x99aabbccu};
    Memory::Write(address, std::as_bytes(std::span(cpuValues)));
    read = dispatch(false);
    recorder.Sync();
    std::memcpy(observed.data(), read->Bytes().data(), sizeof(observed));
    Require(observed == cpuValues, "a GPU consumer missed a CPU write");
    dispatch(true);
    const std::array<std::uint32_t, 2> label{0x99887766u, 0x55443322u};
    recorder.AfterCompletions(address, std::as_bytes(std::span(label)), 1, 0, false);
    recorder.Sync();
    Memory::Read(address, cpu);
    Require(std::memcmp(cpu.data(), label.data(), sizeof(label)) == 0, "resident export rolled back a completion label");
    dispatch(true);
    recorder.Sync();

    static_cast<volatile std::uint8_t*>(raw)[128] = 0x66;
    read = dispatch(false);
    recorder.Sync();
    std::memcpy(observed.data(), read->Bytes().data(), sizeof(observed));
    Require(observed == values && static_cast<std::uint8_t*>(raw)[128] == 0x66, "a raw CPU store caused a refill to discard GPU results");
    if (resident) {
        Memory::Read(address, cpu);

        auto withDetiler = context;
        withDetiler.detiler = &detiler;
        GuestTextureResource resource{};
        resource.baseAddress = address;
        resource.width = 64;
        resource.height = 4;
        resource.mipCount = 1;
        resource.dimension = TextureDimension::k2D;
        resource.tileMode = TextureTileMode::kLinear;
        resource.format = 56;
        auto image = StorageTexture::Create(withDetiler, detiler, resource, 0);
        const auto commands = recorder.Commands();
        recorder.Keep(image);
        const VkImageSubresourceRange layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkClearColorValue red{{1.0f, 0.0f, 0.0f, 1.0f}};
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, &red, 1, &layers);
        image->MarkDirty();
        read = dispatch(false);
        Require(*static_cast<std::uint32_t*>(raw) == values[0], "storage image refresh forced a guest write-back");
        recorder.Sync();
        std::memcpy(observed.data(), read->Bytes().data(), sizeof(observed));
        Require(observed == std::array<std::uint32_t, 2>{0xff0000ffu, 0xff0000ffu}, "address consumer lost resident storage image results");
        Memory::Read(address, cpu);
        Require(std::memcmp(cpu.data(), observed.data(), cpu.size()) == 0, "CPU read lost resident storage image results");
        recorder.Sync();
        recorder.FlushDeferredReleases();
        image.reset();
    }
    dispatch(true);
    recorder.Sync();
    if (resident && device.hostImportAlignment != 0) {
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(address + block, block);
        auto importing = device;
        const auto* imported = HostImportFor(importing, address, 2 * block);
        if (imported != nullptr) Require(std::memcmp(raw, values.data(), sizeof(values)) == 0, "import transition dropped resident results");
    }
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(raw);
    }
    Require(std::memcmp(raw, values.data(), sizeof(values)) == 0, "retirement dropped resident GPU results");
    ClearImageMirrors(context.device);
    ClearHostImports(context.device);
    GuestArena::GuestArenaReset_nid_postfix(raw, 2 * block);
    GuestArena::GuestArenaRelease_nid_postfix(raw, 2 * block);
    if (section != nullptr) CloseHandle(section);
    Require(synchronousPublished, "explicit heap write-back did not publish the GPU stores");
    Require(synchronousCpuPreserved, "explicit heap write-back rolled back a CPU store after the GPU baseline");
    std::cout << (sparse ? "Sparse heap fallback tests passed\n" : "Resident heap recorder tests passed\n");
#endif
}

void RunVramWritableTests(const AgcDriver::Graphics::Context& device, AgcDriver::Graphics::Recorder& recorder, std::unique_lock<AgcDriver::GuestMemory::GpuMutexType>& gpuLock) {
#ifndef _WIN32
    throw std::runtime_error("VRAM writable fixture requires Windows guest page protection");
#else
    using namespace AgcDriver::Graphics;
    namespace Memory = AgcDriver::GuestMemory;
    namespace Abi = ShaderRecompiler::BdaAbi;
    Require(GuestArena::GuestArenaAvailable_nid_postfix() && Memory::WriteWatched(), "VRAM writable fixture requires the watched guest arena");
    auto context = device;
    if (!VramWritable()) {
        context.hostImportAlignment = 0;
        context.dmaBufImport = false;
    }
    constexpr std::size_t block = 65536;
    constexpr std::size_t bytes = 3 * block;
    constexpr std::size_t page = 4096;
    const std::array<std::uint32_t, 2> values{0x12345678u, 0xabcdef09u};
    const std::array<std::uint32_t, 2> initial{0x11111111u, 0x11111111u};
    {
        Require(device.hostImportAlignment != 0 && device.hostImportAlignment <= block, "first-use writable descriptor test requires host imports aligned within 64 KiB");
        void* raw = GuestArena::GuestArenaAllocate_nid_postfix(block, block);
        const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, block, nullptr);
        Require(section != nullptr, "cannot create first-use descriptor memory");
        GuestArena::GuestArenaMap_nid_postfix(raw, block, section, 0, PAGE_READWRITE);
        std::memset(raw, 0x11, block);
        const auto address = reinterpret_cast<std::uint64_t>(raw);
        GuestAllocations::Mutation().Add(raw, block, true, true, true);
        const auto before = VramDescriptorCounters();
        auto memory = std::make_shared<GuestBufferMemory>(device);
        memory->AddWritable(address, sizeof(std::uint32_t));
        memory->Upload(false);
        std::uint32_t adjustment = 0;
        const auto view = memory->Descriptor(address, sizeof(std::uint32_t), adjustment);
        if (VramWritable()) {
            Require(VramDescriptorCounters().mirrorRegions > before.mirrorRegions && VramDescriptorCounters().importedRegions == before.importedRegions, "first-use shared writable descriptor did not select a mirror");
            Require(HostImportFor(device, address, block) == nullptr, "first-use shared writable descriptor claimed a host import");
        } else {
            const auto* imported = HostImportFor(device, address, block);
            Require(imported != nullptr && imported->buffer == view.buffer && VramDescriptorCounters().mirrorRegions == before.mirrorRegions, "disabled writable mode changed first-use descriptor import selection");
        }
        memory->BeginGpuWrites(recorder);
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(device, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        device.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, view.buffer, view.offset + adjustment, sizeof(std::uint32_t), values[0]);
        RecordMemoryBarrier(device, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
        memory->RecordCopyBacks(recorder);
        recorder.Keep(memory);
        recorder.OnComplete([memory] { memory->WriteBack(); });
        memory->ReadyForSubmit(recorder);
        static_cast<volatile std::uint8_t*>(raw)[8] = 0x5a;
        recorder.Sync();
        Require(*static_cast<volatile std::uint32_t*>(raw) == values[0] && static_cast<volatile std::uint8_t*>(raw)[8] == 0x5a, "first-use descriptor publication lost GPU or disjoint CPU bytes");
        memory.reset();
        recorder.FlushDeferredReleases();
        GuestAllocations::Mutation().Remove(raw);
        ClearImageMirrors(device.device);
        ClearHostImports(device.device);
        GuestArena::GuestArenaReset_nid_postfix(raw, block);
        GuestArena::GuestArenaRelease_nid_postfix(raw, block);
        CloseHandle(section);
    }
    if (VramWritable()) {
        auto unsupported = context;
        unsupported.hostImportAlignment = 0;
        unsupported.dmaBufImport = false;
        void* privateMemory = GuestArena::GuestArenaAllocate_nid_postfix(block, block);
        GuestArena::GuestArenaCommit_nid_postfix(privateMemory, block, PAGE_READWRITE, block);
        std::memset(privateMemory, 0x11, block);
        const auto address = reinterpret_cast<std::uint64_t>(privateMemory);
        for (const bool registered : {false, true}) {
            if (registered) GuestAllocations::Mutation().Add(privateMemory, block, true, true, true);
            const auto submissions = recorder.Submissions();
            bool refused = false;
            try {
                GuestBufferMemory memory(unsupported);
                if (registered) memory.AcquireRegistered();
                else memory.AddWritable(address, sizeof(values));
                memory.Upload(registered);
            } catch (const std::runtime_error& error) {
                refused = std::string(error.what()).find("shared protection or a coherent host import") != std::string::npos;
            }
            Require(refused && recorder.Submissions() == submissions && *static_cast<std::uint32_t*>(privateMemory) == 0x11111111u, "unsafe private or unregistered writable memory was not refused before execution");
        }
        GuestAllocations::Mutation().Remove(privateMemory);
        ClearImageMirrors(context.device);
        ClearHostImports(context.device);
        GuestArena::GuestArenaReset_nid_postfix(privateMemory, block);
        GuestArena::GuestArenaRelease_nid_postfix(privateMemory, block);
        recorder.Sync();
        void* abandoned = GuestArena::GuestArenaAllocate_nid_postfix(block, block);
        const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, block, nullptr);
        Require(section != nullptr, "cannot create abandoned writer fixture memory");
        GuestArena::GuestArenaMap_nid_postfix(abandoned, block, section, 0, PAGE_READWRITE);
        std::memset(abandoned, 0x11, block);
        const auto abandonedAddress = reinterpret_cast<std::uint64_t>(abandoned);
        GuestAllocations::Mutation().Add(abandoned, block, true, true, true);
        {
            GuestBufferMemory seed(unsupported);
            seed.SetAddressWrites(false);
            seed.AcquireRegistered();
            seed.Upload(true);
            seed.WriteBack();
        }
        recorder.Sync();
        {
            Recorder invalid(context);
            invalid.Activate();
            auto memory = std::make_unique<GuestBufferMemory>(unsupported);
            memory->AddWritable(abandonedAddress, sizeof(std::uint32_t));
            memory->Upload(false);
            std::uint32_t adjustment = 0;
            const auto view = memory->Descriptor(abandonedAddress, sizeof(std::uint32_t), adjustment);
            memory->BeginGpuWrites(invalid);
            context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(invalid.Commands(), view.buffer, view.offset + adjustment, sizeof(std::uint32_t), 0x76543210u);
            memory.reset();
            bool refused = false;
            try { invalid.Submit(); }
            catch (const std::runtime_error& error) { refused = std::string(error.what()).find("completion was registered") != std::string::npos; }
            Require(refused && invalid.Submissions() == 0, "destroying an unfinished writer allowed its batch to submit");
        }
        recorder.Activate();
        MEMORY_BASIC_INFORMATION restored{};
        Require(!GuestArena::GuestArenaHasGpuOverlay_nid_no_patch(abandonedAddress, block) && VirtualQuery(abandoned, &restored, sizeof(restored)) == sizeof(restored) && restored.Protect != PAGE_NOACCESS, "recorder retirement retained an abandoned writer's inaccessible pages");
        *static_cast<volatile std::uint32_t*>(abandoned) = 0x11223344u;
        Require(*static_cast<volatile std::uint32_t*>(abandoned) == 0x11223344u, "recorder retirement left an unusable write watch");
        {
            GuestBufferMemory synchronous(unsupported);
            synchronous.AddWritable(abandonedAddress, sizeof(std::uint32_t));
            synchronous.Upload(false);
            synchronous.SealForSubmit();
            Require(GuestArena::GuestArenaHasGpuOverlay_nid_no_patch(abandonedAddress, block), "synchronous submission did not retain its page protection");
        }
        Require(!GuestArena::GuestArenaHasGpuOverlay_nid_no_patch(abandonedAddress, block), "abandoned synchronous submission retained its page protection");
        *static_cast<volatile std::uint32_t*>(abandoned) = 0x11223344u;
        GuestAllocations::Mutation().Remove(abandoned);
        ClearImageMirrors(context.device);
        ClearHostImports(context.device);
        GuestArena::GuestArenaReset_nid_postfix(abandoned, block);
        GuestArena::GuestArenaRelease_nid_postfix(abandoned, block);
        CloseHandle(section);
    }
    for (const bool shared : {false, true}) {
        recorder.Sync();
        recorder.FlushDeferredReleases();
        void* raw = GuestArena::GuestArenaAllocate_nid_postfix(bytes, block);
        void* alias = nullptr;
        HANDLE section = nullptr;
        if (shared) {
            section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
            Require(section != nullptr, "cannot create the VRAM writable shared section");
            GuestArena::GuestArenaMap_nid_postfix(raw, bytes, section, 0, PAGE_READWRITE);
            alias = GuestArena::GuestArenaAllocate_nid_postfix(block, block);
            GuestArena::GuestArenaMap_nid_postfix(alias, block, section, 0, PAGE_READWRITE);
        } else GuestArena::GuestArenaCommit_nid_postfix(raw, bytes, PAGE_READWRITE, block);
        std::memset(raw, 0x11, bytes);
        const auto base = reinterpret_cast<std::uint64_t>(raw);
        auto* guest = static_cast<volatile std::uint8_t*>(raw);
        auto* cpuWriter = static_cast<volatile std::uint8_t*>(shared ? alias : raw);
        {
            GuestAllocations::Mutation mutation;
            for (std::size_t index = 0; index < 3; ++index) mutation.Add(static_cast<std::uint8_t*>(raw) + index * block, block, true, index < 2, true);
        }
        void* readiness = nullptr;
        VkDescriptorBufferInfo readinessBuffer{};
        if (VramWritable() && shared) {
            readiness = VirtualAlloc(nullptr, block, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            Require(readiness != nullptr, "cannot allocate the native readiness flag");
            GuestAllocations::Mutation().Add(readiness, block, true, true, true);
            const auto flag = reinterpret_cast<std::uint64_t>(readiness);
            const auto* imported = HostImportFor(context, flag, block);
            Require(imported != nullptr, "native readiness flag was not imported before the mirror build");
            readinessBuffer = {imported->buffer, flag - imported->base, sizeof(std::uint32_t)};
        }
        std::size_t completions = 0;
        std::size_t permissionFaults = 0;
        struct Dispatch {
            std::shared_ptr<GuestBufferMemory> memory;
            std::shared_ptr<BdaResources> bda;
            std::shared_ptr<Buffer> output;
            VkDeviceAddress address;
        };
        const auto dispatch = [&](std::uint64_t target, bool written, bool descriptor = false, bool overflow = false, bool denied = false) {
            Memory::BumpCollectEpoch();
            auto memory = std::make_shared<GuestBufferMemory>(context);
            memory->SetAddressWrites(written && !descriptor);
            memory->AcquireRegistered();
            if (descriptor) memory->AddWritable(target, sizeof(values));
            memory->Upload(true);
            const auto ranges = memory->AddressRanges();
            const auto found = std::find_if(ranges.begin(), ranges.end(), [&](const auto& range) { return range.begin <= target && target < range.end; });
            Require(found != ranges.end(), "VRAM writable range is absent from the BDA table");
            const bool writable = target < base + 2 * block && (VramWritable() || descriptor);
            Require(((found->permissions & Abi::Write) != 0) == writable, "VRAM writable BDA permission differs from its policy");
            auto bda = std::make_shared<BdaResources>(context, *memory);
            auto output = std::make_shared<Buffer>(context, 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            const auto code = written ? MakeBdaDwordWriteTestShader(target, values.size(), values.data()) : MakeBdaDwordReadTestShader(target, values.size(), false, true);
            const std::array<VkDescriptorBufferInfo, 3> bindings{bda->Table(), bda->Fault(), VkDescriptorBufferInfo{output->Handle(), 0, output->Size()}};
            auto pipeline = std::make_shared<Pipeline>(context, code, bindings);
            memory->BeginGpuWrites(recorder);
            RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            pipeline->Record(recorder);
            if (overflow) {
                const auto commands = recorder.Commands();
                RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                std::uint32_t adjustment = 0;
                const auto other = memory->Descriptor(base + block, sizeof(std::uint32_t), adjustment);
                context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, other.buffer, other.offset + adjustment, sizeof(std::uint32_t), 0x76543210u);
                const auto fault = bda->Fault();
                context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, fault.buffer, fault.offset + Abi::WrittenOverflowWord * sizeof(std::uint32_t), sizeof(std::uint32_t), 1);
                RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            }
            memory->RecordCopyBacks(recorder);
            recorder.NotePendingWrites(memory->Writes());
            recorder.Keep(memory);
            recorder.Keep(bda);
            recorder.Keep(output);
            recorder.Keep(pipeline);
            const bool expectPermission = written && (denied || (!VramWritable() && !descriptor));
            recorder.OnComplete([&, memory, bda, expectPermission] {
                bool permission = false;
                try { bda->CheckFault(); }
                catch (const std::runtime_error& error) {
                    if (!expectPermission || std::string(error.what()).find("BDA access failed") == std::string::npos) throw;
                    permission = true;
                }
                Require(permission == expectPermission, "pure BDA write did not follow the writable switch");
                permissionFaults += permission;
                memory->WriteBack();
                ++completions;
            });
            memory->ReadyForSubmit(recorder);
            return Dispatch{memory, bda, output, found->deviceAddress + target - found->begin};
        };
        const auto before = VramWritableCounters();
        dispatch(base, true, true);
        if (VramWritable() && shared) Require(HostImportFor(context, base, block) == nullptr && HostImportFor(context, base + block, block) == nullptr, "writable heap policy imported a shared range while its mirror budget was available");
        if (VramWritable() && !shared) Require(HostImportCovers(context, base, block) && HostImportCovers(context, base + block, block), "private writable ranges did not fall back to coherent host imports");
        cpuWriter[8] = 0x51;
        cpuWriter[9] = 0x52;
        recorder.Sync();
        Require(std::memcmp(raw, values.data(), sizeof(values)) == 0 && guest[8] == 0x51 && guest[9] == 0x52, "descriptor publication lost GPU or disjoint CPU bytes");
        Memory::Write(base, std::as_bytes(std::span(initial)));
        auto writer = dispatch(base, true);
        if (VramWritable() && shared) {
            Require(writer.memory->CopiedWritesOverlap(base, sizeof(values)), "pure BDA mirror writer was invisible to completion-dependent copies");
            MEMORY_BASIC_INFORMATION protection{};
            Require(VirtualQuery(alias, &protection, sizeof(protection)) == sizeof(protection) && protection.Protect == PAGE_READONLY, "prepared GPU writes did not arm native CPU first-write faults");
            const auto armed = VramWritableCounters();
            const auto unsubmitted = recorder.Submissions();
            {
                std::lock_guard lock(Memory::GpuMutex());
                cpuWriter[8] = 0x61;
                const auto firstFault = VramWritableCounters();
                cpuWriter[9] = 0x62;
                Require(firstFault.basePages == armed.basePages + 1 && VramWritableCounters().basePages == firstFault.basePages, "a page did not retain exactly its first CPU-write base");
            }
            Require(recorder.Submissions() == unsubmitted, "pre-submit CPU writes unexpectedly submitted the GPU work");
            recorder.Sync();
            if (VramDeferred()) {
                const auto pending = VramWritableCounters();
                Require(pending.pendingPages == armed.pendingPages + 1 && pending.readbackBytes == armed.readbackBytes, "deferred completion eagerly published a BDA page");
                Require(*static_cast<volatile std::uint32_t*>(raw) == values[0], "deferred BDA page missed its native read fault");
            }
            const auto merged = VramWritableCounters();
            Require(merged.pagesWritten == armed.pagesWritten + 1 && merged.pagesMerged == armed.pagesMerged + 1 && merged.readbackBytes >= armed.readbackBytes + page, "written-page decoding did not publish and merge the one GPU-written page");
            Require(std::memcmp(raw, values.data(), sizeof(values)) == 0 && guest[8] == 0x61 && guest[9] == 0x62, "first-fault merge lost GPU stores or either CPU store");
            Require(guest[page + 8] == 0x11 && guest[block + 8] == 0x11, "written-page publication changed an unwritten page");
            Memory::Write(base, std::as_bytes(std::span(initial)));
            dispatch(base, true);
            std::array<std::uint32_t, 2> completion{};
            const std::array<std::uint32_t, 2> label{0x99887766u, 0x55443322u};
            recorder.AfterCompletions(reinterpret_cast<std::uint64_t>(completion.data()), std::as_bytes(std::span(label)), 1, 0, false);
            recorder.Sync();
            Require(completion == label, "the disjoint completion label was not delivered");
            const auto* native = static_cast<volatile std::uint32_t*>(shared ? alias : raw);
            Require(native[0] == values[0] && native[1] == values[1], "native pointer read after a disjoint completion missed GPU writes");
            std::array<std::byte, sizeof(values)> readback{};
            Memory::Read(base, readback);
            Require(std::memcmp(readback.data(), values.data(), sizeof(values)) == 0, "GuestMemory read missed BDA writes");
            Memory::Write(base, std::as_bytes(std::span(initial)));
            *static_cast<volatile std::uint32_t*>(readiness) = 0;
            dispatch(base, true);
            const auto nativeCommands = recorder.Commands();
            RecordMemoryBarrier(context, nativeCommands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(nativeCommands, readinessBuffer.buffer, readinessBuffer.offset, readinessBuffer.range, 0x76543210u);
            RecordMemoryBarrier(context, nativeCommands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            const auto beforeNativeFault = VramWritableCounters();
            std::array<std::uint32_t, 2> nativeValues{};
            std::exception_ptr nativeFailure;
            recorder.Submit();
            Require(VirtualQuery(alias, &protection, sizeof(protection)) == sizeof(protection) && protection.Protect == PAGE_NOACCESS, "submitted mirror writes left the native alias readable");
            std::thread nativeThread([&] {
                try {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    while (*static_cast<volatile std::uint32_t*>(readiness) != 0x76543210u) {
                        Require(std::chrono::steady_clock::now() < deadline, "GPU readiness flag did not arrive");
                        std::this_thread::yield();
                    }
                    const auto* pointer = static_cast<volatile std::uint32_t*>(alias);
                    nativeValues = {pointer[0], pointer[1]};
                } catch (...) { nativeFailure = std::current_exception(); }
            });
            gpuLock.unlock();
            nativeThread.join();
            gpuLock.lock();
            if (nativeFailure) std::rethrow_exception(nativeFailure);
            Require(nativeValues == values && VramWritableCounters().faults > beforeNativeFault.faults, "native reader after a GPU-written imported flag missed unpublished mirror stores");
            Memory::Write(base, std::as_bytes(std::span(initial)));
            dispatch(base, true);
            recorder.Submit();
            Require(VirtualQuery(alias, &protection, sizeof(protection)) == sizeof(protection) && protection.Protect == PAGE_NOACCESS, "submitted writer did not protect native CPU stores");
            const auto beforeNativeWrite = VramWritableCounters();
            {
                std::lock_guard lock(Memory::GpuMutex());
                cpuWriter[10] = 0x63;
            }
            Require(VramWritableCounters().faults > beforeNativeWrite.faults && std::memcmp(raw, values.data(), sizeof(values)) == 0 && guest[10] == 0x63, "native write fault with the GPU lock held lost CPU or GPU stores");
            Memory::Write(base, std::as_bytes(std::span(initial)));
            writer = dispatch(base, true);
            const auto completed = completions;
            const auto copied = MirrorCounters().blocksCopied;
            const auto reader = dispatch(base, false);
            Require(reader.address == writer.address && completions == completed && MirrorCounters().blocksCopied == copied, "GPU reader replaced or synchronized a clean writable mirror");
            recorder.Sync();
            Require(std::memcmp(reader.output->Bytes().data(), values.data(), sizeof(values)) == 0, "GPU reader lost the preceding mirror writer");
            if (VramDeferred()) FlushHeapMirrorWrites(base, 2 * block, true);
            const auto overflowBefore = VramWritableCounters();
            const auto overflow = dispatch(base, true, false, true);
            recorder.Sync();
            if (VramDeferred()) {
                Require(VramWritableCounters().pendingPages == overflowBefore.pendingPages + 2 * block / page, "overflow did not retain every writable page");
                FlushHeapMirrorWrites(base, 2 * block, true);
            }
            const auto overflowAfter = VramWritableCounters();
            Require(overflowAfter.overflowFallbacks == overflowBefore.overflowFallbacks + 1 && overflowAfter.pagesWritten == overflowBefore.pagesWritten + 2 * block / page, "written-page overflow did not publish exactly the writable mirror set");
            Require(*reinterpret_cast<volatile std::uint32_t*>(base + block) == 0x76543210u && guest[2 * block] == 0x11, "overflow publication lost a writable mirror or changed a read-only mirror");
            overflow.bda->CheckFault();
            Require(VramWritableCounters().overflowFallbacks == overflowAfter.overflowFallbacks, "a cleared overflow was processed twice");
            dispatch(base + 2 * block, true, false, false, true);
            recorder.Sync();
            Require(permissionFaults == 1 && guest[2 * block] == 0x11, "writable policy granted a store to a read-only range");
        } else if (VramWritable()) {
            recorder.Sync();
            Require(permissionFaults == 0 && std::memcmp(raw, values.data(), sizeof(values)) == 0, "private imported writable fallback lost BDA stores");
            Require(!writer.memory->CopiedWritesOverlap(base, sizeof(values)), "private imported fallback acquired a deferred CPU publication");
            const auto after = VramWritableCounters();
            Require(after.pagesWritten == before.pagesWritten && after.basePages == before.basePages && after.readbackBytes == before.readbackBytes, "private writable fallback created mirror publication work");
        } else {
            recorder.Sync();
            Require(permissionFaults == 1 && std::memcmp(raw, initial.data(), sizeof(initial)) == 0, "disabled writable mode changed pure BDA store behavior");
            const auto after = VramWritableCounters();
            Require(after.pagesWritten == before.pagesWritten && after.pagesMerged == before.pagesMerged && after.basePages == before.basePages && after.readbackBytes == before.readbackBytes, "disabled writable mode allocated publication work");
            auto overflow = std::make_shared<BdaResources>(context);
            const auto fault = overflow->Fault();
            context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(recorder.Commands(), fault.buffer, fault.offset + Abi::WrittenOverflowWord * sizeof(std::uint32_t), sizeof(std::uint32_t), 1);
            RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            recorder.Keep(overflow);
            recorder.Sync();
            bool rejected = false;
            try { overflow->CheckFault(); }
            catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("chunks") != std::string::npos; }
            Require(rejected, "disabled writable mode accepted written-page overflow");
        }
        recorder.Sync();
        recorder.FlushDeferredReleases();
        writer = {};
        {
            GuestAllocations::Mutation mutation;
            for (std::size_t index = 0; index < 3; ++index) mutation.Remove(static_cast<std::uint8_t*>(raw) + index * block);
        }
        ClearImageMirrors(context.device);
        ClearHostImports(context.device);
        if (readiness != nullptr) {
            GuestAllocations::Mutation().Remove(readiness);
            VirtualFree(readiness, 0, MEM_RELEASE);
        }
        if (alias != nullptr) {
            GuestArena::GuestArenaReset_nid_postfix(alias, block);
            GuestArena::GuestArenaRelease_nid_postfix(alias, block);
        }
        GuestArena::GuestArenaReset_nid_postfix(raw, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(raw, bytes);
        if (section != nullptr) CloseHandle(section);
    }
    std::cout << "VRAM writable coherence tests passed\n";
#endif
}

void RunVramDeferredTests(const AgcDriver::Graphics::Context& context, AgcDriver::Graphics::Recorder& recorder) {
#ifdef _WIN32
    using namespace AgcDriver::Graphics;
    namespace Memory = AgcDriver::GuestMemory;
    Require(VramWritable(), "deferred publication tests require writable mirrors");
    constexpr std::size_t bytes = 65536;
    constexpr std::size_t page = 4096;
    void* raw = GuestArena::GuestArenaAllocate_nid_postfix(bytes, bytes);
    void* alias = GuestArena::GuestArenaAllocate_nid_postfix(bytes, bytes);
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
    Require(section != nullptr, "cannot create deferred publication memory");
    GuestArena::GuestArenaMap_nid_postfix(raw, bytes, section, 0, PAGE_READWRITE);
    GuestArena::GuestArenaMap_nid_postfix(alias, bytes, section, 0, PAGE_READWRITE);
    const auto base = reinterpret_cast<std::uint64_t>(raw);
    auto* native = static_cast<volatile std::uint8_t*>(alias);
    GuestAllocations::Mutation().Add(raw, bytes, true, true, true);
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        std::memset(raw, 0x11, bytes);
        Memory::BumpCollectEpoch();
        const auto before = VramWritableCounters();
        auto memory = std::make_shared<GuestBufferMemory>(context);
        memory->AcquireRegistered();
        memory->AddWritable(base, sizeof(std::uint32_t));
        memory->AddWritable(base + 2 * page, sizeof(std::uint32_t));
        memory->Upload(true);
        memory->BeginGpuWrites(recorder);
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        for (const auto offset : {0u, 2u * static_cast<unsigned>(page)}) {
            std::uint32_t adjustment = 0;
            const auto view = memory->Descriptor(base + offset, sizeof(std::uint32_t), adjustment);
            context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, view.buffer, view.offset + adjustment, sizeof(std::uint32_t), 0x76543210u + offset);
        }
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
        memory->RecordCopyBacks(recorder);
        recorder.Keep(memory);
        recorder.OnComplete([memory] { memory->WriteBack(); });
        memory->ReadyForSubmit(recorder);
        if (scenario == 1) native[2 * page + 8] = 0x69;
        recorder.Sync();
        memory.reset();
        recorder.FlushDeferredReleases();
        const auto completed = VramWritableCounters();
        Require(!HasVramMirrorWriters(), "completed deferred pages retained an in-flight writer");
        if (VramDeferred()) Require(completed.pendingPages == before.pendingPages + 2 && completed.readbackBytes == before.readbackBytes, "deferred completion did not retain exactly the two written pages");
        else Require(completed.pendingPages == before.pendingPages && completed.readbackBytes == before.readbackBytes + 2 * page, "disabled deferred mode did not publish eagerly");
        MEMORY_BASIC_INFORMATION protection{};
        Require(VirtualQuery(static_cast<std::uint8_t*>(alias) + page, &protection, sizeof(protection)) == sizeof(protection) && protection.Protect != PAGE_NOACCESS && native[page] == 0x11, "an unwritten candidate page stayed inaccessible");
        if (scenario == 0) {
            const auto submissions = recorder.Submissions();
            GuestBufferMemory reader(context);
            reader.SetAddressWrites(false);
            reader.AcquireRegistered();
            reader.Upload(true);
            reader.WriteBack();
            Require(recorder.Submissions() == submissions && VramWritableCounters().pendingPages == completed.pendingPages && VramWritableCounters().readbackBytes == completed.readbackBytes, "a clean GPU consumer published deferred CPU pages");
        }
        if (scenario < 2) {
            {
                std::lock_guard recursive(Memory::GpuMutex());
                Require(*static_cast<volatile std::uint32_t*>(alias) == 0x76543210u, "native read did not publish the first deferred page");
            }
            const auto first = VramWritableCounters();
            if (VramDeferred()) Require(first.pendingPages == before.pendingPages + 1 && first.readbackBytes == before.readbackBytes + page, "one native read published another dirty page");
            std::array<std::byte, sizeof(std::uint32_t)> readback{};
            Memory::Read(base + 2 * page, readback);
            std::uint32_t result = 0;
            std::memcpy(&result, readback.data(), sizeof(result));
            Require(result == 0x76545210u && native[2 * page + 8] == (scenario == 1 ? 0x69 : 0x11), "deferred merge lost GPU or disjoint CPU bytes");
        } else if (scenario == 2) {
            FlushHeapMirrorWrites(base, sizeof(std::uint32_t), true);
        } else {
            GuestAllocations::Mutation().Remove(raw);
            GuestAllocations::Mutation().Add(raw, bytes, true, true, true);
        }
        Require(VramWritableCounters().pendingPages == before.pendingPages && !GuestArena::GuestArenaHasGpuOverlay_nid_no_patch(base, bytes), "publication or retirement retained protected pages");
        Require(*static_cast<volatile std::uint32_t*>(alias) == 0x76543210u && *reinterpret_cast<volatile std::uint32_t*>(static_cast<std::uint8_t*>(alias) + 2 * page) == 0x76545210u, "retirement lost a deferred GPU result");
    }
    GuestAllocations::Mutation().Remove(raw);
    ClearImageMirrors(context.device);
    ClearHostImports(context.device);
    for (auto* mapping : {raw, alias}) {
        GuestArena::GuestArenaReset_nid_postfix(mapping, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(mapping, bytes);
    }
    CloseHandle(section);
    std::cout << "VRAM deferred publication tests passed\n";
#endif
}

void RunVramLazyTests(const AgcDriver::Graphics::Context& device, AgcDriver::Graphics::Recorder& recorder, bool imports) {
#ifndef _WIN32
    throw std::runtime_error("lazy mirror fixture requires Windows guest page protection");
#else
    using namespace AgcDriver::Graphics;
    namespace Memory = AgcDriver::GuestMemory;
    constexpr std::size_t block = 65536;
    constexpr std::size_t bytes = 2 * block;
    constexpr std::size_t page = 4096;
    auto context = device;
    if (!imports) {
        context.hostImportAlignment = 0;
        context.dmaBufImport = false;
    }
    auto* raw = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, block));
    auto* alias = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, block));
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
    Require(raw != nullptr && alias != nullptr && section != nullptr, "cannot create lazy mirror fixture");
    GuestArena::GuestArenaMap_nid_postfix(raw, bytes, section, 0, PAGE_READWRITE);
    GuestArena::GuestArenaMap_nid_postfix(alias, bytes, section, 0, PAGE_READWRITE);
    std::memset(raw, 0x11, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(raw);
    GuestAllocations::Mutation().Add(raw, bytes, true, true, true);
    struct Release {
        const Context& context;
        Recorder& recorder;
        std::uint8_t* raw;
        std::uint8_t* alias;
        HANDLE section;
        ~Release() {
            recorder.Sync();
            recorder.FlushDeferredReleases();
            ClearImageMirrors(context.device);
            ClearHostImports(context.device);
            GuestAllocations::Mutation().Remove(raw);
            for (auto* mapping : {raw, alias}) {
                GuestArena::GuestArenaReset_nid_postfix(mapping, bytes);
                GuestArena::GuestArenaRelease_nid_postfix(mapping, bytes);
            }
            CloseHandle(section);
        }
    } release{context, recorder, raw, alias, section};
    {
        GuestBufferMemory seed(context);
        seed.SetAddressWrites(false);
        seed.AcquireRegistered();
        seed.Upload(true);
        seed.WriteBack();
    }
    const auto write = [&](std::uint64_t at, std::uint32_t value, bool lateCpu = false, std::size_t width = sizeof(std::uint32_t)) {
        Memory::BumpCollectEpoch();
        auto memory = std::make_shared<GuestBufferMemory>(context);
        memory->AddWritable(at, width, true);
        memory->Upload(false);
        std::uint32_t adjustment = 0;
        const auto view = memory->Descriptor(at, width, adjustment);
        if (lateCpu) alias[8] = 0x66;
        memory->BeginGpuWrites(recorder);
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, view.buffer, view.offset + adjustment, width, value);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        memory->RecordCopyBacks(recorder);
        memory->NotePendingWrites(recorder);
        recorder.Keep(memory);
        recorder.OnComplete([memory] { memory->WriteBack(); });
        memory->ReadyForSubmit(recorder);
        return view.buffer;
    };
    const auto read = [&] {
        Memory::BumpCollectEpoch();
        auto memory = std::make_shared<GuestBufferMemory>(context);
        memory->AddReadable(address, 2 * page);
        memory->Upload(false);
        memory->BeginGpuWrites(recorder);
        std::uint32_t adjustment = 0;
        const auto view = memory->Descriptor(address, 2 * page, adjustment);
        auto output = std::make_shared<Buffer>(context, 2 * page, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        CopyBuffer(context, recorder.Commands(), view.buffer, view.offset + adjustment, output->Handle(), 0, 2 * page);
        RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Keep(memory);
        recorder.Keep(output);
        recorder.OnComplete([memory] { memory->WriteBack(); });
        return output;
    };
    const auto initial = VramLazyCounters();
    const auto backing = write(address, 0x12345678u, true);
    const auto first = VramLazyCounters();
    if (VramLazyWrites()) Require(first.snapshotCalls == initial.snapshotCalls + 1, "the late CPU snapshot was not counted");
    Require(write(address, 0x76543210u) == backing, "repeated lazy descriptor writes changed buffer ownership");
    alias[page + 8] = 0x55;
    const auto beforeRead = VramLazyCounters();
    auto output = read();
    recorder.Sync();
    const auto result = output->ReadBytes(0, 2 * page);
    Require(*reinterpret_cast<const std::uint32_t*>(result.data()) == 0x76543210u && result[8] == std::byte{0x66} && result[page + 8] == std::byte{0x55}, "GPU reuse lost a GPU store or a neighbouring CPU store");
    if (VramLazyWrites()) {
        const auto after = VramLazyCounters();
        Require(first.protectedAtRecord == initial.protectedAtRecord + 1 && after.protectedAtRecord == first.protectedAtRecord, "repeated writes reprotected a GPU-owned page or protected a whole heap");
        Require(after.downloadedPages == beforeRead.downloadedPages && after.pendingPages == initial.pendingPages + 1, "GPU-only reuse downloaded its mirror");
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(alias, &info, sizeof(info)) == sizeof(info) && info.Protect == PAGE_NOACCESS, "the native alias of a lazy page was not protected");
        Require(VirtualQuery(alias + page, &info, sizeof(info)) == sizeof(info) && info.Protect != PAGE_NOACCESS, "an unrelated page was protected");
    }
    if (VramLazyWrites() && VramTextures()) {
        const auto beforeSource = VramLazyCounters();
        const auto source = MirrorSourceFor(context, address, 12);
        Require(source.has_value() && source->buffer == backing, "lazily written indirect arguments have no device-local source");
        auto words = std::make_shared<Buffer>(context, 12, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        CopyBuffer(context, recorder.Commands(), source->buffer, source->offset, words->Handle(), 0, 12);
        RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Keep(words);
        recorder.Keep(source->lifetime);
        recorder.Sync();
        const auto copied = words->ReadBytes(0, 12);
        std::array<std::uint32_t, 3> values{};
        std::memcpy(values.data(), copied.data(), sizeof(values));
        Require(values[0] == 0x76543210u && values[1] == 0x11111111u && values[2] == 0x11111166u, "the device-local source of lazily written arguments lost the GPU store");
        Require(VramLazyCounters().downloadedPages == beforeSource.downloadedPages, "the device-local source of lazily written arguments downloaded its page");
    }
    const auto beforeFault = VramLazyCounters();
    Require(*reinterpret_cast<volatile std::uint32_t*>(alias) == 0x76543210u, "native read missed lazy GPU writes");
    if (VramLazyWrites()) Require(VramLazyCounters().downloadedPages == beforeFault.downloadedPages + 1, "native read did not download exactly its page");
    write(address, 0x89abcdefu);
    const auto beforeWriteFault = VramLazyCounters();
    alias[9] = 0x63;
    if (VramLazyWrites()) Require(VramLazyCounters().faultPageWaits == beforeWriteFault.faultPageWaits + 1, "a descriptor writer fault wait was not classified");
    output = read();
    recorder.Sync();
    const auto merged = output->ReadBytes(0, 2 * page);
    Require(*reinterpret_cast<const std::uint32_t*>(merged.data()) == 0x89abcdefu && merged[9] == std::byte{0x63}, "native write fault lost CPU or GPU data");
    {
        GuestBufferMemory synchronous(context);
        synchronous.AddWritable(address, sizeof(std::uint32_t));
        synchronous.Upload(false);
        std::uint32_t adjustment = 0;
        const auto view = synchronous.Descriptor(address, sizeof(std::uint32_t), adjustment);
        CommandBatch batch(context);
        alias[10] = 0x42;
        synchronous.BeginGpuWrites(batch);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(batch.Handle(), view.buffer, view.offset + adjustment, sizeof(std::uint32_t), 0xabcdef01u);
        synchronous.SealForSubmit(&batch);
        batch.SubmitAndWait();
        synchronous.WriteBack();
        Require(*reinterpret_cast<volatile std::uint32_t*>(alias) == 0xabcdef01u && alias[10] == 0x42, "synchronous lazy submission missed its final CPU snapshot or GPU write");
    }
    if (VramLazyWrites()) {
        {
            auto abandoned = std::make_unique<GuestBufferMemory>(context);
            abandoned->AddWritable(address, sizeof(std::uint32_t));
            abandoned->Upload(false);
            abandoned->BeginGpuWrites(recorder);
        }
        recorder.Sync();
        Require(*reinterpret_cast<volatile std::uint32_t*>(alias) == 0xabcdef01u, "abandoned recorded work left an unwakeable lazy page");
        {
            GuestBufferMemory abandoned(context);
            abandoned.AddWritable(address, sizeof(std::uint32_t));
            abandoned.Upload(false);
            CommandBatch batch(context);
            abandoned.BeginGpuWrites(batch);
            abandoned.SealForSubmit(&batch);
        }
        Require(!GuestArena::GuestArenaHasGpuOverlay_nid_no_patch(address, page) && *reinterpret_cast<volatile std::uint32_t*>(alias) == 0xabcdef01u, "abandoned synchronous work retained lazy protection");
        const auto bdaWrite = [&](std::uint64_t at, std::uint32_t value, bool abandon = false) {
            Memory::BumpCollectEpoch();
            auto memory = std::make_shared<GuestBufferMemory>(context);
            memory->SetAddressWrites(true);
            memory->AcquireRegistered();
            memory->Upload(true);
            auto bda = std::make_shared<BdaResources>(context, *memory);
            auto output = std::make_shared<Buffer>(context, 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            const std::array<std::uint32_t, 2> values{value, value};
            const auto code = MakeBdaDwordWriteTestShader(at, values.size(), values.data());
            const std::array<VkDescriptorBufferInfo, 3> bindings{bda->Table(), bda->Fault(), VkDescriptorBufferInfo{output->Handle(), 0, output->Size()}};
            auto pipeline = std::make_shared<Pipeline>(context, code, bindings);
            memory->BeginGpuWrites(recorder);
            pipeline->Record(recorder);
            memory->RecordCopyBacks(recorder);
            memory->NotePendingWrites(recorder);
            if (!abandon) recorder.Keep(memory);
            recorder.Keep(bda);
            recorder.Keep(output);
            recorder.Keep(pipeline);
            if (!abandon) {
                recorder.OnComplete([memory, bda] { bda->CheckFault(); memory->WriteBack(); });
                memory->ReadyForSubmit(recorder);
            }
        };
        const auto beforeBda = VramLazyCounters();
        bdaWrite(address + page, 0x2468ace0u);
        Require(!GuestArena::GuestArenaHasGpuOverlay_nid_no_patch(address + page, page), "raw-pointer writes protected their heap before completion");
        *reinterpret_cast<volatile std::uint32_t*>(alias + page) = 0xdeadbeefu;
        std::uint32_t label = 0;
        const std::uint32_t ready = 0x55aa55aau;
        recorder.AfterCompletions(reinterpret_cast<std::uint64_t>(&label), std::as_bytes(std::span(&ready, 1)), 1, 0, false);
        recorder.Sync();
        Require(label == ready && GuestArena::GuestArenaHasGpuOverlay_nid_no_patch(address + page, page), "BDA protection was not established before completion labels");
        Require(VramLazyCounters().protectedAtCompletion == beforeBda.protectedAtCompletion + 1 && VramLazyCounters().conflicts > beforeBda.conflicts, "BDA page publication missed its CPU-write conflict");
        Require(*reinterpret_cast<volatile std::uint32_t*>(alias + page) == 0x2468ace0u, "GPU bytes did not win the counted BDA conflict");
        bdaWrite(address + 3 * page, 0x10293847u);
        write(address, 0xabcdef01u);
        const auto beforeGpuReuse = VramLazyCounters();
        const auto beforeGpuUploads = VramUploadCounters();
        {
            GuestBufferMemory gpuOnly(context);
            gpuOnly.SetAddressWrites(false);
            gpuOnly.AcquireRegistered();
            gpuOnly.Upload(true);
            gpuOnly.WriteBack();
        }
        Require(VramLazyCounters().writerWaits == beforeGpuReuse.writerWaits && VramUploadCounters().bytes == beforeGpuUploads.bytes,
            "GPU-only write markers caused a mirror refresh or writer wait");
        const auto beforeBdaFault = VramLazyCounters();
        Require(*reinterpret_cast<volatile std::uint32_t*>(alias) == 0xabcdef01u && VramLazyCounters().faultBdaWaits == beforeBdaFault.faultBdaWaits + 1,
            "a raw-pointer writer fault wait was not classified");
        recorder.Sync();
        Require(*reinterpret_cast<volatile std::uint32_t*>(alias + 3 * page) == 0x10293847u, "GPU-only mirror reuse lost a raw-pointer result");
        const std::byte hostLabel{0x77};
        Memory::StoreOwnBytes(address + page + 8, std::span(&hostLabel, 1));
        output = read();
        recorder.Sync();
        Require(output->ReadBytes(0, 2 * page)[page + 8] == hostLabel, "lazy refresh ignored a driver CPU store beside a protected page");
        const auto beforeCoalesced = VramLazyCounters();
        write(address + 4 * page, 0x8899aabbu, false, 2 * page);
        const auto coalesced = VramLazyCounters();
        Require(coalesced.protectRuns == beforeCoalesced.protectRuns + 1 && coalesced.protectCalls == beforeCoalesced.protectCalls + 2,
            "contiguous pages did not share one protection call per native alias");
        write(address + 4 * page, 0xaabbccddu, false, 2 * page);
        Require(VramLazyCounters().protectRuns == coalesced.protectRuns, "GPU-only reuse reprotected an existing range");
        recorder.Sync();
        Require(*reinterpret_cast<volatile std::uint32_t*>(alias + 4 * page) == 0xaabbccddu && *reinterpret_cast<volatile std::uint32_t*>(alias + 5 * page) == 0xaabbccddu,
            "coalesced lazy protection lost GPU results on a native alias");
        {
            const auto fillAddress = address + 8 * page + 64;
            const std::size_t fillBytes = 2 * page + 64;
            volatile std::uint8_t* const native = alias;
            native[8 * page + 8] = 0x5c;
            native[10 * page + 200] = 0x6d;
            const auto beforeFill = VramLazyCounters();
            Require(LazyMirrorFillCovers(context, fillAddress, fillBytes), "a writable lazy mirror did not cover its fill");
            const auto fill = BeginLazyMirrorFill(context, recorder, fillAddress, fillBytes);
            const auto commands = recorder.Commands();
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, fill.buffer, fill.offset, fillBytes, 0xc3c3c3c3u);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
            FinishLazyMirrorFill(recorder, fill, fillAddress, fillBytes);
            const auto filled = VramLazyCounters();
            Require(filled.mirrorFills == beforeFill.mirrorFills + 1 && filled.mirrorFillBytes == beforeFill.mirrorFillBytes + fillBytes && filled.protectedAtRecord == beforeFill.protectedAtRecord + 3 && filled.snapshotCalls == beforeFill.snapshotCalls + 2,
                "a mirror fill did not protect its pages or snapshot its changed edges");
            Require(GuestArena::GuestArenaHasGpuOverlay_nid_no_patch(address + 9 * page, page), "a mirror fill left its page unprotected");
            recorder.Submit();
            Check(context.Function<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(context.queue), "lazy mirror fill GPU wait");
            Require(native[9 * page + 100] == 0xc3 && VramLazyCounters().faultPageWaits == filled.faultPageWaits + 1, "a CPU read of a mirror fill did not fault and wait for it");
            Require(native[8 * page + 8] == 0x5c && native[8 * page + 63] == 0x11 && native[8 * page + 64] == 0xc3 && native[10 * page + 127] == 0xc3 && native[10 * page + 128] == 0x11 && native[10 * page + 200] == 0x6d,
                "a mirror fill lost the CPU bytes beside it");
            Require(VramLazyCounters().downloadedPages == filled.downloadedPages + 3, "a mirror fill did not download exactly its pages");
        }
        if (VramTextures()) {
            const auto texture = address + 24 * page;
            const auto source = MirrorSourceFor(context, texture, 2 * page);
            Require(source.has_value(), "a lazy heap mirror offered no texture source");
            const auto generation = Memory::CollectWritesUncached(texture, 2 * page);
            Require(MirrorSourceUnchanged(context, texture, 2 * page, source->proof, generation), "a fresh mirror texture proof did not hold");
            write(address + 4 * page, 0x0badf00du);
            recorder.Sync();
            Require(MirrorSourceUnchanged(context, texture, 2 * page, source->proof, generation), "a GPU write elsewhere in the heap invalidated a mirror texture proof");
            write(texture, 0x600dcafeu);
            recorder.Sync();
            Require(!MirrorSourceUnchanged(context, texture, 2 * page, source->proof, generation), "a GPU write into the texture kept its mirror proof");
        }
        if (imports) {
            auto* flag = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, block, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            Require(flag != nullptr, "cannot allocate a native lazy readiness flag");
            GuestAllocations::Mutation().Add(flag, block, true, true, true);
            const auto flagAddress = reinterpret_cast<std::uint64_t>(flag);
            const auto* imported = HostImportFor(context, flagAddress, block);
            Require(imported == nullptr, "lazy writes allowed a native readiness flag to bypass completion publication");
            const auto beforeUnordered = VramLazyCounters();
            bdaWrite(address + block, 0x13579bdfu);
            Require(VramLazyCounters().unorderedImportRanges == beforeUnordered.unorderedImportRanges + 1 && VramLazyCounters().unorderedImportBytes == beforeUnordered.unorderedImportBytes + block,
                "an unmirrorable writable range was not counted as an unordered import");
            write(flagAddress, 0x5a5a5a5au);
            recorder.Submit();
            Check(context.Function<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(context.queue), "native lazy readiness GPU wait");
            const bool ready = *static_cast<volatile std::uint32_t*>(flag) == 0x5a5a5a5au;
            recorder.Sync();
            Require(!ready && *static_cast<volatile std::uint32_t*>(flag) == 0x5a5a5a5au &&
                *reinterpret_cast<volatile std::uint32_t*>(alias + block) == 0x13579bdfu,
                "a native readiness flag overtook lazy BDA publication");
            auto* readOnly = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, block, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            Require(readOnly != nullptr, "cannot allocate a native read-only range");
            GuestAllocations::Mutation().Add(readOnly, block, true, false, true);
            const auto readOnlyAddress = reinterpret_cast<std::uint64_t>(readOnly);
            const auto beforeReadOnly = VramLazyCounters();
            {
                Memory::BumpCollectEpoch();
                GuestBufferMemory space(context);
                space.SetAddressWrites(false);
                space.AcquireRegistered();
                space.Upload(true);
                const auto ranges = space.AddressRanges();
                const auto permissions = [&](std::uint64_t at) {
                    const auto found = std::find_if(ranges.begin(), ranges.end(), [&](const auto& range) { return range.begin <= at && at < range.end; });
                    Require(found != ranges.end(), "a native range is absent from the BDA table");
                    return found->permissions;
                };
                Require(permissions(readOnlyAddress) == ShaderRecompiler::BdaAbi::Read && HostImportCovers(context, readOnlyAddress, block), "an unmirrorable read-only range was not served by a read-only import");
                Require(permissions(flagAddress) == (ShaderRecompiler::BdaAbi::Read | ShaderRecompiler::BdaAbi::Write), "an unordered writable import lost its BDA write permission");
                space.WriteBack();
            }
            const auto counted = VramLazyCounters();
            Require(counted.readOnlyImportBuilds > beforeReadOnly.readOnlyImportBuilds && counted.unorderedImportBuilds > beforeReadOnly.unorderedImportBuilds && counted.unorderedImportRanges == beforeReadOnly.unorderedImportRanges,
                "read-only and unordered imports were not counted per build");
            recorder.FlushDeferredReleases();
            GuestAllocations::Mutation().Remove(readOnly);
            VirtualFree(readOnly, 0, MEM_RELEASE);
            {
                auto* joined = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(2 * block, block));
                auto* twin = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(block, block));
                const auto joinedSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, 2 * block, nullptr);
                Require(joined != nullptr && twin != nullptr && joinedSection != nullptr, "cannot create contiguous lazy import ranges");
                GuestArena::GuestArenaMap_nid_postfix(joined, 2 * block, joinedSection, 0, PAGE_READWRITE);
                GuestArena::GuestArenaMap_nid_postfix(twin, block, joinedSection, 0, PAGE_READWRITE);
                std::memset(joined, 0x22, 2 * block);
                {
                    GuestAllocations::Mutation mutation;
                    mutation.Add(joined, block, true, true, true);
                    mutation.Add(joined + block, block, true, true, true);
                    mutation.Add(twin, block, true, true, true);
                }
                const auto joinedAddress = reinterpret_cast<std::uint64_t>(joined);
                const auto beforeJoined = VramLazyCounters();
                {
                    Memory::BumpCollectEpoch();
                    GuestBufferMemory space(context);
                    space.SetAddressWrites(false);
                    space.AcquireRegistered();
                    space.Upload(true);
                    Require(HostImportCovers(context, joinedAddress, block) && !HostImportCovers(context, joinedAddress + block, 1), "an unordered import joined a mirrorable neighbour");
                    Require(VramLazyCounters().unorderedImportRanges == beforeJoined.unorderedImportRanges + 2 && VramLazyCounters().unorderedImportBytes == beforeJoined.unorderedImportBytes + 2 * block,
                        "only the aliased ranges of a contiguous pair were not counted as unordered");
                    space.WriteBack();
                }
                recorder.Sync();
                recorder.FlushDeferredReleases();
                ClearImageMirrors(context.device);
                ClearHostImports(context.device);
                {
                    GuestAllocations::Mutation mutation;
                    mutation.Remove(joined);
                    mutation.Remove(joined + block);
                    mutation.Remove(twin);
                }
                GuestArena::GuestArenaReset_nid_postfix(joined, 2 * block);
                GuestArena::GuestArenaRelease_nid_postfix(joined, 2 * block);
                GuestArena::GuestArenaReset_nid_postfix(twin, block);
                GuestArena::GuestArenaRelease_nid_postfix(twin, block);
                CloseHandle(joinedSection);
            }
            GuestAllocations::Mutation().Remove(flag);
            VirtualFree(flag, 0, MEM_RELEASE);
        }
        {
            auto* privateBytes = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(block, block));
            GuestArena::GuestArenaCommit_nid_postfix(privateBytes, block, PAGE_READWRITE, block);
            std::memset(privateBytes, 0x11, block);
            GuestAllocations::Mutation().Add(privateBytes, block, true, true, true);
            const auto privateAddress = reinterpret_cast<std::uint64_t>(privateBytes);
            const auto beforeFallback = VramLazyCounters();
            write(privateAddress, 0x98765432u);
            recorder.Sync();
            Require(*reinterpret_cast<volatile std::uint32_t*>(privateBytes) == 0x98765432u && VramLazyCounters().fallbacks > beforeFallback.fallbacks,
                "unprotectable private memory did not retain a coherent fallback");
            recorder.FlushDeferredReleases();
            GuestAllocations::Mutation().Remove(privateBytes);
            GuestArena::GuestArenaReset_nid_postfix(privateBytes, block);
            GuestArena::GuestArenaRelease_nid_postfix(privateBytes, block);
        }
        bdaWrite(address + 2 * page, 0x31415926u, true);
        recorder.Sync();
        Require(*reinterpret_cast<volatile std::uint32_t*>(alias + 2 * page) == 0x31415926u, "abandoned raw-pointer work lost its GPU result");
        write(address, 0x11223344u);
        recorder.Sync();
        const auto beforeClear = VramLazyCounters();
        ClearImageMirrors(context.device);
        Require(VramLazyCounters().pendingPages == 0 && VramLazyCounters().downloadedPages > beforeClear.downloadedPages && *reinterpret_cast<volatile std::uint32_t*>(alias) == 0x11223344u, "clearing mirrors lost lazy GPU data");
    } else {
        Require(!LazyMirrorFillCovers(context, address, page) && VramLazyCounters().mirrorFills == 0, "disabled lazy writes offered a mirror fill");
        Require(VramLazyCounters().protectedAtRecord == initial.protectedAtRecord && VramLazyCounters().downloadedPages == initial.downloadedPages, "disabled lazy writes changed page ownership");
    }
#endif
}

void RunLazyStorageWriteBackTests(const AgcDriver::Graphics::Context& device, AgcDriver::Graphics::Recorder& recorder) {
#ifndef _WIN32
    throw std::runtime_error("lazy storage write-back fixture requires Windows guest page protection");
#else
    using namespace AgcDriver::Graphics;
    constexpr std::size_t block = 65536;
    constexpr std::uint32_t side = 256;
    constexpr std::size_t surfaceBytes = side * side * 4;
    constexpr std::size_t bytes = surfaceBytes + block;
    auto context = device;
    context.hostImportAlignment = 0;
    context.dmaBufImport = false;
    auto* raw = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, block));
    auto* alias = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, block));
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
    Require(raw != nullptr && alias != nullptr && section != nullptr, "cannot create the lazy storage write-back fixture");
    GuestArena::GuestArenaMap_nid_postfix(raw, bytes, section, 0, PAGE_READWRITE);
    GuestArena::GuestArenaMap_nid_postfix(alias, bytes, section, 0, PAGE_READWRITE);
    std::memset(raw, 0x55, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(raw);
    GuestAllocations::Mutation().Add(raw, bytes, true, true, true);
    struct Release {
        const Context& context;
        Recorder& recorder;
        std::uint8_t* raw;
        std::uint8_t* alias;
        HANDLE section;
        ~Release() {
            recorder.Sync();
            recorder.FlushDeferredReleases();
            ClearCachedTextures(context.device);
            ClearImageMirrors(context.device);
            ClearHostImports(context.device);
            GuestAllocations::Mutation().Remove(raw);
            for (auto* mapping : {raw, alias}) {
                GuestArena::GuestArenaReset_nid_postfix(mapping, bytes);
                GuestArena::GuestArenaRelease_nid_postfix(mapping, bytes);
            }
            CloseHandle(section);
        }
    } release{context, recorder, raw, alias, section};
    {
        GuestBufferMemory seed(context);
        seed.SetAddressWrites(false);
        seed.AcquireRegistered();
        seed.Upload(true);
        seed.WriteBack();
    }
    if (!MirrorSourceCovers(context, address, surfaceBytes)) {
        std::cout << "no device-local mirror over the surface: lazy storage write-back not tested\n";
        return;
    }
    TextureDetiler detiler(context);
    context.detiler = &detiler;
    GuestTextureResource resource{};
    resource.baseAddress = address;
    resource.width = side;
    resource.height = side;
    resource.mipCount = 1;
    resource.tileMode = TextureTileMode::kR64KBX;
    resource.dimension = TextureDimension::k2D;
    resource.format = 56;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    Require(DescribeSurface(resource).guestBytes == surfaceBytes, "the lazy write-back surface has an unexpected size");
    {
        auto image = StorageTexture::Create(context, detiler, resource, 0);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const auto commands = recorder.Commands();
        recorder.Keep(image);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearColorValue value{{1.0f, 0.0f, 0.0f, 1.0f}};
        context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        image->MarkDirty();
        const auto before = VramLazyCounters();
        Require(StorageTexture::FlushPending(address, surfaceBytes, nullptr, "lazy storage write-back test"), "the pending storage image was not flushed");
        const auto flushed = VramLazyCounters();
        recorder.Sync();
        if (StorageTexture::LazyMirrorWriteBack()) {
            Require(flushed.downloadedPages == before.downloadedPages, "the GPU write-back into the lazy mirror downloaded pages");
            Require(flushed.pendingPages > before.pendingPages, "the GPU write-back left no GPU-owned pages for the CPU to fault in");
        } else {
            Require(flushed.pendingPages <= before.pendingPages, "the CPU write-back left GPU-owned pages");
        }
        const auto* texels = static_cast<volatile const std::uint8_t*>(alias);
        for (std::size_t i = 0; i < surfaceBytes; ++i) {
            const std::uint8_t expected = i % 4 == 0 || i % 4 == 3 ? 255 : 0;
            Require(texels[i] == expected, "guest memory does not hold the flushed storage image results");
        }
    }
    {
        auto small = resource;
        small.width = 128;
        small.height = 128;
        auto pendingImage = StorageTexture::Create(context, detiler, small, 0);
        auto overlapping = StorageTexture::Create(context, detiler, resource, 0);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const auto commands = recorder.Commands();
        recorder.Keep(pendingImage);
        recorder.Keep(overlapping);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearColorValue value{{0.0f, 1.0f, 0.0f, 1.0f}};
        context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, pendingImage->Image(), VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        pendingImage->MarkDirty();
        overlapping->Refresh();
        const bool kept = StorageTexture::FindPending(address, block) == pendingImage;
        Require(kept == StorageTexture::KeepOverlappingResults(), StorageTexture::KeepOverlappingResults() ? "refreshing an overlapping image flushed another image's pending results" : "refreshing an overlapping image left another image's results pending");
        Require(StorageTexture::FlushPending(address, surfaceBytes, nullptr, "lazy storage overlap test") == kept, "the overlap test's final flush did not match the pending state");
        recorder.Sync();
        const auto* texels = static_cast<volatile const std::uint8_t*>(alias);
        for (std::size_t i = 0; i < surfaceBytes; ++i) {
            const std::uint8_t expected = i < block ? (i % 4 == 1 || i % 4 == 3 ? 255 : 0) : (i % 4 == 0 || i % 4 == 3 ? 255 : 0);
            Require(texels[i] == expected, "guest memory does not hold the overlapped image's results");
        }
    }
    recorder.Sync();
    std::cout << "lazy storage write-back " << (StorageTexture::LazyMirrorWriteBack() ? "on the GPU" : "on the CPU") << (StorageTexture::KeepOverlappingResults() ? ", overlapping results kept" : "") << " passed\n";
#endif
}

void RunLazyMirrorLabelTests(const AgcDriver::Graphics::Context& device, AgcDriver::Graphics::Recorder& recorder) {
#ifndef _WIN32
    throw std::runtime_error("lazy mirror label fixture requires Windows guest page protection");
#else
    using namespace AgcDriver::Graphics;
    namespace Memory = AgcDriver::GuestMemory;
    constexpr std::size_t block = 65536;
    constexpr std::size_t bytes = 2 * block;
    constexpr std::size_t labelOffset = 256;
    constexpr std::size_t window = 128;
    auto context = device;
    context.hostImportAlignment = 0;
    context.dmaBufImport = false;
    auto* raw = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, block));
    auto* alias = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, block));
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
    Require(raw != nullptr && alias != nullptr && section != nullptr, "cannot create the lazy mirror label fixture");
    GuestArena::GuestArenaMap_nid_postfix(raw, bytes, section, 0, PAGE_READWRITE);
    GuestArena::GuestArenaMap_nid_postfix(alias, bytes, section, 0, PAGE_READWRITE);
    std::memset(raw, 0x11, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(raw);
    GuestAllocations::Mutation().Add(raw, bytes, true, true, true);
    struct Release {
        const Context& context;
        Recorder& recorder;
        std::uint8_t* raw;
        std::uint8_t* alias;
        HANDLE section;
        ~Release() {
            recorder.Sync();
            recorder.FlushDeferredReleases();
            ClearImageMirrors(context.device);
            ClearHostImports(context.device);
            GuestAllocations::Mutation().Remove(raw);
            for (auto* mapping : {raw, alias}) {
                GuestArena::GuestArenaReset_nid_postfix(mapping, bytes);
                GuestArena::GuestArenaRelease_nid_postfix(mapping, bytes);
            }
            CloseHandle(section);
        }
    } release{context, recorder, raw, alias, section};
    {
        GuestBufferMemory seed(context);
        seed.SetAddressWrites(false);
        seed.AcquireRegistered();
        seed.Upload(true);
        seed.WriteBack();
    }
    if (!MirrorSourceCovers(context, address, bytes)) {
        std::cout << "no device-local heap mirror: lazy mirror labels not tested\n";
        return;
    }
    const auto labelAddress = address + labelOffset;
    const auto source = MirrorSourceFor(context, labelAddress, window);
    Require(source.has_value(), "the label fixture has no device-local source");
    static_cast<void>(recorder.Commands());
    const std::uint32_t label = 0x600dcafeu;
    const auto labelBytes = std::as_bytes(std::span(&label, 1));
    const auto before = VramLazyCounters();
    const auto mirrored = StoreLabelInLazyMirror(context, recorder, labelAddress, labelBytes);
    Require((mirrored != 0) == LazyMirrorLabels(), "the label was stored in the heap mirror with the switch in the other state");
    recorder.AfterCompletions(labelAddress, labelBytes, 1, 0, false);
    if (mirrored != 0) recorder.NoteInMirrorWrite(labelAddress, labelAddress + sizeof(label), mirrored);
    alias[labelOffset + 64] = 0x5a;
    Memory::BumpCollectEpoch();
    {
        GuestBufferMemory heap(context);
        heap.SetAddressWrites(false);
        heap.AcquireRegistered();
        heap.Upload(true);
        heap.WriteBack();
    }
    const auto after = VramLazyCounters();
    if (LazyMirrorLabels()) {
        Require(after.mirrorLabels == before.mirrorLabels + 1 && after.labelRefreshesHeld > before.labelRefreshesHeld && after.labelRefreshesSynced == before.labelRefreshesSynced,
            "a whole-heap refresh over a label held in the mirror synced");
    } else {
        Require(after.mirrorLabels == before.mirrorLabels && after.labelRefreshesHeld == before.labelRefreshesHeld && after.labelRefreshesSynced == before.labelRefreshesSynced,
            "lazy mirror label counters moved with the switch off");
        std::uint32_t landed = 0;
        std::memcpy(&landed, const_cast<const std::uint8_t*>(alias) + labelOffset, sizeof(landed));
        Require(landed == label, "a whole-heap refresh over a pending label did not land it first");
    }
    auto copy = std::make_shared<Buffer>(context, window, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    CopyBuffer(context, recorder.Commands(), source->buffer, source->offset, copy->Handle(), 0, window);
    RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder.Keep(copy);
    recorder.Keep(source->lifetime);
    recorder.Sync();
    const auto held = copy->ReadBytes(0, window);
    std::uint32_t mirrorLabel = 0;
    std::memcpy(&mirrorLabel, held.data(), sizeof(mirrorLabel));
    Require(mirrorLabel == label, "the heap mirror lost the label to the refresh");
    Require(held[64] == std::byte{0x5a} && held[sizeof(label)] == std::byte{0x11}, "the heap mirror lost the CPU store beside the label");
    std::uint32_t guest = 0;
    std::memcpy(&guest, const_cast<const std::uint8_t*>(alias) + labelOffset, sizeof(guest));
    Require(guest == label, "the label did not land in guest memory at completion");
    std::cout << "lazy mirror labels " << (LazyMirrorLabels() ? "stored on the GPU" : "landed before the refresh") << " passed\n";
#endif
}

void RunLazyPathFixTests(const AgcDriver::Graphics::Context& device, AgcDriver::Graphics::Recorder& recorder) {
#ifndef _WIN32
    throw std::runtime_error("lazy path fix fixture requires Windows guest page protection");
#else
    using namespace AgcDriver::Graphics;
    namespace Memory = AgcDriver::GuestMemory;
    constexpr std::size_t block = 65536;
    constexpr std::size_t bytes = 2 * block;
    constexpr std::size_t page = 4096;
    auto context = device;
    context.hostImportAlignment = 0;
    context.dmaBufImport = false;
    auto* raw = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, block));
    auto* alias = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, block));
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
    Require(raw != nullptr && alias != nullptr && section != nullptr, "cannot create lazy path fix fixture");
    GuestArena::GuestArenaMap_nid_postfix(raw, bytes, section, 0, PAGE_READWRITE);
    GuestArena::GuestArenaMap_nid_postfix(alias, bytes, section, 0, PAGE_READWRITE);
    std::memset(raw, 0x11, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(raw);
    GuestAllocations::Mutation().Add(raw, bytes, true, true, true);
    struct Release {
        const Context& context;
        Recorder& recorder;
        std::uint8_t* raw;
        std::uint8_t* alias;
        HANDLE section;
        ~Release() {
            recorder.Sync();
            recorder.FlushDeferredReleases();
            ClearImageMirrors(context.device);
            ClearHostImports(context.device);
            GuestAllocations::Mutation().Remove(raw);
            for (auto* mapping : {raw, alias}) {
                GuestArena::GuestArenaReset_nid_postfix(mapping, bytes);
                GuestArena::GuestArenaRelease_nid_postfix(mapping, bytes);
            }
            CloseHandle(section);
        }
    } release{context, recorder, raw, alias, section};
    {
        GuestBufferMemory seed(context);
        seed.SetAddressWrites(false);
        seed.AcquireRegistered();
        seed.Upload(true);
        seed.WriteBack();
    }
    recorder.Sync();
    const auto writeOn = [&](Recorder& target, std::uint64_t at, std::uint32_t value) {
        Memory::BumpCollectEpoch();
        auto memory = std::make_shared<GuestBufferMemory>(context);
        memory->AddWritable(at, sizeof(std::uint32_t), true);
        memory->Upload(false);
        std::uint32_t adjustment = 0;
        const auto view = memory->Descriptor(at, sizeof(std::uint32_t), adjustment);
        memory->BeginGpuWrites(target);
        const auto commands = target.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, view.buffer, view.offset + adjustment, sizeof(std::uint32_t), value);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        memory->RecordCopyBacks(target);
        memory->NotePendingWrites(target);
        target.Keep(memory);
        target.OnComplete([memory] { memory->WriteBack(); });
        memory->ReadyForSubmit(target);
    };
    const auto write = [&](std::uint64_t at, std::uint32_t value) { writeOn(recorder, at, value); };
    const auto refresh = [&](std::uint64_t at, std::size_t length) {
        Memory::BumpCollectEpoch();
        GuestBufferMemory memory(context);
        memory.AddReadable(at, length);
        memory.Upload(false);
        memory.WriteBack();
    };
    const auto copyOut = [&](std::uint64_t at, std::size_t length) {
        Memory::BumpCollectEpoch();
        auto memory = std::make_shared<GuestBufferMemory>(context);
        memory->AddReadable(at, length);
        memory->Upload(false);
        memory->BeginGpuWrites(recorder);
        std::uint32_t adjustment = 0;
        const auto view = memory->Descriptor(at, length, adjustment);
        auto output = std::make_shared<Buffer>(context, length, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        CopyBuffer(context, recorder.Commands(), view.buffer, view.offset + adjustment, output->Handle(), 0, length);
        RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Keep(memory);
        recorder.Keep(output);
        recorder.OnComplete([memory] { memory->WriteBack(); });
        recorder.Sync();
        return output->ReadBytes(0, length);
    };
    const bool bits = std::getenv("APS5_LAZY_PAGE_BITS") != nullptr && std::string_view(std::getenv("APS5_LAZY_PAGE_BITS")) == "1";
    const bool window = std::getenv("APS5_LAZY_DOWNLOAD_WINDOW") != nullptr && std::string_view(std::getenv("APS5_LAZY_DOWNLOAD_WINDOW")) == "1";
    const bool shadows = std::getenv("APS5_MIRROR_PAGE_SHADOWS") != nullptr && std::string_view(std::getenv("APS5_MIRROR_PAGE_SHADOWS")) == "1";
    {
        raw[16 * page + 100] = 0x5a;
        const auto before = VramUploadCounters();
        refresh(address + 16 * page, 64);
        const auto first = VramUploadCounters();
        Require(first.bytes == before.bytes + block, "the first refresh of a written heap block did not upload the block");
        Require(first.shadowSkips == before.shadowSkips && first.shadowPartials == before.shadowPartials, "the first refresh of a block was served from page shadows");
        raw[16 * page + 100] = 0x5a;
        refresh(address + 16 * page, 64);
        const auto same = VramUploadCounters();
        if (shadows) Require(same.bytes == first.bytes && same.shadowSkips == first.shadowSkips + 1, "an identical rewrite of a shadowed block was uploaded again");
        else Require(same.bytes == first.bytes + block && same.shadowSkips == first.shadowSkips, "page shadows served a refresh while off");
        raw[17 * page + 5] = 0x77;
        refresh(address + 16 * page, 64);
        const auto changed = VramUploadCounters();
        if (shadows) Require(changed.bytes == same.bytes + page && changed.shadowPartials == same.shadowPartials + 1, "a one-page change did not upload exactly its page");
        else Require(changed.bytes == same.bytes + block && changed.shadowPartials == same.shadowPartials, "page shadows split a refresh while off");
        const auto result = copyOut(address + 16 * page, 2 * page);
        Require(result[100] == std::byte{0x5a} && result[page + 5] == std::byte{0x77}, "the GPU does not see the CPU bytes after a shadow-filtered refresh");
        Require(shadows == (changed.shadowPagesHeld > before.shadowPagesHeld), "page shadows were held while off, or none were held while on");
    }
    {
        write(address, 0x11111111u);
        recorder.Sync();
        const auto before = VramLazyCounters();
        FlushHeapMirrorWrites(address + 8 * page, page);
        const auto clean = VramLazyCounters();
        Require(clean.downloadedPages == before.downloadedPages, "a driver read of a clean range downloaded a page");
        Require(clean.bitSkips == before.bitSkips + (bits ? 1 : 0), "the page bitmap did not skip a clean range, or skipped one while off");
        FlushHeapMirrorWrites(address, page);
        const auto published = VramLazyCounters();
        Require(published.downloadedPages == clean.downloadedPages + 1, "a driver read of a lazy page did not download it");
        FlushHeapMirrorWrites(address, page);
        Require(VramLazyCounters().bitSkips == published.bitSkips + (bits ? 1 : 0), "a published page kept its lazy bit");
        Require(*reinterpret_cast<volatile std::uint32_t*>(alias) == 0x11111111u, "a driver read published the wrong bytes");
    }
    {
        write(address + page, 0xaaaa0001u);
        write(address + 2 * page, 0xaaaa0002u);
        write(address + 3 * page, 0xaaaa0003u);
        recorder.Sync();
        write(address + 4 * page, 0xaaaa0004u);
        const auto before = VramLazyCounters();
        Require(*reinterpret_cast<volatile std::uint32_t*>(alias + 2 * page) == 0xaaaa0002u, "a native read missed a lazy GPU write");
        const auto after = VramLazyCounters();
        Require(after.faults == before.faults + 1, "the native read did not fault exactly once");
        Require(after.downloadedPages == before.downloadedPages + (window ? 3 : 1), "the fault download window did not publish the completed neighbours, or published while off");
        Require(after.faultPageWaits == before.faultPageWaits, "the fault download window waited for a writer it did not need");
        Require(*reinterpret_cast<volatile std::uint32_t*>(alias + page) == 0xaaaa0001u && *reinterpret_cast<volatile std::uint32_t*>(alias + 3 * page) == 0xaaaa0003u, "neighbouring lazy pages lost their results");
        Require(VramLazyCounters().downloadedPages == before.downloadedPages + 3, "the neighbours were not downloaded exactly once in total");
        recorder.Sync();
        Require(*reinterpret_cast<volatile std::uint32_t*>(alias + 4 * page) == 0xaaaa0004u, "the in-flight page lost its result");
    }
    {
        const bool requeue = LazyCompletionRequeue();
        Recorder other(context);
        writeOn(other, address + 6 * page, 0xbbbb0006u);
        bool ran = false;
        recorder.Commands();
        recorder.OnComplete([&] {
            FlushHeapMirrorWrites(address + 6 * page, page);
            ran = true;
        });
        const auto before = VramLazyCounters();
        recorder.Sync();
        Require(!ran, "a completion write-back waited for another recorder's writer during completion");
        const auto blocked = VramLazyCounters();
        Require(blocked.requeuedCompletions == before.requeuedCompletions + (requeue ? 1 : 0), "the blocked write-back was not requeued, or was requeued while off");
        other.Sync();
        other.FlushDeferredReleases();
        recorder.Commands();
        recorder.Sync();
        Require(ran == requeue, requeue ? "the requeued write-back did not run after the writer completed" : "a dropped write-back ran while requeueing is off");
        if (requeue) Require(VramLazyCounters().downloadedPages == blocked.downloadedPages + 1, "the requeued write-back did not publish its page");
        Require(*reinterpret_cast<volatile std::uint32_t*>(alias + 6 * page) == 0xbbbb0006u, "the other recorder's lazy result was lost");
        Require(VramLazyCounters().droppedCompletions == before.droppedCompletions, "a requeued write-back was dropped");
    }
    recorder.Sync();
    std::cout << "lazy path fixes: page bits " << (bits ? "on" : "off") << ", download window " << (window ? "on" : "off") << ", completion requeue " << (LazyCompletionRequeue() ? "on" : "off") << ", page shadows " << (shadows ? "on" : "off") << " passed\n";
#endif
}

void RunKytyBufferCacheTests(const AgcDriver::Graphics::Context& device, AgcDriver::Graphics::Recorder& recorder) {
#ifndef _WIN32
    throw std::runtime_error("Kyty buffer cache fixture requires Windows guest page protection");
#else
    using namespace AgcDriver::Graphics;
    namespace Memory = AgcDriver::GuestMemory;
    constexpr std::size_t block = 65536;
    constexpr std::size_t bytes = 2 * block;
    constexpr std::size_t page = 4096;
    auto context = device;
    context.hostImportAlignment = 0;
    context.dmaBufImport = false;
    auto* raw = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, block));
    auto* alias = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, block));
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
    Require(raw != nullptr && alias != nullptr && section != nullptr, "cannot create the Kyty buffer cache fixture");
    GuestArena::GuestArenaMap_nid_postfix(raw, bytes, section, 0, PAGE_READWRITE);
    GuestArena::GuestArenaMap_nid_postfix(alias, bytes, section, 0, PAGE_READWRITE);
    std::memset(raw, 0x11, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(raw);
    GuestAllocations::Mutation().Add(raw, bytes, true, true, true);
    struct Release {
        const Context& context;
        Recorder& recorder;
        std::uint8_t* raw;
        std::uint8_t* alias;
        HANDLE section;
        ~Release() {
            recorder.Sync();
            recorder.FlushDeferredReleases();
            ClearImageMirrors(context.device);
            ClearHostImports(context.device);
            GuestAllocations::Mutation().Remove(raw);
            for (auto* mapping : {raw, alias}) {
                GuestArena::GuestArenaReset_nid_postfix(mapping, bytes);
                GuestArena::GuestArenaRelease_nid_postfix(mapping, bytes);
            }
            CloseHandle(section);
        }
    } release{context, recorder, raw, alias, section};
    const auto before = KytyBufferCache::Counters();
    {
        Memory::BumpCollectEpoch();
        auto memory = std::make_shared<GuestBufferMemory>(context);
        memory->AddWritable(address, sizeof(std::uint32_t));
        memory->Upload(false);
        std::uint32_t adjustment = 0;
        const auto view = memory->Descriptor(address, sizeof(std::uint32_t), adjustment);
        memory->BeginGpuWrites(recorder);
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, view.buffer, view.offset + adjustment, sizeof(std::uint32_t), 0x12345678u);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        memory->RecordCopyBacks(recorder);
        memory->NotePendingWrites(recorder);
        recorder.Keep(memory);
        recorder.OnComplete([memory] { memory->WriteBack(); });
        memory->ReadyForSubmit(recorder);
    }
    recorder.Sync();
    Require(*reinterpret_cast<volatile std::uint32_t*>(alias) == 0x12345678u, "a CPU read missed a GPU store through a guest buffer");
    alias[page + 8] = 0x55;
    auto output = std::make_shared<Buffer>(context, 2 * page, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    {
        Memory::BumpCollectEpoch();
        auto memory = std::make_shared<GuestBufferMemory>(context);
        memory->AddReadable(address, 2 * page);
        memory->Upload(false);
        memory->BeginGpuWrites(recorder);
        std::uint32_t adjustment = 0;
        const auto view = memory->Descriptor(address, 2 * page, adjustment);
        RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        CopyBuffer(context, recorder.Commands(), view.buffer, view.offset + adjustment, output->Handle(), 0, 2 * page);
        RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Keep(memory);
        recorder.Keep(output);
        recorder.OnComplete([memory] { memory->WriteBack(); });
    }
    recorder.Sync();
    const auto result = output->ReadBytes(0, 2 * page);
    std::uint32_t stored = 0;
    std::memcpy(&stored, result.data(), sizeof(stored));
    Require(stored == 0x12345678u && result[page + 8] == std::byte{0x55} && result[8] == std::byte{0x11}, "a GPU read missed a GPU store or a later CPU store");
    const auto after = KytyBufferCache::Counters();
    if (KytyBufferCache::Enabled()) {
        Require(after.descriptors >= before.descriptors + 2 && after.writtenDescriptors > before.writtenDescriptors && after.faults > before.faults, "the Kyty buffer cache did not serve the descriptors or the CPU read");
    } else {
        Require(after.descriptors == before.descriptors && after.faults == before.faults, "the Kyty buffer cache served descriptors with the switch off");
    }
    std::cout << "Kyty buffer cache " << (KytyBufferCache::Enabled() ? "on" : "off") << " passed\n";
#endif
}
