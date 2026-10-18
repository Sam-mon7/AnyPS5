#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_RESOURCEPHASE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_RESOURCEPHASE_HPP

#include <cstdint>

namespace AgcDriver {

class ResourcePhase {
public:
    explicit ResourcePhase(const char* name);
    ~ResourcePhase();
    ResourcePhase(const ResourcePhase&) = delete;
    ResourcePhase& operator=(const ResourcePhase&) = delete;
    void End();

private:
    const char* name;
    std::uint64_t start = 0;
};

}

#endif
