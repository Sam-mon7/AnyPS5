#include "Recompiler.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {

void Require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}

#ifdef _WIN32
std::size_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX info{};
    info.cb = sizeof(info);
    Require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info)), "process memory counters unavailable");
    return info.PrivateUsage;
}
#endif

}

int main() {
    try {
        using ShaderRecompiler::SharedSpirv;
        constexpr std::size_t Count = 16u << 20u;
        constexpr std::uint32_t Pattern = 0x12345678u;
        const bool backed = std::getenv("APS5_FILE_BACKED_SPIRV") != nullptr && std::string_view(std::getenv("APS5_FILE_BACKED_SPIRV")) == "1";
#ifdef _WIN32
        const auto before = PrivateBytes();
#endif
        SharedSpirv original(std::vector<std::uint32_t>(Count, Pattern));
        const auto& read = original;
        const auto* pointer = read.data();
        Require(read.size() == Count && !read.empty(), "module extent changed");
#ifdef _WIN32
        const auto after = PrivateBytes();
        std::printf("Private bytes delta: %lld\n", static_cast<long long>(after) - static_cast<long long>(before));
        MEMORY_BASIC_INFORMATION region{};
        Require(VirtualQuery(pointer, &region, sizeof(region)) == sizeof(region), "module memory query failed");
        Require((region.Type == MEM_MAPPED) == backed, "module uses the wrong backing type");
        if (backed) {
            Require(region.Protect == PAGE_READONLY, "file-backed module permits writes");
            Require(after <= before + (16u << 20u), "file-backed module retains private commit");
        }
#endif
        std::vector<SharedSpirv> copies(8, original);
        for (const auto& copy : copies) Require(copy.data() == pointer && copy[50000] == Pattern, "shared immutable words changed");
        copies.front()[50000] = 0xabcdef01u;
        Require(read[50000] == Pattern && static_cast<const SharedSpirv&>(copies.front())[50000] == 0xabcdef01u, "indexed write did not detach");
        auto& edited = copies[1];
        edited.data()[Count - 1] = 0xffu;
        edited.insert(edited.begin() + 4, {0xfeedu});
        edited.resize(Count + 2);
        Require(edited.size() == Count + 2 && read.size() == Count && read[Count - 1] == Pattern, "vector editing changed another holder");
        Require(read[4] == Pattern && static_cast<const SharedSpirv&>(edited)[4] == 0xfeedu, "insertion lost its position");
        const auto& legacy = copies[2].Words();
        Require(legacy.size() == Count && legacy[50000] == Pattern, "legacy vector read changed words");
        original = SharedSpirv{};
        Require(static_cast<const SharedSpirv&>(copies.back())[Count - 1] == Pattern, "words did not survive the first owner");
        std::array<std::thread, 4> readers;
        std::atomic<bool> correct{true};
        for (auto& reader : readers) reader = std::thread([&] {
            const auto& copy = copies.back();
            if (copy.Words()[50000] != Pattern || copy.Read()[Count - 1] != Pattern) correct.store(false);
        });
        for (auto& reader : readers) reader.join();
        Require(correct.load(), "concurrent reads changed words");
        SharedSpirv empty;
        Require(empty.empty() && empty.Read().empty() && empty == SharedSpirv{}, "empty module changed");
        std::printf("SharedSpirv backing=%u: sharing, write isolation, lifetime and reads passed\n", backed);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
