#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
constexpr std::uint32_t Side = 256;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t Page = 0x1000;
constexpr std::size_t Block = 0x10000;
constexpr std::size_t SurfaceBytes = 4 * Block;
constexpr std::uint32_t Untouched = 0xdeadbeefu;
constexpr std::uint32_t Stored = 0x12345678u;
constexpr std::uint32_t Filled = 0x9abcdef0u;

alignas(256) constexpr std::array<std::uint32_t, 7> StoreCode{
    // x = local thread id, y = workgroup x (s16 after the 16 user SGPRs).
    // Write every texel, so all four tracked units have a real GPU result to preserve.
    0x7e040300, 0x7e060210, 0x7e0202ff, Stored, 0xf0200108, 0x00020102, 0xbf810000,
};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint32_t* texels, const std::uint8_t* keys = nullptr) {
    const auto address = AddressOf(texels);
    const auto meta = AddressOf(keys) >> 8u;
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (TileR64KBX << 20u) | (Type2D << 28u),
        0u,
        0u,
        keys == nullptr ? 0u : (1u << 20u) | (1u << 21u) | (static_cast<std::uint32_t>(meta & 0xffu) << 24u),
        static_cast<std::uint32_t>(meta >> 8u),
    };
}

void Dispatch(std::span<const std::uint32_t> code, std::uint32_t threads, std::uint32_t resource, std::span<const std::uint32_t> userData, std::uint32_t groups) {
    std::vector<std::uint32_t> words;
    const auto registers = [&](const std::uint32_t first, const std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{threads, 1, 1};
    registers(0x207, shape);
    const auto address = AddressOf(code.data());
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> layout{resource};
    registers(0x213, layout);
    registers(0x240, userData);
    words.insert(words.end(), {0xc0031500u, groups, 1, 1, 0x41u});
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
}

void Store(const std::uint32_t* texels, const std::uint8_t* keys = nullptr) {
    std::array<std::uint32_t, 16> userData{};
    const auto texture = TextureDescriptor(texels, keys);
    std::copy(texture.begin(), texture.end(), userData.begin() + 8);
    Dispatch(StoreCode, Side, (static_cast<std::uint32_t>(userData.size()) << 1u) | 0x80u, userData, Side);
}

class Surface {
public:
    explicit Surface(bool watched = false) : watched(watched) {
        if (watched) {
#ifdef _WIN32
            texels = static_cast<std::uint32_t*>(GuestArena::GuestArenaAllocate_nid_postfix(SurfaceBytes, Block));
            Require(texels != nullptr, "store over untracked: cannot reserve the watched surface");
            // A shared guest mapping imports through an alias, retaining the guest write watch.
            const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, SurfaceBytes, nullptr);
            Require(section != nullptr, "store over untracked: cannot create shared test memory");
            try {
                GuestArena::GuestArenaMap_nid_postfix(texels, SurfaceBytes, section, 0, PAGE_READWRITE);
            } catch (...) {
                CloseHandle(section);
                GuestArena::GuestArenaRelease_nid_postfix(texels, SurfaceBytes);
                throw;
            }
            CloseHandle(section);
#else
            void* mapped = mmap(nullptr, SurfaceBytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mapped != MAP_FAILED) {
                const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
                const auto aligned = (begin + Block - 1) & ~std::uintptr_t{Block - 1};
                if (aligned != begin) munmap(mapped, aligned - begin);
                if (aligned != begin + Block) munmap(reinterpret_cast<void*>(aligned + SurfaceBytes), begin + Block - aligned);
                texels = reinterpret_cast<std::uint32_t*>(aligned);
                GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(texels, SurfaceBytes);
            }
#endif
        } else {
#ifdef _WIN32
            texels = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, SurfaceBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
            storage.resize((SurfaceBytes + Block) / 4u);
            texels = reinterpret_cast<std::uint32_t*>((reinterpret_cast<std::uintptr_t>(storage.data()) + Block - 1) & ~std::uintptr_t{Block - 1});
#endif
        }
        Require(texels != nullptr, "store over untracked: cannot allocate the guest surface");
        std::fill(texels, texels + SurfaceBytes / 4u, Untouched);
        GuestAllocations::Mutation().Add(texels, SurfaceBytes, true, true, true);
    }

    ~Surface() {
        GuestAllocations::Mutation().Remove(texels);
        if (!watched) {
#ifdef _WIN32
            VirtualFree(texels, 0, MEM_RELEASE);
#endif
            return;
        }
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(texels, SurfaceBytes);
        GuestArena::GuestArenaRelease_nid_postfix(texels, SurfaceBytes);
#else
        munmap(texels, SurfaceBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(texels, SurfaceBytes);
#endif
    }

    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    std::uint32_t* Texels() const { return texels; }
    std::uint8_t* Bytes() const { return reinterpret_cast<std::uint8_t*>(texels); }
    std::uint64_t Address() const { return AddressOf(texels); }

private:
    bool watched;
    std::vector<std::uint32_t> storage;
    std::uint32_t* texels = nullptr;
};

void Fill(const Surface& surface) {
    const auto image = StorageTexture::FindLive(surface.Address(), SurfaceBytes);
    Require(image != nullptr, "store over untracked: no storage image to clear");
    const std::array<std::uint32_t, 4> pattern{Filled, Filled, Filled, Filled};
    const char* refusal = nullptr;
    Require(image->FillClear(pattern, StorageTexture::WholeImage, refusal), "store over untracked: the integer image clear was refused");
}

std::uint8_t Pattern(std::size_t offset) {
    return static_cast<std::uint8_t>(0x40u + offset % 0x3bu);
}

std::size_t LostBytes(const std::uint8_t* bytes) {
    std::size_t lost = 0;
    for (std::size_t offset = 0; offset < Page; ++offset) {
        if (static_cast<const volatile std::uint8_t*>(bytes)[offset] != Pattern(offset)) ++lost;
    }
    return lost;
}

bool Holds(const std::uint32_t* texels, std::uint32_t value) {
    const auto* words = static_cast<const volatile std::uint32_t*>(texels);
    for (std::size_t index = 0; index < SurfaceBytes / 4u; ++index) {
        if (words[index] == value) return true;
    }
    return false;
}

bool Pending(const Surface& surface, std::size_t offset = 0, std::size_t bytes = SurfaceBytes) {
    if (offset != 0 || bytes != SurfaceBytes) {
        StorageTexture::AccessClassification classification;
        StorageTexture::ClassifyAccess(surface.Address() + offset, bytes, classification);
        return classification.images != 0;
    }
    const std::array range{std::pair{surface.Address() + offset, surface.Address() + offset + bytes}};
    return StorageTexture::AnyPendingOverlaps(range);
}

void ExpectBlock(const Surface& surface, std::size_t unit, std::uint32_t value) {
    const auto* words = static_cast<const volatile std::uint32_t*>(surface.Texels()) + unit * Block / 4;
    for (std::size_t i = 0; i < Block / 4; ++i) {
        const auto actual = words[i];
        if (actual == value) continue;
        char message[192];
        std::snprintf(message, sizeof(message), "store over untracked: unit %zu word %zu holds %08x, expected GPU result %08x", unit, i, actual, value);
        Require(false, message);
    }
}

template <typename THold>
bool HoldPending(THold& hold, const Surface& surface, std::uint32_t result) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true) {
        hold.lock();
        if (Pending(surface)) return true;
        hold.unlock();
        if (Holds(surface.Texels(), result)) return false;
        Require(std::chrono::steady_clock::now() < deadline, "store over untracked: the result neither became pending nor reached guest memory");
        std::this_thread::yield();
    }
}

