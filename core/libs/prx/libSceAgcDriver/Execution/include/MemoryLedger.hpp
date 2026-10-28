#pragma once

#include <cstdint>
#include <vector>

namespace AgcDriver::MemoryLedger {

enum class Kind : std::uint32_t {
    ImportMade,
    ImportRefused,
    ImportFailed,
    ImportRetired,
    CpuRead,
    CpuWrite,
    GpuCopy,
    CopyBack,
    StagingUpload,
    TextureUpload,
    StorageUpload,
    StorageWriteBack,
    BufferWriteBack,
    MirrorCopy,
};

struct Entry {
    std::uint64_t frame;
    Kind kind;
    const char* reason;
    std::uint64_t count;
    std::uint64_t bytes;
};

bool Enabled();
const char* KindName(Kind kind);
void Note(Kind kind, const char* reason, std::uint64_t bytes);
void AdvanceFrame();
std::vector<Entry> Snapshot();
void Reset();

}
