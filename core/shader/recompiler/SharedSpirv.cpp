#include "Recompiler.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#include <sys/mman.h>
#endif

namespace ShaderRecompiler {

struct SharedSpirv::Storage {
    mutable std::mutex mutex;
    mutable std::vector<std::uint32_t> values;
    const std::uint32_t* mapped = nullptr;
    std::size_t count = 0;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
#else
    int file = -1;
#endif

    ~Storage() {
#ifdef _WIN32
        if (mapped != nullptr) UnmapViewOfFile(mapped);
        if (mapping != nullptr) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
#else
        if (mapped != nullptr) munmap(const_cast<std::uint32_t*>(mapped), count * sizeof(std::uint32_t));
        if (file != -1) close(file);
#endif
    }

    std::span<const std::uint32_t> Read() const {
        return mapped != nullptr ? std::span(mapped, count) : std::span<const std::uint32_t>(values);
    }

    void Back(std::span<const std::uint32_t> source) {
        const auto bytes = source.size_bytes();
#ifdef _WIN32
        const auto directory = std::filesystem::temp_directory_path();
        wchar_t name[MAX_PATH];
        if (GetTempFileNameW(directory.c_str(), L"asp", 0, name) == 0) throw std::system_error(GetLastError(), std::system_category(), "SPIR-V temporary file");
        file = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            DeleteFileW(name);
            throw std::system_error(error, std::system_category(), "SPIR-V backing file");
        }
        const auto* cursor = reinterpret_cast<const std::byte*>(source.data());
        for (std::size_t remaining = bytes; remaining != 0;) {
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 64u << 20u));
            DWORD written = 0;
            if (!WriteFile(file, cursor, chunk, &written, nullptr)) throw std::system_error(GetLastError(), std::system_category(), "SPIR-V backing write");
            if (written == 0) throw std::runtime_error("SPIR-V backing write made no progress");
            cursor += written;
            remaining -= written;
        }
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping == nullptr) throw std::system_error(GetLastError(), std::system_category(), "SPIR-V file mapping");
        mapped = static_cast<const std::uint32_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, bytes));
        if (mapped == nullptr) throw std::system_error(GetLastError(), std::system_category(), "SPIR-V mapped words");
#else
        auto name = (std::filesystem::temp_directory_path() / "anyps5-spv-XXXXXX").string();
        file = mkstemp(name.data());
        if (file == -1) throw std::system_error(errno, std::generic_category(), "SPIR-V backing file");
        if (unlink(name.c_str()) != 0) throw std::system_error(errno, std::generic_category(), "SPIR-V backing unlink");
        const auto* cursor = reinterpret_cast<const std::byte*>(source.data());
        for (std::size_t remaining = bytes; remaining != 0;) {
            const auto written = write(file, cursor, std::min<std::size_t>(remaining, 64u << 20u));
            if (written < 0 && errno == EINTR) continue;
            if (written < 0) throw std::system_error(errno, std::generic_category(), "SPIR-V backing write");
            if (written == 0) throw std::runtime_error("SPIR-V backing write made no progress");
            cursor += written;
            remaining -= written;
        }
        const auto view = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, file, 0);
        if (view == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "SPIR-V mapped words");
        mapped = static_cast<const std::uint32_t*>(view);
#endif
        count = source.size();
    }
};

SharedSpirv::SharedSpirv(std::vector<std::uint32_t> source) {
    auto storage = std::make_shared<Storage>();
    const auto* enabled = std::getenv("APS5_FILE_BACKED_SPIRV");
    if (enabled != nullptr && std::string_view(enabled) == "1" && source.size() >= 16384u) storage->Back(source);
    else storage->values = std::move(source);
    words = std::move(storage);
}

SharedSpirv& SharedSpirv::operator=(std::vector<std::uint32_t> other) {
    *this = SharedSpirv(std::move(other));
    return *this;
}

std::span<const std::uint32_t> SharedSpirv::Read() const {
    return words != nullptr ? words->Read() : std::span<const std::uint32_t>();
}

const std::vector<std::uint32_t>& SharedSpirv::Words() const {
    if (words == nullptr) {
        static const std::vector<std::uint32_t> empty;
        return empty;
    }
    if (words->mapped != nullptr) {
        std::lock_guard lock(words->mutex);
        if (words->values.empty()) words->values.assign(words->mapped, words->mapped + words->count);
    }
    return words->values;
}

std::vector<std::uint32_t>& SharedSpirv::Mutable() {
    if (words == nullptr || words.use_count() != 1 || words->mapped != nullptr) {
        const auto source = Read();
        auto storage = std::make_shared<Storage>();
        storage->values.assign(source.begin(), source.end());
        words = std::move(storage);
    }
    return words->values;
}

bool operator==(const SharedSpirv& left, const SharedSpirv& right) {
    return left.words == right.words || std::ranges::equal(left.Read(), right.Read());
}

}
