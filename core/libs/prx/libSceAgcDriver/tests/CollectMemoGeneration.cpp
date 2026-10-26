#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t Block = 65536;
constexpr std::uint64_t Page = 4096;
constexpr std::uint64_t Base = std::uint64_t{1} << 32;
constexpr std::chrono::seconds Patience{10};

struct FakeWatch {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> written;
    bool holdNextWalk = false;
    bool holding = false;
    bool released = false;
    bool timedOut = false;
    bool failCollect = false;
    std::uint64_t walks = 0;
};

FakeWatch& Fake() {
    static FakeWatch fake;
    return fake;
}

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

}

namespace GuestWriteWatch {

extern "C" {

bool GuestWriteWatchAvailable_nid_postfix() {
    return true;
}

bool GuestWriteWatchCovers_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    return address >= Base && bytes <= 2 * Block && address - Base <= 2 * Block - bytes;
}

bool GuestWriteWatchCollect_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
    auto& fake = Fake();
    const auto end = address + bytes;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> runs;
    {
        std::unique_lock lock(fake.mutex);
        ++fake.walks;
        if (fake.failCollect) return false;
        if (fake.holdNextWalk) {
            fake.holdNextWalk = false;
            fake.holding = true;
            fake.changed.notify_all();
            fake.timedOut = !fake.changed.wait_for(lock, Patience, [&] { return fake.released; });
        }
        for (auto it = fake.written.begin(); it != fake.written.end();) {
            if (it->first < end && address < it->second) {
                runs.emplace_back(std::max(it->first, address), std::min(it->second, end));
                it = fake.written.erase(it);
            } else ++it;
        }
    }
    for (const auto& [from, to] : runs) written(context, from, to);
    return true;
}

}

}

namespace {

using namespace AgcDriver::GuestMemory;

void CheckFailedCollectMemo() {
    auto& fake = Fake();
    const bool enabled = std::getenv("APS5_NO_FAILED_COLLECT_MEMO") == nullptr && std::getenv("APS5_NO_COLLECT_MEMO") == nullptr && std::getenv("APS5_NO_EPOCH_COLLECT_MAP") == nullptr && std::getenv("APS5_SHARED_COLLECT_MEMO") == nullptr;
    fake.failCollect = true;
    BumpCollectEpoch();
    const auto before = fake.walks;
    Require(CollectWrites(Base, 2 * Block) == 0 && CollectWrites(Base, 2 * Block) == 0, "a failed collection manufactured a generation");
    Require(fake.walks - before == (enabled ? 1u : 2u), "a repeated failed collection did not follow its memo switch");
    auto walks = fake.walks;
    Require(CollectWritesUncached(Base, 2 * Block) == 0 && fake.walks == walks + 1, "an uncached failed collection skipped the watch");
    walks = fake.walks;
    BumpCollectEpoch();
    Require(CollectWrites(Base, 2 * Block) == 0 && fake.walks == walks + 1, "a failed collection survived an epoch change");
    walks = fake.walks;
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(Base, 2 * Block);
    Require(CollectWrites(Base, 2 * Block) == 0 && fake.walks == walks + 1, "a failed collection survived a mapping change");
    bool otherRetried = false;
    walks = fake.walks;
    std::thread other([&] {
        BumpCollectEpoch();
        otherRetried = CollectWrites(Base, 2 * Block) == 0;
    });
    other.join();
    Require(otherRetried && fake.walks == walks + 1, "another thread reused a failed collection");
    fake.failCollect = false;
    Require(CollectWrites(Base, Block) != 0, "a failed whole-range collection poisoned its watched subset");
    Require(CollectWritesUncached(Base, 2 * Block) != 0 && CollectWrites(Base, 2 * Block) != 0, "a failed collection hid a later completed walk");
    BumpCollectEpoch();
    Require(CollectWrites(Base, 2 * Block) != 0, "a recovered collection did not retry in a new epoch");
    bool unbumpedRetried = false;
    std::thread unbumped([&] {
        fake.failCollect = true;
        const auto start = fake.walks;
        unbumpedRetried = CollectWrites(Base, 2 * Block) == 0 && CollectWrites(Base, 2 * Block) == 0 && fake.walks == start + 2;
    });
    unbumped.join();
    Require(unbumpedRetried, "a thread without an epoch reused a failed collection");
    fake.failCollect = false;
}

void CheckMemoHitDuringWalk() {
    auto& fake = Fake();
    BumpCollectEpoch();
    const auto collected = CollectWrites(Base, Block);
    Require(collected != 0 && UnchangedSince(Base, Block, collected), "the block was not collected");
    {
        const std::lock_guard lock(fake.mutex);
        fake.written.emplace_back(Base + Page, Base + 2 * Page);
        fake.holdNextWalk = true;
    }
    std::thread walker([] { CollectWritesUncached(Base, 2 * Block); });
    bool holding = false;
    {
        std::unique_lock lock(fake.mutex);
        holding = fake.changed.wait_for(lock, Patience, [&] { return fake.holding; });
    }
    const auto memoized = holding ? CollectWrites(Base, Block) : 0;
    {
        const std::lock_guard lock(fake.mutex);
        fake.released = true;
    }
    fake.changed.notify_all();
    walker.join();
    Require(holding, "the second walk did not reach the write watch");
    Require(!fake.timedOut, "the memoized collect waited for the walk in progress");
    Require(!UnchangedSince(Base + Page, Page, memoized), "a memoized collect returned the generation a walk in progress then stamped a write with");
    BumpCollectEpoch();
    Require(CollectWrites(Base, Block) > memoized && !UnchangedSince(Base, Block, memoized), "a later epoch trusts the write stamped by the walk in progress");
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--failed-collect-memo") {
            CheckFailedCollectMemo();
            return 0;
        }
        CheckMemoHitDuringWalk();
    } catch (const std::exception& error) {
        std::cerr << "collect memo generation test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "collect memo generation test passed\n";
    return 0;
}
