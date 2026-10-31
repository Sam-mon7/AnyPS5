#include "prx/libSceAgcDriver/Execution/include/EpochMemoMap.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>

namespace {

std::size_t allocations = 0;

// The shape of GuestMemory.cpp's WriteTracker::Memo.
struct Memo {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uint64_t epoch = 0;
    std::uint64_t unwatched = 0;
    std::uint64_t mapping = 0;
};

using Map = AgcDriver::GuestMemory::EpochMemoMap<Memo>;
using State = Map::SlotState;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// No std::string on the measured paths: building one may allocate.
State insert(Map& map, std::uintptr_t key, std::uint64_t epoch) {
    State state;
    auto& entry = map.Slot(key, state);
    if (state != State::Current) check(entry.epoch == 0 && entry.end == 0, "a slot to fill was not value-initialized");
    entry = {key, key + 0x1000, epoch, 7, 9};
    return state;
}

// Off (the original rule): every epoch change frees all entries.
void dropsWithoutKeep() {
    Map map;
    map.Enter(1, false);
    check(insert(map, 0x1000, 1) == State::Inserted, "first insert");
    check(insert(map, 0x1000, 1) == State::Current, "an entry of this epoch was not current");
    map.Enter(1, false);
    check(map.Size() == 1 && map.Find(0x1000) != nullptr, "re-entering the same epoch dropped its entries");
    map.Enter(2, false);
    check(map.Size() == 0 && map.Pooled() == 0 && map.Find(0x1000) == nullptr, "an epoch change kept entries without keep");
    check(insert(map, 0x1000, 2) == State::Inserted, "an insert after the drop reused a node");
}

// Keep: no entry of an earlier epoch is ever found or handed out as current, even when every other
// field (range, serials) would still match; its node is only recycled, value-initialized.
void staleNeverValid() {
    Map map;
    map.Enter(10, true);
    check(insert(map, 0x2000, 10) == State::Inserted, "first insert");
    check(insert(map, 0x3000, 10) == State::Inserted, "second insert");
    const auto* found = map.Find(0x2000);
    check(found != nullptr && found->epoch == 10, "a current entry was not found");
    map.Enter(11, true);
    check(map.Size() == 0 && map.Pooled() == 2, "keep did not pool the nodes");
    check(map.Find(0x2000) == nullptr && map.Find(0x3000) == nullptr, "an entry of an earlier epoch was found");
    check(insert(map, 0x2000, 11) == State::Reused, "the same key did not take a pooled node");
    check(insert(map, 0x9000, 11) == State::Reused, "a new key did not take a pooled node");
    check(insert(map, 0xa000, 11) == State::Inserted, "an empty pool did not insert");
    found = map.Find(0x9000);
    check(found != nullptr && found->epoch == 11 && found->end == 0xa000, "a reused node did not hold its new entry");
    check(map.Find(0x3000) == nullptr, "a key not made in this epoch was found");
    check(insert(map, 0x2000, 11) == State::Current, "an entry of this epoch was not current");
    map.Enter(12, true);
    map.Enter(20, true);
    check(map.Find(0x2000) == nullptr && map.Pooled() == 3, "entries survived several epochs");
    // Turning keep off frees the pool too.
    map.Enter(21, false);
    check(map.Size() == 0 && map.Pooled() == 0, "turning keep off did not free the pooled nodes");
}

// The size bound: the pool keeps at most cap nodes; map and pool hold at most cap plus one epoch's
// inserts.
void capRespected() {
    constexpr std::size_t cap = 8;
    Map map;
    map.Enter(1, true, cap);
    for (std::uintptr_t key = 0; key < 3 * cap; ++key) insert(map, key * 0x1000, 1);
    map.Enter(2, true, cap);
    check(map.Size() == 0 && map.Pooled() == cap, "the pool kept more than the cap");
    constexpr std::size_t perEpoch = 5;
    std::uintptr_t next = 0x100000;
    for (std::uint64_t epoch = 3; epoch < 1000; ++epoch) {
        map.Enter(epoch, true, cap);
        check(map.Size() == 0 && map.Pooled() <= cap, "an epoch began above the cap");
        const auto inserts = epoch % 3 == 0 ? 2 * cap : perEpoch;
        for (std::size_t i = 0; i < inserts; ++i, next += 0x1000) insert(map, next, epoch);
        check(map.Size() + map.Pooled() <= cap + inserts, "the map grew past the cap plus one epoch's inserts");
    }
    static_assert(Map::KeepCap == 4096, "the documented cap");
}

// The point of keep: later epochs allocate no nodes, for the same ranges or for new ones.
void keepReusesNodes() {
    Map map;
    map.Enter(1, true);
    for (std::uintptr_t key = 0; key < 64; ++key) insert(map, key * 0x1000, 1);
    std::uintptr_t moving = 0x1000000;
    for (std::uint64_t epoch = 2; epoch < 10; ++epoch) {
        map.Enter(epoch, true);
        const auto before = allocations;
        std::size_t reused = 0;
        for (std::uintptr_t key = 0; key < 32; ++key) reused += insert(map, key * 0x1000, epoch) == State::Reused ? 1 : 0;
        for (std::uintptr_t key = 0; key < 32; ++key, moving += 0x1000) reused += insert(map, moving, epoch) == State::Reused ? 1 : 0;
        const auto allocated = allocations - before;
        check(reused == 64, "pooled nodes were not reused");
        check(allocated == 0, "reusing pooled nodes allocated");
    }
    Map dropping;
    dropping.Enter(1, false);
    for (std::uintptr_t key = 0; key < 64; ++key) insert(dropping, key * 0x1000, 1);
    dropping.Enter(2, false);
    const auto before = allocations;
    for (std::uintptr_t key = 0; key < 64; ++key) insert(dropping, key * 0x1000, 2);
    check(allocations >= before + 64, "the original rule did not allocate a node per entry");
}

}

void* operator new(std::size_t size) {
    ++allocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    try {
        dropsWithoutKeep();
        staleNeverValid();
        capRespected();
        keepReusesNodes();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "epoch memo map test failed: %s\n", error.what());
        return 1;
    }
    std::puts("epoch memo map tests passed");
    return 0;
}
