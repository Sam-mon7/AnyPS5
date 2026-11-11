#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DISPATCHBUILDTRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DISPATCHBUILDTRACE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ShaderRecompiler { struct RecompileResult; }

namespace AgcDriver {

class DispatchBuildTrace {
public:
    struct Context {
        bool known = false;
        std::uint32_t queue = 0;
        std::uint64_t program = 0, digest = 0, packetOffset = 0, submission = 0, interval = 0, invocation = 0;
        const char* recipe = "not-attempted";
    };
    class Scope {
    public:
        Scope(std::uint32_t queue, std::uint64_t program, std::uint64_t digest, std::uint64_t packetOffset, std::uint64_t submission);
        explicit Scope(Context context);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        Context previous;
        bool active = false;
    };
    struct Detail {
        const char* check = "not-recorded";
        const char* role = "unknown";
        std::size_t binding = static_cast<std::size_t>(-1), element = static_cast<std::size_t>(-1);
        std::optional<std::uint64_t> priorAddress, currentAddress, priorBytes, currentBytes, priorIdentity, currentIdentity;
    };
    struct Event {
        const ShaderRecompiler::RecompileResult* shader = nullptr;
        std::span<const std::uint32_t> key;
        std::uint64_t program = 0;
        const char* event = "build";
        const char* reason = "unknown";
        const char* exactFind = "not-attempted";
        const char* movableFind = "not-attempted";
        const char* reuse = "not-attempted";
        const char* cacheInsert = "not-attempted";
        std::uint64_t cacheMembership = 0;
        const char* reusable = "unknown";
        const char* recipeBuilt = "not-attempted";
        Detail detail;
        std::optional<std::uint64_t> addressTable, bdaReadOnly, spaceOnly, copiedWrites, holdsLease;
        std::uint64_t stageANs = 0, completeNs = 0, imagesNs = 0, uploadNs = 0, bdaNs = 0, revalidateNs = 0;
        std::uint64_t cpuBytes = 0, gpuBytes = 0, copiedBytes = 0;
    };
    struct Statistics { std::uint64_t events, emitted, suppressed, bytes, failures; };
    static bool Enabled() noexcept;
    static Context Current() noexcept;
    static void Recipe(const char* result) noexcept;
    static void Emit(const Event& event) noexcept;
    static Statistics Counts() noexcept;
};

}

#endif
