#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Lanes/SubmissionCursor.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

alignas(64) std::array<std::uint32_t, 16> labels{};

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t Address(std::size_t index) { return reinterpret_cast<std::uintptr_t>(&labels[index]); }
std::uint32_t Read(std::size_t index) { return *reinterpret_cast<const volatile std::uint32_t*>(&labels[index]); }

void Write(std::vector<std::uint32_t>& words, std::size_t index, std::uint32_t value) {
    const auto address = Address(index);
    words.insert(words.end(), {0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), value});
}

void Wait(std::vector<std::uint32_t>& words, std::size_t index, std::uint32_t value) {
    const auto address = Address(index);
    words.insert(words.end(), {0xc0053c00u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), value, 0xffffffffu, 0x19u});
}

void Submit(std::uint32_t queue, const std::vector<std::uint32_t>& words) {
    Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require((queue == 0 ? sceAgcDriverSubmitDcb(&packet) : sceAgcDriverSubmitAcb(queue, &packet)) == 0, "PM4 submission failed");
}

void Await(std::size_t index, std::uint32_t value) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (Read(index) != value) {
        Require(std::chrono::steady_clock::now() < deadline, "PM4 cursor did not resume");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}

int main() {
    try {
        AgcDriver::DriverDetail::Lanes::SubmissionCursor chunks;
        for (std::size_t index = 0; index < 129; ++index) chunks.submission.commands.insert(chunks.submission.commands.end(), {0xc0022d00u, 0, 0, 0});
        Require(chunks.End() == 128 * 4, "draw chunk did not stop at 128 draws");
        chunks.cursor = chunks.End();
        Require(chunks.End() == 129 * 4, "last draw chunk was lost");
        std::vector<std::uint32_t> consumer;
        Write(consumer, 0, 1);
        Wait(consumer, 1, 1);
        Write(consumer, 2, 1);
        Submit(0, consumer);
        std::vector<std::uint32_t> queued;
        const auto address = Address(4);
        queued.insert(queued.end(), {0xc0079300u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1u, 2u, 0xffffffffu, 0xffffffffu, 0x19u});
        Write(queued, 3, 1);
        Submit(0, queued);
        std::vector<std::uint32_t> producer;
        Write(producer, 1, 1);
        Write(producer, 4, 1);
        Write(producer, 5, 2);
        Wait(producer, 2, 1);
        Write(producer, 6, 1);
        Submit(0x20, producer);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (Read(3) != 1 || Read(6) != 1) {
            Require(std::chrono::steady_clock::now() < deadline, "cross-queue WAIT_REG_MEM did not resume");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        AgcDriverWaitIdle_nid_postfix();
        Require(Read(0) == 1 && Read(2) == 1, "same-queue chunks were reordered");
        const auto condition = Address(8);
        std::vector<std::uint32_t> conditional{0xc0032200u, static_cast<std::uint32_t>(condition), static_cast<std::uint32_t>(condition >> 32u), 0, 7};
        Wait(conditional, 9, 1);
        Write(conditional, 10, 1);
        Submit(0, conditional);
        Await(10, 1);
        Require(Read(9) == 0, "conditional wait label unexpectedly changed");
        std::vector<std::uint32_t> rewind;
        Write(rewind, 11, 1);
        rewind.insert(rewind.end(), {0xc0005900u, 0});
        Write(rewind, 12, 1);
        Submit(0, rewind);
        Await(11, 1);
        Require(Read(12) == 0, "REWIND executed its unreleased tail");
        const auto control = reinterpret_cast<std::uintptr_t>(&rewind[6]);
        const std::vector<std::uint32_t> release{0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(control), static_cast<std::uint32_t>(control >> 32u), 0x80000000u};
        Submit(0x21, release);
        Await(12, 1);
        AgcDriverWaitIdle_nid_postfix();
        AgcDriverShutdown_nid_postfix();
        std::cout << "GPU lanes PM4 wait/FIFO test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
