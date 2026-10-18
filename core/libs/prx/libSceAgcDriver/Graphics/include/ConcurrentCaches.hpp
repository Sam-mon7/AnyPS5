#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_CONCURRENTCACHES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_CONCURRENTCACHES_HPP

#include <cstdlib>
#include <cstring>

namespace AgcDriver::Graphics {

inline bool ConcurrentCachesEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_CONCURRENT_CACHES");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

}

#endif
