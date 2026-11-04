#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LAZYPUBLISH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LAZYPUBLISH_HPP

#include <cstddef>
#include <cstdint>

namespace AgcDriver::Graphics::ArchC::LazyPublish {

bool Active();
void BeforeLabelStores();
bool DeferLabelStore(std::uint64_t address, std::size_t bytes);
void PublishAll();
std::size_t Entries();

struct Counts {
    std::uint64_t deferred = 0;
    std::uint64_t renewed = 0;
    std::uint64_t services = 0;
    std::uint64_t partial = 0;
    std::uint64_t eager = 0;
    std::uint64_t dropped = 0;
    std::uint64_t verified = 0;
    std::uint64_t newer = 0;
};
Counts Totals();

}

#endif