template <typename THold>
void Settle(THold& hold, const Surface& surface, std::uint32_t result) {
    if (!HoldPending(hold, surface, result)) hold.lock();
    StorageTexture::FlushAllPending("store over untracked fixture");
    std::vector<std::byte> bytes(SurfaceBytes);
    AgcDriver::GuestMemory::Read(surface.Address(), bytes);
    hold.unlock();
}

enum class Kind { ReadAfter, ReadBefore, Discard, Filled, Copied, PartialWatchSaved, LeftWatch, PartialLeftWatch, ShadowGap, SingleReadBefore };

struct Case {
    const char* name;
    Kind kind;
    std::size_t access;
};

enum class Outcome { Kept, Lost, Missed, Skipped };

Outcome Run(const Case& test, std::vector<std::unique_ptr<Surface>>& surfaces) {
    const bool watched = test.kind == Kind::LeftWatch || test.kind == Kind::PartialLeftWatch || test.kind == Kind::PartialWatchSaved || test.kind == Kind::ShadowGap;
    if (watched && !AgcDriver::GuestMemory::WriteWatched()) {
        std::fprintf(stderr, "[store over untracked] %s: skipped, write watching is unavailable\n", test.name);
        return Outcome::Skipped;
    }
    Surface* source = nullptr;
    if (test.kind == Kind::Copied) source = surfaces.emplace_back(std::make_unique<Surface>()).get();
    Surface* singleKeys = nullptr;
    if (test.kind == Kind::SingleReadBefore) singleKeys = surfaces.emplace_back(std::make_unique<Surface>()).get();
    auto& surface = *surfaces.emplace_back(std::make_unique<Surface>(watched));
    if (singleKeys != nullptr) {
        std::fill_n(singleKeys->Bytes(), SurfaceBytes / 256, 0x10);
        for (std::size_t word = 0; word < SurfaceBytes / 4; ++word) surface.Texels()[word] = 0x01000000u + static_cast<std::uint32_t>(word);
    }
    // Main retains every allocation through driver shutdown, including failed attempts. The hold
    // below ends first; cleanup drops any deliberately refused results without freeing GPU inputs.
    struct Discard {
        const Surface& surface;
        const Surface* source;
        ~Discard() {
            try {
                std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
                static_cast<void>(StorageTexture::DiscardPendingInside(surface.Address(), SurfaceBytes));
                if (source) static_cast<void>(StorageTexture::DiscardPendingInside(source->Address(), SurfaceBytes));
            } catch (...) {}
        }
    } discard{surface, source};
    auto* bytes = surface.Bytes();
    const auto address = surface.Address();
    std::unique_lock hold(AgcDriver::GuestMemory::GpuMutex(), std::defer_lock);
    if (test.kind == Kind::Copied) {
        Store(source->Texels());
        Settle(hold, *source, Stored);
        hold.lock();
        Fill(*source);
        hold.unlock();
        Settle(hold, *source, Filled);
        ExpectBlock(*source, 3, Filled);
    }
    Store(surface.Texels(), singleKeys == nullptr ? nullptr : singleKeys->Bytes());
    if (test.kind == Kind::Filled || test.kind == Kind::PartialWatchSaved) {
        Settle(hold, surface, Stored);
        hold.lock();
        if (test.kind == Kind::PartialWatchSaved) {
            if (!AgcDriver::GuestMemory::Watched(address, SurfaceBytes)) {
                std::fprintf(stderr, "[store over untracked] %s: skipped, the import did not retain write watching\n", test.name);
                return Outcome::Skipped;
            }
            AgcDriver::GuestMemory::Unwatch(address, Block);
            Require(!AgcDriver::GuestMemory::Watched(address, Block) && AgcDriver::GuestMemory::Watched(address + Block, 3 * Block), "store over untracked: the partial watch setup did not retain the neighbors");
        }
        Fill(surface);
        Require(Pending(surface), "store over untracked: the image clear left no pending result");
    } else if (test.kind == Kind::Copied) {
        Settle(hold, surface, Stored);
        hold.lock();
        const auto from = StorageTexture::FindLive(source->Address(), SurfaceBytes);
        const auto to = StorageTexture::FindLive(address, SurfaceBytes);
        Require(from != nullptr && to != nullptr, "store over untracked: the surfaces have no storage images to copy between");
        const char* refusal = nullptr;
        if (!to->CopyFrom(*from, refusal)) {
            Require(refusal != nullptr && std::string_view(refusal) == "not imported", "store over untracked: the image copy was refused");
            std::fprintf(stderr, "[store over untracked] %s: skipped, the surface has no host import\n", test.name);
            return Outcome::Skipped;
        }
    } else if (!HoldPending(hold, surface, Stored)) {
        return Outcome::Missed;
    }
    if (test.kind == Kind::ShadowGap) {
        using namespace AgcDriver::Graphics;
        if (!UnitShadowEnabled() || !AgcDriver::GuestMemory::Watched(address, SurfaceBytes)) {
            std::fprintf(stderr, "[store over untracked] %s: skipped, watched host-import shadows are unavailable\n", test.name);
            return Outcome::Skipped;
        }
        StorageTexture::FlushPending(address, SurfaceBytes, nullptr, "store over untracked fixture", PublishScope::None);
        for (std::size_t unit = 0; unit < 4; ++unit) {
            if (!AnyShadowedOverlaps(address + unit * Block, Block)) {
                std::fprintf(stderr, "[store over untracked] %s: skipped, the write-back did not retain all four GPU shadows\n", test.name);
                return Outcome::Skipped;
            }
        }
        AgcDriver::GuestMemory::Unwatch(address + Block, Block);
        Require(AgcDriver::GuestMemory::Watched(address, Block) && AgcDriver::GuestMemory::Watched(address + 2 * Block, 2 * Block), "store over untracked: the shadow gap lost watched neighbors");
        // No classification or collection may intervene: publication itself must discover this
        // newer CPU write despite the unwatched interior hole in its live bounding span.
        for (std::size_t offset = 0; offset < Page; ++offset) {
            static_cast<volatile std::uint8_t*>(bytes)[offset] = Pattern(offset);
            static_cast<volatile std::uint8_t*>(bytes)[Block + offset] = Pattern(offset);
        }
        Require(PublishShadow(address, SurfaceBytes, PublishScope::Whole, PublishReason::Hook) == 2, "store over untracked: a mixed-watch publish did not select exactly the two fresh GPU units");
        std::vector<std::byte> whole(SurfaceBytes);
        AgcDriver::GuestMemory::Read(address, whole);
        Require(LostBytes(bytes) == 0 && LostBytes(bytes + Block) == 0, "store over untracked: mixed-watch shadow publication overwrote CPU bytes");
        ExpectBlock(surface, 2, Stored);
        ExpectBlock(surface, 3, Stored);
        return Outcome::Kept;
    }
    const bool leavesWatch = test.kind == Kind::LeftWatch || test.kind == Kind::PartialLeftWatch;
    if (leavesWatch) {
        const auto image = StorageTexture::FindLive(address, SurfaceBytes);
        Require(image != nullptr, "store over untracked: the surface has no storage image");
        if (!AgcDriver::GuestMemory::Watched(address, SurfaceBytes) || image->GuestSnapshotValid()) {
            std::fprintf(stderr, "[store over untracked] %s: skipped, fully watched pending results without a saved baseline are unavailable\n", test.name);
            return Outcome::Skipped;
        }
        AgcDriver::GuestMemory::Unwatch(address, test.kind == Kind::PartialLeftWatch ? Block : SurfaceBytes);
        if (test.kind == Kind::PartialLeftWatch) Require(AgcDriver::GuestMemory::Watched(address + Block, 3 * Block), "store over untracked: unwatching one unit also excluded its neighbors");
    }
    if (singleKeys != nullptr) {
        const auto image = StorageTexture::FindPending(address, SurfaceBytes);
        Require(image != nullptr && image->UploadedKeys() == AgcDriver::Graphics::DccKeys::Uncompressed && image->GuestSnapshotValid(), "single-key store: the first write did not retain its raw guest baseline under uncompressed keys");
        for (std::size_t offset = 0; offset < Page; ++offset) static_cast<volatile std::uint8_t*>(bytes)[offset] = Pattern(offset);
    }
    std::array<std::byte, Page> read{};
    if (test.kind == Kind::ReadBefore || test.kind == Kind::SingleReadBefore) {
        AgcDriver::GuestMemory::Read(address + Block + Page, read);
        ExpectBlock(surface, 1, Stored);
        if (singleKeys != nullptr) Require(LostBytes(bytes) == 0, "single-key store: a partial store overwrote the CPU-edited neighboring unit");
        Require(!Pending(surface, Block, Block) && Pending(surface, 2 * Block, Block), "store over untracked: a partial store did not leave its neighbor pending");
    }
    if (test.kind == Kind::Discard) {
        Require(StorageTexture::DiscardPendingInside(address + Block, Block) != 0, "store over untracked: no selected unit was discarded");
        Require(!Pending(surface, Block, Block) && Pending(surface, 2 * Block, Block), "store over untracked: a partial discard lost its neighbor's pending result");
        ExpectBlock(surface, 1, Untouched);
    }
    for (std::size_t offset = 0; offset < Page; ++offset) static_cast<volatile std::uint8_t*>(bytes)[offset] = Pattern(offset);
    if (test.kind == Kind::PartialWatchSaved) {
        Require(!AgcDriver::GuestMemory::Watched(address, Block) && AgcDriver::GuestMemory::Watched(address + Block, 3 * Block), "store over untracked: fill lost partial watch coverage");
        // The whole-surface collect could not cover unit0, so every unit has generation zero.
        // After the read below consumes unit0, the watched remainder must compare its snapshot.
        for (std::size_t offset = 0; offset < Page; ++offset) static_cast<volatile std::uint8_t*>(bytes)[Block + offset] = Pattern(offset);
    }
    if (leavesWatch) {
        const std::vector<std::uint8_t> before(bytes, bytes + SurfaceBytes);
        bool refused = false;
        try {
            AgcDriver::GuestMemory::Read(address + test.access, read);
        } catch (const std::runtime_error& error) {
            if (std::string_view(error.what()).find("outside the write watch without the guest bytes saved before them") == std::string_view::npos) throw;
            refused = true;
        }
        Require(refused, "store over untracked: unsaved pending data outside the watch was not refused");
        Require(std::equal(before.begin(), before.end(), bytes), "store over untracked: a refused store mutated guest memory");
        Require(Pending(surface, 0, Block) && Pending(surface, Block, Block), "store over untracked: a refused store dropped pending results");
        static_cast<void>(StorageTexture::DiscardPendingInside(address, SurfaceBytes));
        return Outcome::Kept;
    }
    AgcDriver::GuestMemory::Read(address + test.access, read);
    const auto lostAtAccess = LostBytes(bytes);
    Require(Pending(surface, 3 * Block, Block), "store over untracked: a partial access dropped an untouched GPU unit");
    if (test.kind == Kind::PartialWatchSaved) Require(!Pending(surface, 0, Block) && Pending(surface, Block, Block), "store over untracked: the watched unit did not remain pending after storing the unwatched unit");
    StorageTexture::FlushAllPending("store over untracked fixture");
    std::vector<std::byte> whole(SurfaceBytes);
    AgcDriver::GuestMemory::Read(address, whole);
    const auto lostAtEnd = LostBytes(bytes);
    const auto result = test.kind == Kind::Filled || test.kind == Kind::Copied || test.kind == Kind::PartialWatchSaved ? Filled : Stored;
    ExpectBlock(surface, 2, result);
    ExpectBlock(surface, 3, result);
    if (test.kind == Kind::Discard) ExpectBlock(surface, 1, Untouched);
    if (test.kind == Kind::PartialWatchSaved) Require(LostBytes(bytes + Block) == 0, "store over untracked: a watched unit's CPU edit was lost beside an unwatched unit");
    if (lostAtAccess == 0 && lostAtEnd == 0) return Outcome::Kept;
    std::fprintf(stderr, "[store over untracked] %s: of 0x%zx bytes the CPU wrote while the result was pending, %zu were lost after the access at +0x%zx and %zu after every result was stored\n", test.name, Page, lostAtAccess, test.access, lostAtEnd);
    return Outcome::Lost;
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 1 || (argc == 3 && std::string_view(argv[1]) == "--case"), "usage: store_over_untracked [--case NAME]");
        if (std::getenv("APS5_NO_BLOCK_TRACKING") != nullptr || std::getenv("APS5_NO_LAYER_CLEAR") != nullptr || std::getenv("APS5_NO_FLUSH_PRETEST") != nullptr || std::getenv("APS5_EAGER_WRITEBACK") != nullptr || std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr || (std::getenv("APS5_BLOCK_WRITEBACK_WIDEN") != nullptr && std::getenv("APS5_BLOCK_WRITEBACK_EACH") == nullptr)) {
            std::puts("skipped, the fixture requires deferred per-block storage write-back");
            return VulkanTestSkipped;
        }
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        {
            Surface probe;
            Require(AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(TextureDescriptor(probe.Texels()))).guestBytes == SurfaceBytes, "store over untracked: unexpected surface size");
        }
        std::vector<std::unique_ptr<Surface>> surfaces;
        struct ShutdownGuard {
            ~ShutdownGuard() { try { AgcDriverShutdown_nid_postfix(); } catch (...) {} }
        } shutdown;
        constexpr std::array<Case, 10> cases{{
            {"read-after", Kind::ReadAfter, Block + Page},
            {"read-before", Kind::ReadBefore, 0xe000},
            {"single-read-before", Kind::SingleReadBefore, 0xe000},
            {"discard", Kind::Discard, 0xe000},
            {"fill", Kind::Filled, 0xe000},
            {"copy", Kind::Copied, 0xe000},
            {"partial-watch-saved", Kind::PartialWatchSaved, 0xe000},
            {"left-watch", Kind::LeftWatch, 0xe000},
            {"partial-left-watch", Kind::PartialLeftWatch, 0xe000},
            {"shadow-gap", Kind::ShadowGap, 0},
        }};
        bool skipped = false;
        std::size_t selected = 0;
        for (const auto& test : cases) {
            if (argc == 3 && std::string_view(argv[2]) != test.name) continue;
            ++selected;
            auto outcome = Outcome::Missed;
            for (std::uint32_t attempt = 0; attempt < 256u && outcome == Outcome::Missed; ++attempt) outcome = Run(test, surfaces);
            Require(outcome != Outcome::Missed, "store over untracked: the result was never held pending");
            Require(outcome != Outcome::Lost, "a pending storage result was stored over bytes the CPU wrote to untracked memory");
            skipped = skipped || outcome == Outcome::Skipped;
            if (outcome == Outcome::Kept) std::printf("[store over untracked] %s passed\n", test.name);
        }
        Require(selected != 0, "store over untracked: unknown case");
        AgcDriverShutdown_nid_postfix();
        if (skipped) return VulkanTestSkipped;
        std::puts("store over untracked tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
