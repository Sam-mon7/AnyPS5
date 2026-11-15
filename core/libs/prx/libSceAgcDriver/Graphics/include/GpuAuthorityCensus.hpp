#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUAUTHORITYCENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUAUTHORITYCENSUS_HPP

#include <cstddef>
#include <cstdint>

namespace AgcDriver::Graphics::GpuAuthorityCensus {

enum class Kind : std::uint8_t { CopyBack, BlockPublish, HeapReadback, StorageWriteBack, DepthPlane, ShadowPublish, CopyDestination, Label, Fill, DirectShader, CpuWriteBack, Dcc, StorageShadow, Count };
struct Producer {
    std::uint32_t queue = 0xfffffffeu;
    std::uint32_t type = 0;
    std::uint64_t program = 0;
    std::uint64_t target = 0;
    bool operator==(const Producer&) const = default;
};
struct ProducerScope {
    explicit ProducerScope(std::uint32_t type, std::uint64_t program = 0);
    explicit ProducerScope(Producer source);
    ~ProducerScope();
    Producer previous;
    bool active;
};
bool Enabled();
Producer CurrentProducer();
Producer ProducerFor(std::uint64_t begin, std::uint64_t end);
void Produced(std::uint64_t begin, std::uint64_t end, std::uint64_t target = 0);
void Published(std::uint64_t begin, std::uint64_t end, Kind kind, const void* recorder = nullptr, std::uint64_t serial = 0, std::uint32_t timing = ~0u);
void StoragePublished(std::uint64_t begin, std::uint64_t end, const char* reason, const void* recorder, std::uint64_t serial, std::uint32_t timing);
void CpuRead(std::uint64_t begin, std::size_t bytes, std::uint32_t reader, std::uint64_t ticks = 0);
void GpuRead(std::uint64_t begin, std::uint64_t end, std::uint32_t reader);
void Overwritten(std::uint64_t begin, std::uint64_t end, std::uint64_t ticks = 0);
void WatchCoverage(std::uint64_t begin, std::uint64_t end, bool complete);
void GpuTime(const void* recorder, std::uint64_t serial, std::uint32_t index, std::uint64_t key, double ns, std::uint64_t bytes);
void Flip();
struct Totals {
    std::uint64_t publications = 0;
    std::uint64_t bytes = 0;
    std::uint64_t cpuBytes = 0;
    std::uint64_t gpuOnlyBytes = 0;
    std::uint64_t untouchedBytes = 0;
    std::uint64_t verifyChecks = 0;
    std::uint64_t verifyMismatches = 0;
};
Totals Snapshot();
void ResetForTest();

}

#endif
