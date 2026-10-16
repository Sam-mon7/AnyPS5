#include "BdaShader.hpp"
#include "BdaAbi.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RebarBudget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace AgcDriver::Graphics;
class Pipeline {
public:
    Pipeline(const Context& context, std::span<const std::uint32_t> code, const std::array<Buffer*, 3>& buffers) : context(context) {
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
                const VkDescriptorBufferInfo buffer{buffers[i]->Handle(), 0, buffers[i]->Bytes().size()};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = set;
                write.dstBinding = i;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &buffer;
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
    GuestArena::GuestArenaCommit_nid_postfix(raw, sparse ? block : 2 * block, PAGE_READWRITE, block);
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
    VkDeviceAddress serving = 0;
    const auto dispatch = [&](bool written, const std::function<void()>& beforeWriteBack = {}) {
        Memory::BumpCollectEpoch();
        auto memory = std::make_shared<GuestBufferMemory>(context);
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
        pipeline->Record(recorder);
        recorder.Keep(table);
        recorder.Keep(fault);
        recorder.Keep(output);
        recorder.Keep(pipeline);
        recorder.Keep(memory);
        recorder.NotePendingWrites(memory->Writes());
        CountLeaseOutcome(false, recorder.Submissions() + 1);
        if (beforeWriteBack) {
            recorder.Sync();
            beforeWriteBack();
            memory->WriteBack();
        } else recorder.OnComplete([memory] { memory->WriteBack(); });
        return output;
    };
    dispatch(true, [&] { static_cast<volatile std::uint8_t*>(raw)[8] = 0x55; });
    if (resident && RebarMirrors()) {
        if (context.RebarMemoryType(UINT32_MAX) && RebarMemory().limit >= liveBefore + 2 * block) Require(RebarMemory().live.load() >= liveBefore + 2 * block, "resident heap was not allocated in budgeted ReBAR memory");
        if (RebarMemory().limit == 0) Require(RebarMemory().overBudget.load() > fallbackBefore, "resident heap did not fall back when the ReBAR budget was full");
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
    Memory::Write(address, std::as_bytes(std::span(initial)));
    dispatch(true);
    alignas(8) std::array<std::uint32_t, 2> completion{};
    const std::array<std::uint32_t, 2> completed{0x11223344u, 0x55667788u};
    recorder.AfterCompletions(reinterpret_cast<std::uint64_t>(completion.data()), std::as_bytes(std::span(completed)), 1, 0, false);
    recorder.Sync();
    Require(completion == completed, "native CPU fixture completion label was not published");
    Require(static_cast<volatile std::uint32_t*>(raw)[0] == values[0] && static_cast<volatile std::uint32_t*>(raw)[1] == values[1], "native CPU read after a disjoint completion label missed resident GPU writes");
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
    Require(synchronousPublished, "explicit heap write-back did not publish the GPU stores");
    Require(synchronousCpuPreserved, "explicit heap write-back rolled back a CPU store after the GPU baseline");
    std::cout << (sparse ? "Sparse heap fallback tests passed\n" : "Resident heap recorder tests passed\n");
#endif
}
