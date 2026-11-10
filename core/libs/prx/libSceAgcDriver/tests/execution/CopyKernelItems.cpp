// APS5_ARCHB_PIPE_COPY_KERNEL_ITEMS: queue-0 copy kernels (counted and stream) committed as pipeline
// items instead of draining the pipeline and copying inline. With queue 0 pipelined (the test
// environment sets APS5_ARCHB_PIPE_Q0_COMMITTER=1), each case checks the copied bytes and how the copy
// went: as an item without a dispatch drain (switch on), inline after a drain (switch off), and inline
// when a queued item writes the count record (both). A dispatch behind a queued copy item reads the
// copied bytes.
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/BufferCopy.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;
using namespace AgcDriver::DriverDetail;

constexpr std::uint32_t DispatchOpcode = 0x15;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Elements = 8192;

// The counted copy kernel (tests/execution/PeriodCopy.cpp) and the stream copy kernel (StreamCopy.cpp).
alignas(256) constexpr std::array<std::uint32_t, 38> CountedCode{
    0xbfa00002u, 0xd7460000u, 0x04010c0cu, 0xf4201a84u, 0xfa000000u, 0xbf8cc07fu, 0x7da8006au, 0xbf88001du,
    0xf4200204u, 0xfa000004u, 0xbf8cc07fu, 0x7e020c08u, 0x81ea0880u, 0x7e025701u, 0x100202ffu, 0x4f7ffffeu,
    0x7e020f01u, 0x7ed60501u, 0x936a6b6au, 0x9aea6b6au, 0x4a02026au, 0xd56a0001u, 0x00020101u, 0xd5690001u,
    0x00020208u, 0x4c020300u, 0x7d860208u, 0x4e040208u, 0x02020501u, 0x7d860208u, 0x4e040208u, 0x02020501u,
    0xe0002000u, 0x80000101u, 0xbf8c3f70u, 0xe0102000u, 0x80010100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 16> StreamCode{0xd7460008u, 0x04010e08u, 0x8f6a8708u, 0xd76d0009u, 0x0401806au, 0xe00c2000u, 0x80000008u, 0xe00c2000u, 0x80000409u, 0xbf8c3f71u, 0xe01c2000u, 0x80010008u, 0xbf8c3f70u, 0xe01c2000u, 0x80010409u, 0xbf810000u};
// Reads input[lane], writes output[lane] = input[lane] + 1 (tests/execution/PipelinedDispatch.cpp).
alignas(256) constexpr std::array<std::uint32_t, 8> AddCode{0x34020082u, 0xe0301000u, 0x80000201u, 0xbf8c3f70u, 0x4a060481u, 0xe0701000u, 0x80010301u, 0xbf810000u};
constexpr std::uint32_t Untouched = 0xcafebabeu;

std::uint32_t* Source = nullptr;
std::uint32_t* Destination = nullptr;
std::uint32_t* Record = nullptr;
std::uint32_t* Output = nullptr;

void check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("copy kernel items: " + what);
}

std::uint32_t* NewBlock() {
#ifdef _WIN32
    auto* block = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    auto* block = static_cast<std::uint32_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
    if (block == nullptr) throw std::runtime_error("copy kernel items: cannot allocate a guest block");
    std::memset(block, 0, BlockBytes);
    GuestAllocations::Mutation().Add(block, BlockBytes, true, true, false);
    return block;
}

template <typename Ready>
void WaitUntil(Ready ready, const std::string& what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("copy kernel items: timed out waiting for " + what);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void AppendDescriptor(std::vector<std::uint32_t>& data, const void* base, std::uint32_t stride, std::uint32_t records, std::uint32_t format) {
    const auto address = reinterpret_cast<std::uintptr_t>(base);
    data.insert(data.end(), {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), records, format});
}

void AppendDispatch(std::vector<std::uint32_t>& words, const void* code, std::uint32_t threads, const std::vector<std::uint32_t>& data, std::uint32_t groups, std::uint32_t initiator) {
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{threads, 1, 1};
    registers(0x207, shape);
    const auto address = reinterpret_cast<std::uintptr_t>(code);
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> resource{(static_cast<std::uint32_t>(data.size()) << 1u) | (1u << 7u)};
    registers(0x213, resource);
    registers(0x240, data);
    words.insert(words.end(), {0xc0031500u, groups, 1, 1, initiator});
}

void AppendCountedCopy(std::vector<std::uint32_t>& words, const void* source, const void* destination, std::uint32_t groups) {
    std::vector<std::uint32_t> data;
    AppendDescriptor(data, source, 4, Elements, 0x00014004u);
    AppendDescriptor(data, destination, 4, Elements, 0x00014004u);
    AppendDescriptor(data, Record, 16, 1, 0x0004dfacu);
    AppendDispatch(words, CountedCode.data(), 64, data, groups, 0x41u);
}

