#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_KYTY_GPU_INCLUDE_KYTYGPUAPI_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_KYTY_GPU_INCLUDE_KYTYGPUAPI_HPP

#include <cstddef>
#include <cstdint>

extern "C" {

struct KytyGpuEvent {
    std::uintptr_t ident;
    std::int16_t filter;
    std::uint16_t flags;
    std::uint32_t fflags;
    std::intptr_t data;
    void* udata;
};

struct KytyGpuEventView;

struct KytyGpuEventHooks {
    void (*trigger)(void* cookie, KytyGpuEventView* view, void* triggerData);
    void (*reset)(void* cookie, KytyGpuEventView* view);
    void (*remove)(void* cookie, std::int64_t eq, KytyGpuEventView* view);
    void (*release)(void* cookie);
};

struct KytyGpuHostTable {
    void (*readEvent)(const KytyGpuEventView* view, bool* triggered, KytyGpuEvent* event);
    void (*writeEvent)(KytyGpuEventView* view, bool triggered, const KytyGpuEvent* event);
    std::size_t (*pendingCount)(const KytyGpuEventView* view);
    void (*pendingAt)(const KytyGpuEventView* view, std::size_t index, KytyGpuEvent* event);
    void (*setPending)(KytyGpuEventView* view, const KytyGpuEvent* events, std::size_t count);
    int (*addEvent)(std::int64_t eq, const KytyGpuEvent* event, bool triggered, void* cookie, const KytyGpuEventHooks* hooks, bool trigger, bool reset, bool remove);
    int (*triggerEvent)(std::int64_t eq, std::uintptr_t ident, std::int16_t filter, void* triggerData);
    int (*deleteEvent)(std::int64_t eq, std::uintptr_t ident, std::int16_t filter);
    bool (*pinEqueue)(std::int64_t eq);
    std::uint64_t (*processTime)();
    std::uint64_t (*processTimeCounter)();
    std::uint64_t (*readTsc)();
    std::uint64_t (*tscFrequency)();
    void (*titleId)(char* value, std::size_t size);
    void* (*mapAlias)(std::uintptr_t address, std::size_t bytes);
    void (*unmapAlias)(void* alias);
};

struct KytyGpuShader {
    std::uint8_t type;
    void* userData;
    void* inputSemantics;
    std::uint32_t inputSemanticCount;
    std::uint32_t codeBytes;
    std::uint32_t scratchDwords;
    std::uint64_t code;
};

using KytyGpuStartFunction = int (*)(const KytyGpuHostTable* host, void* window, std::uint32_t width, std::uint32_t height);
using KytyGpuSubmitFunction = void (*)(const std::uint32_t* commands, std::uint32_t dwords);
using KytyGpuSubmitComputeFunction = void (*)(std::uint32_t queue, const std::uint32_t* commands, std::uint32_t dwords);
using KytyGpuEqEventFunction = int (*)(std::int64_t eq, int id, void* udata);
using KytyGpuRangeFunction = void (*)(std::uint64_t address, std::uint64_t bytes);
using KytyGpuShaderFunction = void (*)(const KytyGpuShader* shader);
using KytyGpuLookupFunction = void* (*)(const char* name);
using KytyGpuHostWriteFunction = void (*)(std::uint64_t address, std::uint64_t bytes, bool begin);

}

#endif
