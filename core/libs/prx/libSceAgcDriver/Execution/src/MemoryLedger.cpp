#include "prx/libSceAgcDriver/Execution/include/MemoryLedger.hpp"
#include <cstdlib>
#include <map>
#include <mutex>
#include <string_view>
#include <tuple>

namespace AgcDriver::MemoryLedger {

namespace {

struct Ledger {
    std::mutex mutex;
    std::uint64_t frame = 0;
    std::map<std::tuple<std::uint64_t, Kind, std::string_view>, Entry> rows;
};

Ledger& State() {
    static Ledger ledger;
    return ledger;
}

}

bool Enabled() {
    static const bool enabled = std::getenv("APS5_PROFILE_MEMORY_LEDGER") != nullptr;
    return enabled;
}

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::ImportMade: return "import_made";
    case Kind::ImportRefused: return "import_refused";
    case Kind::ImportFailed: return "import_failed";
    case Kind::ImportRetired: return "import_retired";
    case Kind::CpuRead: return "cpu_read";
    case Kind::CpuWrite: return "cpu_write";
    case Kind::GpuCopy: return "gpu_copy";
    case Kind::CopyBack: return "copy_back";
    case Kind::StagingUpload: return "staging_upload";
    case Kind::TextureUpload: return "texture_upload";
    case Kind::StorageUpload: return "storage_upload";
    case Kind::StorageWriteBack: return "storage_write_back";
    case Kind::BufferWriteBack: return "buffer_write_back";
    case Kind::MirrorCopy: return "mirror_copy";
    }
    return "unknown";
}

void Note(Kind kind, const char* reason, std::uint64_t bytes) {
    if (!Enabled()) return;
    auto& ledger = State();
    std::lock_guard lock(ledger.mutex);
    auto [it, inserted] = ledger.rows.try_emplace({ledger.frame, kind, reason}, Entry{ledger.frame, kind, reason, 0, 0});
    ++it->second.count;
    it->second.bytes += bytes;
}

void AdvanceFrame() {
    if (!Enabled()) return;
    auto& ledger = State();
    std::lock_guard lock(ledger.mutex);
    ++ledger.frame;
}

std::vector<Entry> Snapshot() {
    if (!Enabled()) return {};
    auto& ledger = State();
    std::lock_guard lock(ledger.mutex);
    std::vector<Entry> rows;
    rows.reserve(ledger.rows.size());
    for (const auto& [key, row] : ledger.rows) rows.push_back(row);
    return rows;
}

void Reset() {
    if (!Enabled()) return;
    auto& ledger = State();
    std::lock_guard lock(ledger.mutex);
    ledger.rows.clear();
    ledger.frame = 0;
}

}