void AppendStreamCopy(std::vector<std::uint32_t>& words, std::uint32_t groups) {
    std::vector<std::uint32_t> data;
    AppendDescriptor(data, Source, 16, Elements / 4, 0x0004bfacu);
    AppendDescriptor(data, Destination, 16, Elements / 4, 0x0004bfacu);
    AppendDispatch(words, StreamCode.data(), 64, data, groups, 0x41u);
}

void AppendFill(std::vector<std::uint32_t>& words, std::uint32_t value, const void* destination, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    words.insert(words.end(), {0xc0055000u, 2u << 29u, value, 0u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), bytes});
}

void Submit(std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(sceAgcDriverSubmitDcb(&packet) == 0, "submission failed");
}

void Settle() {
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Destination), BlockBytes);
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output), BlockBytes);
}

void Reset() {
    for (std::uint32_t i = 0; i < Elements; ++i) Source[i] = i * 2654435761u + 1u;
    for (std::uint32_t i = 0; i < Elements; ++i) Destination[i] = Untouched;
}

struct Counts {
    std::uint64_t enqueued, copied, refusedRecord, drains;
    static Counts Now() {
        return {copyItemCounts[CopyItemEnqueued].load(), copyItemCounts[CopyItemCopied].load(), copyItemCounts[CopyItemRefusedRecord].load(), DrawPipeline::DrainRequestsByOpcode(DispatchOpcode)};
    }
};

void CheckCopied(std::uint32_t elements, const std::string& where) {
    for (std::uint32_t i = 0; i < Elements; ++i) {
        const auto expected = i < elements ? Source[i] : Untouched;
        check(Destination[i] == expected, where + ": element " + std::to_string(i) + " is " + std::to_string(Destination[i]) + ", expected " + std::to_string(expected));
    }
}

// A committer-blocking item, so the items behind it are still queued when queue 0 decodes the next packets.
struct Gate {
    std::atomic<bool> released{false};
    Gate() {
        WaitUntil([] { return DrawPipeline::QueuedItems() == 0; }, "the pipeline to empty");
        DrawPipeline::Queue0().Enqueue([this] {
            while (!released.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }, {});
    }
    ~Gate() { released.store(true, std::memory_order_release); }
};

}

