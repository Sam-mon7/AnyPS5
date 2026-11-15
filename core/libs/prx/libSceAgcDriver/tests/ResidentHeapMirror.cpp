#include "BdaShader.hpp"
#include "BdaAbi.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BdaResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RebarBudget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <array>
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
}

void RunResidentHeapMirrorTests(const AgcDriver::Graphics::Context& device, AgcDriver::Graphics::Recorder& recorder, bool sparse) {
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
    if (VramWritable() && RebarMemory().limit == 0) {
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
    const auto dispatch = [&](bool written, const std::function<void()>& beforeWriteBack = {}) {
        Memory::BumpCollectEpoch();
        auto memory = std::make_shared<GuestBufferMemory>(context);
        memory->SetAddressWrites(written);
        memory->AcquireRegistered();
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
    dispatch(true, [&] { static_cast<volatile std::uint8_t*>(raw)[8] = 0x55; });
    const auto* vramValue = std::getenv("APS5_VRAM_MIRRORS");
    const bool vramMirrors = vramValue != nullptr && std::strcmp(vramValue, "1") == 0;
    if (resident && RebarMirrors()) {
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
            const auto overflowBefore = VramWritableCounters();
            const auto overflow = dispatch(base, true, false, true);
            recorder.Sync();
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
    std::cout << "VRAM writable eager coherence tests passed\n";
#endif
}
