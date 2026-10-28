#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryLedger.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("memory ledger: " + message);
}

std::vector<AgcDriver::MemoryLedgerRow> Rows() {
    std::vector<AgcDriver::MemoryLedgerRow> rows(AgcDriverMemoryLedger_nid_postfix(nullptr, 0));
    Require(AgcDriverMemoryLedger_nid_postfix(rows.data(), rows.size()) == rows.size(), "idle snapshot size changed");
    return rows;
}

const AgcDriver::MemoryLedgerRow& Find(const std::vector<AgcDriver::MemoryLedgerRow>& rows, std::uint64_t frame, std::string_view kind, std::string_view reason) {
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
        return row.frame == frame && row.kindName == kind && row.reason == reason;
    });
    Require(found != rows.end(), "missing " + std::string(kind) + "/" + std::string(reason) + " in frame " + std::to_string(frame));
    return *found;
}

class Request final : public AgcDriver::IFlipRequest {
public:
    explicit Request(std::atomic<unsigned>& flips) : flips(flips) {}
    void GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>&) override { ++flips; }
    void Fail(std::exception_ptr) noexcept override {}
private:
    std::atomic<unsigned>& flips;
};

class Output final : public AgcDriver::IVideoOutput {
public:
    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo&) override { return std::make_shared<Request>(flips); }
    void Fail(std::exception_ptr) noexcept override {}
    std::atomic<unsigned> flips = 0;
};

void Copy() {
    alignas(256) std::array<std::uint32_t, 32> source;
    alignas(256) std::array<std::uint32_t, 32> destination{};
    source.fill(0x12345678u);
    const auto from = reinterpret_cast<std::uintptr_t>(source.data());
    const auto to = reinterpret_cast<std::uintptr_t>(destination.data());
    std::array<std::uint32_t, 7> words{0xc0055000u, 0, static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(from >> 32u), static_cast<std::uint32_t>(to), static_cast<std::uint32_t>(to >> 32u), sizeof(source)};
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "DMA submission failed");
    AgcDriverWaitIdle_nid_postfix();
    std::array<std::uint32_t, 32> read{};
    const AgcDriver::GuestMemory::ReadSiteScope site(AgcDriver::GuestMemory::ReadSite::IndexBuffer);
    AgcDriver::GuestMemory::Read(to, std::as_writable_bytes(std::span(read)));
    Require(read == source, "DMA destination differs from source");
}

void CheckCopy(const std::vector<AgcDriver::MemoryLedgerRow>& rows, std::uint64_t frame) {
    const auto& read = Find(rows, frame, "cpu_read", "index-buffer");
    Require(read.kind == static_cast<std::uint32_t>(AgcDriver::MemoryLedger::Kind::CpuRead) && read.count == 1 && read.bytes == 128, "CPU destination read was not counted exactly once");
    const auto& write = Find(rows, frame, "cpu_write", "store");
    Require(write.kind == static_cast<std::uint32_t>(AgcDriver::MemoryLedger::Kind::CpuWrite) && write.count == 1 && write.bytes == 128, "DMA CPU write was not counted exactly once");
}

void CheckChangedBytes() {
    AgcDriverResetMemoryLedger_nid_postfix();
    std::array<std::byte, 35> original{}, current{}, destination{};
    for (const auto index : {0, 2, 17, 34}) current[index] = std::byte{1};
    AgcDriver::GuestMemory::WriteChanged(reinterpret_cast<std::uintptr_t>(destination.data()), current, original);
    const auto rows = Rows();
    const auto& write = Find(rows, 0, "cpu_write", "store");
    Require(write.count == 1 && write.bytes == 4 && destination == current, "changed write did not count exactly four stored bytes");
    AgcDriverResetMemoryLedger_nid_postfix();
    AgcDriver::GuestMemory::WriteChanged(reinterpret_cast<std::uintptr_t>(destination.data()), current, current);
    Require(Rows().empty(), "unchanged write counted traffic");
}

void CheckAggregation() {
    static constexpr char first[] = "a static reason longer than forty-seven bytes for truncation";
    static constexpr char second[] = "a static reason longer than forty-seven bytes for truncation";
    using namespace AgcDriver::MemoryLedger;
    std::vector<std::thread> threads;
    for (unsigned index = 0; index < 4; ++index) threads.emplace_back([index] {
        for (unsigned count = 0; count < 100; ++count) Note(Kind::GpuCopy, index % 2 == 0 ? first : second, 8);
    });
    for (auto& thread : threads) thread.join();
    const auto rows = Rows();
    Require(rows.size() == 1 && rows[0].count == 400 && rows[0].bytes == 3200, "concurrent reasons did not aggregate by text");
    Require(rows[0].reason[47] == '\0' && std::string_view(rows[0].reason) == std::string_view(first, 47), "reason truncation was not terminated");
}

}

int main(int argc, char** argv) {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        const bool off = argc > 1 && std::string_view(argv[1]) == "--off";
        auto output = std::make_shared<Output>();
        AgcDriverRegisterVideoOutput_nid_postfix(7, output);
        AgcDriverResetMemoryLedger_nid_postfix();
        Require(Rows().empty(), "initial reset retained rows");
        Copy();
        AgcDriverSubmitFlip_nid_postfix({7, -2, 1, 0});
        AgcDriverWaitIdle_nid_postfix();
        Copy();
        Require(output->flips.load() == 1, "flip did not execute exactly once");
        const auto rows = Rows();
        if (off) Require(rows.empty(), "disabled ledger recorded traffic");
        else {
            CheckCopy(rows, 0);
            CheckCopy(rows, 1);
            std::array<AgcDriver::MemoryLedgerRow, 2> limited{};
            limited[1].count = 123;
            Require(AgcDriverMemoryLedger_nid_postfix(limited.data(), 1) == rows.size(), "limited snapshot did not return the total count");
            Require(limited[0].count == rows[0].count && limited[1].count == 123, "snapshot did not respect capacity");
            AgcDriverResetMemoryLedger_nid_postfix();
            Require(Rows().empty(), "reset retained rows");
            Copy();
            CheckCopy(Rows(), 0);
            CheckChangedBytes();
            CheckAggregation();
        }
        AgcDriverUnregisterVideoOutput_nid_postfix(7, output);
        AgcDriverShutdown_nid_postfix();
        std::cout << "memory ledger test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
