#include "prx/libSceAgcDriver/Graphics/include/MemoryLedger.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace AgcDriver::Graphics {

namespace {

constexpr std::uint32_t ImportedKey = 0x100u;

struct Entry {
    VkDeviceSize bytes = 0;
    std::uint32_t key = 0;
    std::uintptr_t caller = 0;
};

struct Site {
    VkDeviceSize liveBytes = 0;
    std::uint64_t live = 0;
    VkDeviceSize allocatedBytes = 0;
    std::uint64_t allocated = 0;
    VkDeviceSize freedBytes = 0;
    std::uint64_t freed = 0;
};

struct Ledger {
    std::mutex mutex;
    PFN_vkGetDeviceProcAddr resolver = nullptr;
    PFN_vkAllocateMemory allocate = nullptr;
    PFN_vkFreeMemory free = nullptr;
    VkPhysicalDeviceMemoryProperties properties{};
    bool haveProperties = false;
    std::unordered_map<VkDeviceMemory, Entry> live;
    std::map<std::pair<std::uintptr_t, std::uint32_t>, Site> sites;
    std::map<std::uintptr_t, std::string> names;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

Ledger& State() {
    static auto& ledger = *new Ledger();
    return ledger;
}

bool importsHost(const void* next) {
    for (auto* header = static_cast<const VkBaseInStructure*>(next); header != nullptr; header = header->pNext) {
        if (header->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT) return true;
    }
    return false;
}

const std::string& siteName(Ledger& ledger, std::uintptr_t address) {
    auto& name = ledger.names[address];
    if (!name.empty()) return name;
    char text[300] = {};
#ifdef _WIN32
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) && module != nullptr) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* base = std::strrchr(path, '\\');
        std::snprintf(text, sizeof(text), "%s+0x%llx", base != nullptr ? base + 1 : path, static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
    }
#endif
    if (text[0] == 0) std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
    name = text;
    return name;
}

std::string typeName(const Ledger& ledger, std::uint32_t key) {
    if ((key & ImportedKey) != 0) return "imported";
    const auto type = key & 0xffu;
    if (!ledger.haveProperties || type >= ledger.properties.memoryTypeCount) return "type " + std::to_string(type);
    const auto flags = ledger.properties.memoryTypes[type].propertyFlags;
    std::string name;
    if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) name += "device";
    if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) name += name.empty() ? "host" : "+host";
    if ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) name += "-cached";
    if (name.empty()) name = "other";
    return name + "/" + std::to_string(type);
}

std::string reportLocked(Ledger& ledger) {
    const auto mib = [](VkDeviceSize bytes) { return static_cast<double>(bytes) / 1048576.0; };
    std::map<std::uint32_t, std::pair<VkDeviceSize, std::uint64_t>> byType;
    for (const auto& [memory, entry] : ledger.live) {
        auto& total = byType[entry.key];
        total.first += entry.bytes;
        ++total.second;
    }
    std::string text = "[memory] live Vulkan memory by type:";
    char part[512];
    for (const auto& [key, total] : byType) {
        std::snprintf(part, sizeof(part), " %s %.0f MiB in %llu;", typeName(ledger, key).c_str(), mib(total.first), static_cast<unsigned long long>(total.second));
        text += part;
    }
    std::vector<std::pair<std::pair<std::uintptr_t, std::uint32_t>, Site>> sites(ledger.sites.begin(), ledger.sites.end());
    std::sort(sites.begin(), sites.end(), [](const auto& left, const auto& right) { return left.second.liveBytes > right.second.liveBytes; });
    text += " top sites by live bytes (live MiB/count, 10 s allocated MiB/count, freed MiB/count):";
    for (std::size_t index = 0; index < sites.size() && index < 25 && sites[index].second.liveBytes != 0; ++index) {
        const auto& [key, site] = sites[index];
        std::snprintf(part, sizeof(part), " [%s %s %.0f/%llu, %.0f/%llu, %.0f/%llu]", siteName(ledger, key.first).c_str(), typeName(ledger, key.second).c_str(), mib(site.liveBytes), static_cast<unsigned long long>(site.live), mib(site.allocatedBytes), static_cast<unsigned long long>(site.allocated), mib(site.freedBytes), static_cast<unsigned long long>(site.freed));
        text += part;
    }
    std::sort(sites.begin(), sites.end(), [](const auto& left, const auto& right) { return left.second.allocatedBytes > right.second.allocatedBytes; });
    text += "; top sites by 10 s allocations:";
    for (std::size_t index = 0; index < sites.size() && index < 10 && sites[index].second.allocatedBytes != 0; ++index) {
        const auto& [key, site] = sites[index];
        std::snprintf(part, sizeof(part), " [%s %s %.0f MiB/%llu]", siteName(ledger, key.first).c_str(), typeName(ledger, key.second).c_str(), mib(site.allocatedBytes), static_cast<unsigned long long>(site.allocated));
        text += part;
    }
    for (auto& [key, site] : ledger.sites) {
        site.allocatedBytes = 0;
        site.allocated = 0;
        site.freedBytes = 0;
        site.freed = 0;
    }
    return text;
}

