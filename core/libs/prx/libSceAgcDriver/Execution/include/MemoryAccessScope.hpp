#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MEMORYACCESSSCOPE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MEMORYACCESSSCOPE_HPP

#include "prx/libc/include/HostThreadSlot.hpp"
#include <cstddef>
#include <cstdint>

namespace AgcDriver::GuestMemory {

class MemoryAccessScope {
public:
    using Resolver = void (*)(void*, std::uint64_t, std::size_t, bool);
    MemoryAccessScope(void* context, Resolver resolver) : previousContext(ContextSlot::Get()), previousResolver(ResolverSlot::Get()) {
        ContextSlot::Set(context);
        ResolverSlot::Set(resolver);
    }
    ~MemoryAccessScope() {
        ContextSlot::Set(previousContext);
        ResolverSlot::Set(previousResolver);
    }
    MemoryAccessScope(const MemoryAccessScope&) = delete;
    MemoryAccessScope& operator=(const MemoryAccessScope&) = delete;
    static void Resolve(std::uint64_t address, std::size_t bytes, bool writable) {
        const auto resolver = ResolverSlot::Get();
        const auto context = ContextSlot::Get();
        if (resolver == nullptr || bytes == 0) return;
        const MemoryAccessScope suspended(nullptr, nullptr);
        resolver(context, address, bytes, writable);
    }

private:
    struct ContextTag {};
    struct ResolverTag {};
    using ContextSlot = HostThreadSlot<void*, ContextTag>;
    using ResolverSlot = HostThreadSlot<Resolver, ResolverTag>;
    void* previousContext;
    Resolver previousResolver;
};

}

#endif
