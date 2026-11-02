#include "prx/libSceAgcDriver/Graphics/include/CallableRef.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

namespace {

std::size_t allocations = 0;

using AgcDriver::Graphics::CallableRef;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::shared_ptr<int> callThrough(const CallableRef<std::shared_ptr<int>()>& lookup) {
    return lookup();
}

int sumThrough(const CallableRef<int(int, const std::array<int, 4>&)>& add, int base, const std::array<int, 4>& values) {
    return add(base, values);
}

void callsTheReferencedCallable() {
    auto stored = std::make_shared<int>(7);
    std::uint64_t a = 1;
    std::uint64_t b = 2;
    std::uint64_t c = 3;
    std::size_t calls = 0;
    const auto lookup = [&] {
        ++calls;
        *stored += static_cast<int>(a + b + c);
        return stored;
    };
    check(sizeof(lookup) > 16, "the lookup does not exceed the small-object buffer it is meant to model");
    const auto first = callThrough(CallableRef<std::shared_ptr<int>()>(lookup));
    check(first == stored && *first == 13 && calls == 1, "the referenced callable did not run once with its captures");
    a = 10;
    const auto second = callThrough(CallableRef<std::shared_ptr<int>()>(lookup));
    check(second == stored && *second == 28 && calls == 2, "the reference copied the captures instead of referring to them");
    const auto add = [&](int base, const std::array<int, 4>& values) { return base + values[0] + values[1] + values[2] + values[3] + static_cast<int>(calls); };
    check(sumThrough(CallableRef<int(int, const std::array<int, 4>&)>(add), 100, {1, 2, 3, 4}) == 112, "arguments were not passed through");
}

void propagatesExceptions() {
    const auto failing = []() -> std::shared_ptr<int> { throw std::runtime_error("lookup failed"); };
    bool caught = false;
    try {
        callThrough(CallableRef<std::shared_ptr<int>()>(failing));
    } catch (const std::runtime_error& error) {
        caught = std::string(error.what()) == "lookup failed";
    }
    check(caught, "an exception of the callable did not reach the caller");
}

void allocatesNothing() {
    auto stored = std::make_shared<int>(1);
    std::uint64_t a = 1;
    std::uint64_t b = 2;
    std::uint64_t c = 3;
    const auto lookup = [&] {
        *stored += static_cast<int>(a + b + c);
        return stored;
    };
    auto before = allocations;
    for (int round = 0; round < 64; ++round) callThrough(CallableRef<std::shared_ptr<int>()>(lookup));
    check(allocations == before, "calling through a reference allocated");
    before = allocations;
    for (int round = 0; round < 64; ++round) {
        const std::function<std::shared_ptr<int>()> owned = lookup;
        callThrough(CallableRef<std::shared_ptr<int>()>(owned));
    }
    check(allocations >= before + 64, "a std::function of the same callable did not allocate, so the reference saves nothing");
    check(*stored == 1 + 128 * 6, "not every call ran");
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
        callsTheReferencedCallable();
        propagatesExceptions();
        allocatesNothing();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "callable ref test failed: %s\n", error.what());
        return 1;
    }
    std::puts("callable ref tests passed");
    return 0;
}