void maybeReport(Ledger& ledger) {
    const auto now = std::chrono::steady_clock::now();
    if (now - ledger.lastReport < std::chrono::seconds(10)) return;
    ledger.lastReport = now;
    const auto text = reportLocked(ledger);
    AgcDriver::ProfilePrint_nid_no_patch("%s\n", text.c_str());
}

VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    auto& ledger = State();
    const auto result = ledger.allocate(device, info, callbacks, memory);
    if (result != VK_SUCCESS) return result;
    const std::uint32_t key = info->memoryTypeIndex | (importsHost(info->pNext) ? ImportedKey : 0u);
    std::lock_guard lock(ledger.mutex);
    ledger.live[*memory] = {info->allocationSize, key, caller};
    auto& site = ledger.sites[{caller, key}];
    site.liveBytes += info->allocationSize;
    ++site.live;
    site.allocatedBytes += info->allocationSize;
    ++site.allocated;
    maybeReport(ledger);
    return result;
}

VKAPI_ATTR void VKAPI_CALL release(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* callbacks) {
    auto& ledger = State();
    if (memory != VK_NULL_HANDLE) {
        std::lock_guard lock(ledger.mutex);
        if (const auto found = ledger.live.find(memory); found != ledger.live.end()) {
            auto& site = ledger.sites[{found->second.caller, found->second.key}];
            site.liveBytes -= found->second.bytes;
            --site.live;
            site.freedBytes += found->second.bytes;
            ++site.freed;
            ledger.live.erase(found);
        }
        maybeReport(ledger);
    }
    ledger.free(device, memory, callbacks);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL resolve(VkDevice device, const char* name) {
    auto& ledger = State();
    if (std::strcmp(name, "vkAllocateMemory") == 0 || std::strcmp(name, "vkFreeMemory") == 0) {
        std::lock_guard lock(ledger.mutex);
        if (ledger.allocate == nullptr) ledger.allocate = reinterpret_cast<PFN_vkAllocateMemory>(ledger.resolver(device, "vkAllocateMemory"));
        if (ledger.free == nullptr) ledger.free = reinterpret_cast<PFN_vkFreeMemory>(ledger.resolver(device, "vkFreeMemory"));
        if (ledger.allocate == nullptr || ledger.free == nullptr) return nullptr;
        return name[2] == 'A' ? reinterpret_cast<PFN_vkVoidFunction>(&allocate) : reinterpret_cast<PFN_vkVoidFunction>(&release);
    }
    return ledger.resolver(device, name);
}

}

bool MemoryLedgerEnabled() {
    static const bool enabled = std::getenv("APS5_PROFILE_MEMORY") != nullptr;
    return enabled;
}

PFN_vkGetDeviceProcAddr MemoryLedgerResolver(PFN_vkGetDeviceProcAddr resolver) {
    if (!MemoryLedgerEnabled() || resolver == nullptr) return resolver;
    State().resolver = resolver;
    return &resolve;
}

void MemoryLedgerSetProperties(const VkPhysicalDeviceMemoryProperties& properties) {
    if (!MemoryLedgerEnabled()) return;
    auto& ledger = State();
    std::lock_guard lock(ledger.mutex);
    ledger.properties = properties;
    ledger.haveProperties = true;
}

std::string MemoryLedgerReport() {
    auto& ledger = State();
    std::lock_guard lock(ledger.mutex);
    return reportLocked(ledger);
}

}