int main() {
    // Without APS5_ARCHB_PIPE_Q0_COMMITTER=1 (the auto-registered run) queue 0 is pipelined through
    // APS5_PIPELINED_DRAWS instead, so the copies still drain (the switch needs PIPE_Q0_COMMITTER).
    const char* pipeSwitch = std::getenv("APS5_ARCHB_PIPE_Q0_COMMITTER");
    if (pipeSwitch == nullptr || pipeSwitch[0] != '1') {
#ifdef _WIN32
        _putenv_s("APS5_PIPELINED_DRAWS", "1");
#else
        setenv("APS5_PIPELINED_DRAWS", "1", 1);
#endif
    }
    try {
        {
            const auto probe = OpenVulkanTestDevice();
            if (!probe) return VulkanTestSkipped;
        }
        const char* itemsSwitch = std::getenv("APS5_ARCHB_PIPE_COPY_KERNEL_ITEMS");
        const bool items = itemsSwitch != nullptr && itemsSwitch[0] == '1' && pipeSwitch != nullptr && pipeSwitch[0] == '1';
        Shader shader{};
        shader.file_header = 0x34333231;
        shader.version = 0x18;
        shader.header_size = sizeof(Shader);
        shader.shader_size = sizeof(AddCode);
        shader.code = AddCode.data();
        AgcDriverRegisterShader_nid_postfix(&shader);
        Source = NewBlock();
        Destination = NewBlock();
        Record = NewBlock();
        Output = NewBlock();

        // 1. A counted copy with a stable count record: an item (on), inline after a drain (off).
        {
            Reset();
            Record[0] = 1000;
            Record[1] = 4096;
            const auto before = Counts::Now();
            std::vector<std::uint32_t> words;
            AppendCountedCopy(words, Source, Destination, 16);
            Submit(words);
            Settle();
            const auto after = Counts::Now();
            CheckCopied(1000, "counted copy");
            check(after.enqueued - before.enqueued == (items ? 1u : 0u) && after.copied - before.copied == (items ? 1u : 0u), "counted copy: items enqueued/copied " + std::to_string(after.enqueued - before.enqueued) + "/" + std::to_string(after.copied - before.copied));
            check(items ? after.drains == before.drains : after.drains > before.drains, "counted copy: dispatch drains " + std::to_string(after.drains - before.drains));
            std::printf("copy kernel items: counted copy, items %llu, drains %llu\n", static_cast<unsigned long long>(after.enqueued - before.enqueued), static_cast<unsigned long long>(after.drains - before.drains));
        }
        // 2. A stream copy.
        {
            Reset();
            const auto before = Counts::Now();
            std::vector<std::uint32_t> words;
            AppendStreamCopy(words, 4);
            Submit(words);
            Settle();
            const auto after = Counts::Now();
            CheckCopied(4u * 128u * 4u, "stream copy");
            check(after.copied - before.copied == (items ? 1u : 0u), "stream copy: items copied " + std::to_string(after.copied - before.copied));
            check(items ? after.drains == before.drains : after.drains > before.drains, "stream copy: dispatch drains " + std::to_string(after.drains - before.drains));
        }
        // 3. A queued fill writes the count record ({100, 100} -> {128, 128}): the copy must see 128
        // elements; queue 0 cannot read the record yet, so the copy drains and runs inline either way.
        {
            Reset();
            Record[0] = 100;
            Record[1] = 100;
            const auto before = Counts::Now();
            {
                Gate gate;
                std::vector<std::uint32_t> words;
                AppendFill(words, 128, Record, 8);
                AppendCountedCopy(words, Source, Destination, 2);
                Submit(words);
                WaitUntil([&] { return DrawPipeline::DrainRequestsByOpcode(DispatchOpcode) > before.drains || DrawPipeline::QueuedItems() >= 3; }, "the copy decoded behind the queued record fill");
            }
            Settle();
            const auto after = Counts::Now();
            CheckCopied(128, "copy behind a queued record fill");
            check(after.enqueued == before.enqueued, "copy behind a queued record fill: committed as an item");
            check(after.refusedRecord - before.refusedRecord == (items ? 1u : 0u), "copy behind a queued record fill: record refusals " + std::to_string(after.refusedRecord - before.refusedRecord));
        }
        // 4. A dispatch reading the destination of a copy still queued behind the gate sees the copy.
        {
            Reset();
            Record[0] = 32;
            Record[1] = 32;
            for (std::uint32_t lane = 0; lane < 32; ++lane) Output[lane] = 0;
            const auto before = Counts::Now();
            {
                Gate gate;
                std::vector<std::uint32_t> words;
                AppendCountedCopy(words, Source, Destination, 1);
                std::vector<std::uint32_t> data;
                const auto input = reinterpret_cast<std::uintptr_t>(Destination);
                const auto output = reinterpret_cast<std::uintptr_t>(Output);
                data.insert(data.end(), {static_cast<std::uint32_t>(input), static_cast<std::uint32_t>((input >> 32u) & 0xffffu), 128u, 0x31016facu});
                data.insert(data.end(), {static_cast<std::uint32_t>(output), static_cast<std::uint32_t>((output >> 32u) & 0xffffu), 128u, 0x31016facu});
                AppendDispatch(words, AddCode.data(), 32, data, 1, 0x8041u);
                Submit(words);
                WaitUntil([&] { return DrawPipeline::DrainRequestsByOpcode(DispatchOpcode) > before.drains || DrawPipeline::QueuedItems() >= 3; }, "the dispatch decoded behind the queued copy");
            }
            Settle();
            const auto after = Counts::Now();
            CheckCopied(32, "copy before a reading dispatch");
            for (std::uint32_t lane = 0; lane < 32; ++lane) check(Output[lane] == Source[lane] + 1u, "the dispatch behind the copy read lane " + std::to_string(lane) + " as " + std::to_string(Output[lane] - 1u) + ", expected " + std::to_string(Source[lane]));
            check(after.enqueued - before.enqueued == (items ? 1u : 0u), "copy before a reading dispatch: items " + std::to_string(after.enqueued - before.enqueued));
        }
        const char* verifySwitch = std::getenv("APS5_ARCHB_PIPE_COPY_KERNEL_ITEMS_VERIFY");
        if (items && verifySwitch != nullptr && verifySwitch[0] == '1') {
            check(copyItemCounts[CopyItemVerified].load() >= 2, "verify mode checked " + std::to_string(copyItemCounts[CopyItemVerified].load()) + " count records, expected the two counted items");
            check(copyItemCounts[CopyItemVerifyMismatch].load() == 0, "verify mode: a count record changed between queue 0 and the commit");
        }
        check(copyItemCounts[CopyItemFallback].load() == 0, "a copy item fell back to the plain copy");
        AgcDriverShutdown_nid_postfix();
        std::printf("copy kernel items tests passed (switch %s)\n", items ? "on" : "off");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
