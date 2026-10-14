#include "prx/libSceAgcDriver/Execution/include/DeviceFault.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GpuAddressBindings.hpp"
#include <chrono>
#include <memory>
#include <thread>
#include <string_view>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int faultCalls = 0;
bool faultFails = false;
bool faultDataFails = false;

VKAPI_ATTR VkResult VKAPI_CALL MockFaultInfo(VkDevice, VkDeviceFaultCountsEXT* counts, VkDeviceFaultInfoEXT* info) {
    ++faultCalls;
    if (faultFails) return VK_ERROR_DEVICE_LOST;
    if (info == nullptr) {
        counts->addressInfoCount = 2;
        counts->vendorInfoCount = 1;
        counts->vendorBinarySize = 4096;
        return VK_SUCCESS;
    }
    if (faultDataFails) return VK_ERROR_DEVICE_LOST;
    Require(counts->vendorBinarySize == 0, "the vendor binary was requested");
    Require(counts->addressInfoCount == 2 && info->pAddressInfos != nullptr, "the address array was not passed");
    Require(counts->vendorInfoCount == 1 && info->pVendorInfos != nullptr, "the vendor array was not passed");
    std::memset(info->description, 'd', sizeof(info->description));
    std::memcpy(info->description, "page fault", 10);
    info->description[10] = '\0';
    info->pAddressInfos[0] = {VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x1234000ull, 0xfffull};
    info->pAddressInfos[1] = {VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT, 0xabcdull, 0x3ull};
    std::memset(info->pVendorInfos[0].description, 'v', sizeof(info->pVendorInfos[0].description));
    info->pVendorInfos[0].vendorFaultCode = 0x77ull;
    info->pVendorInfos[0].vendorFaultData = 0x88ull;
    return VK_SUCCESS;
}

std::uint32_t checkpointCount = 0;

VKAPI_ATTR void VKAPI_CALL MockCheckpointData(VkQueue, std::uint32_t* count, VkCheckpointDataNV* data) {
    if (data == nullptr) {
        *count = checkpointCount;
        return;
    }
    Require(*count == checkpointCount, "the checkpoint array has the wrong size");
    for (std::uint32_t i = 0; i < *count; ++i) {
        Require(data[i].sType == VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV, "checkpoint entries were not initialised");
        data[i].stage = i == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        data[i].pCheckpointMarker = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x10000ull + i * 0x100ull));
    }
}

VkDevice FakeDevice() {
    return reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0x1000));
}

VkQueue FakeQueue() {
    return reinterpret_cast<VkQueue>(static_cast<std::uintptr_t>(0x2000));
}

void FaultInfo() {
    faultCalls = 0;
    faultFails = false;
    std::vector<VkDeviceFaultAddressInfoEXT> addresses;
    const auto lines = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr, &addresses);
    Require(addresses.size() == 2 && addresses[0].reportedAddress == 0x1234000 && addresses[0].addressPrecision == 0xfff && addresses[1].addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT, "typed fault addresses were not preserved for binding correlation");
    Require(faultCalls == 2, "the fault info was not queried for counts and then for data");
    Require(lines.size() == 4, "the report does not have a result, two addresses and a vendor line");
    Require(lines[0] == "[device-fault] result 0: page fault", "the description line changed");
    Require(lines[1] == "[device-fault] read invalid at 0x1234000 (precision 0xfff)", "the read fault line changed");
    Require(lines[2] == "[device-fault] instruction pointer fault at 0xabcd (precision 0x3)", "the instruction pointer line changed");
    Require(lines[3] == "[device-fault] vendor: " + std::string(VK_MAX_DESCRIPTION_SIZE, 'v') + " (code 0x77, data 0x88)", "an unterminated vendor description was not bounded");
}

void FaultInfoFails() {
    faultCalls = 0;
    faultFails = true;
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(1);
    const auto lines = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr, &addresses);
    Require(addresses.empty(), "a failed count query retained stale fault addresses");
    Require(faultCalls == 1, "a failed count query was followed by a data query");
    Require(lines.size() == 1 && lines[0] == "[device-fault] vkGetDeviceFaultInfoEXT reported nothing", "a failed query was not reported");
    faultFails = false;
    faultDataFails = true;
    addresses.resize(1);
    AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr, &addresses);
    Require(addresses.empty(), "a failed data query reported unavailable fault addresses");
    faultDataFails = false;
}

void Checkpoints() {
    checkpointCount = 2;
    auto lines = AgcDriver::DeviceFaultReport(VK_NULL_HANDLE, nullptr, FakeQueue(), MockCheckpointData);
    Require(lines.size() == 2, "each checkpoint gets one line");
    Require(lines[0] == "[device-fault] checkpoint: program 0x10000 at stage 0x1", "the first checkpoint line changed");
    Require(lines[1] == "[device-fault] checkpoint: program 0x10100 at stage 0x2000", "the second checkpoint line changed");
    checkpointCount = 0;
    lines = AgcDriver::DeviceFaultReport(VK_NULL_HANDLE, nullptr, FakeQueue(), MockCheckpointData);
    Require(lines.size() == 1 && lines[0] == "[device-fault] no checkpoint reached", "an empty checkpoint list was not reported");
}

void Nothing() {
    faultCalls = 0;
    Require(AgcDriver::DeviceFaultReport(FakeDevice(), nullptr, FakeQueue(), nullptr).empty(), "a device without either extension reported something");
    Require(faultCalls == 0, "the fault info was queried without the extension");
}

void AddressKinds() {
    Require(std::strcmp(AgcDriver::DeviceFaultAddressKind(VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT), "none") == 0, "none");
    Require(std::strcmp(AgcDriver::DeviceFaultAddressKind(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT), "write invalid") == 0, "write invalid");
    Require(std::strcmp(AgcDriver::DeviceFaultAddressKind(VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT), "execute invalid") == 0, "execute invalid");
    Require(std::strcmp(AgcDriver::DeviceFaultAddressKind(VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT), "instruction pointer unknown") == 0, "instruction pointer unknown");
    Require(std::strcmp(AgcDriver::DeviceFaultAddressKind(VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT), "instruction pointer invalid") == 0, "instruction pointer invalid");
    Require(std::strcmp(AgcDriver::DeviceFaultAddressKind(static_cast<VkDeviceFaultAddressTypeEXT>(99)), "unknown") == 0, "an unknown kind");
}

void BindingHistory() {
    using namespace AgcDriver::Graphics;
    using History = GpuAddressBindings;
    static_assert(sizeof(History) < 1024, "binding history entries must not consume the caller's stack");
    static_assert(sizeof(History::Entry) <= 56, "the opt-in binding ring must stay within 56 MiB per instance");
    auto history = std::make_shared<History>(73);
    std::array<VkDebugUtilsObjectNameInfoEXT, 10> objects{};
    for (std::size_t i = 0; i < objects.size(); ++i) {
        objects[i].sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        objects[i].objectType = i == 0 ? VK_OBJECT_TYPE_BUFFER : VK_OBJECT_TYPE_IMAGE;
        objects[i].objectHandle = 0xa0 + i;
    }
    VkDeviceAddressBindingCallbackDataEXT binding{VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT};
    binding.baseAddress = 0x12345000;
    binding.size = 1;
    binding.bindingType = VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT;
    binding.flags = VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT;
    VkBaseInStructure prefix{VK_STRUCTURE_TYPE_APPLICATION_INFO, reinterpret_cast<const VkBaseInStructure*>(&binding)};
    VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    data.pNext = &prefix;
    data.objectCount = 2;
    data.pObjects = objects.data();
    const auto emit = [&] {
        Require(History::Callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT, &data, history.get()) == VK_FALSE, "address binding callback never aborts Vulkan");
    };
    History::Callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT, &data, history.get());
    History::Callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data, history.get());
    Require(history->Find(0, UINT64_MAX).newest == 0, "binding history filters unrelated messages");
    const auto beforeCallbacks = std::chrono::steady_clock::now();
    emit();
    binding.bindingType = VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT;
    data.objectCount = 1;
    emit();
    binding.baseAddress = 0x12346000;
    emit();
    const auto afterCallbacks = std::chrono::steady_clock::now();
    auto matches = history->Find(0x12345000, 0x12345fff);
    Require(matches.total == 3 && matches.count == 3, "binding history preserves aliases and unbinds without merging live ownership");
    Require(matches.entries[0].bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT && matches.entries[1].object == 0xa1 && matches.entries[1].objectType == VK_OBJECT_TYPE_IMAGE && matches.entries[2].flags == VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT, "binding history copies object handles, types and internal flags");
    Require(matches.entries[2].observedAt >= beforeCallbacks && matches.entries[0].observedAt <= afterCallbacks && matches.entries[0].observedAt >= matches.entries[1].observedAt, "binding timestamps follow callback recording order within the observed interval");
    Require(matches.entries[1].observedAt == matches.entries[2].observedAt, "all objects from one callback share its clock sample");
    History::Entry boundary{};
    boundary.base = 4;
    boundary.size = 4;
    Require(!History::Overlaps(boundary, 8, 8) && History::Overlaps(boundary, 7, 7), "binding ranges exclude the end address");
    boundary.base = UINT64_MAX - 3;
    boundary.size = 8;
    Require(History::Overlaps(boundary, UINT64_MAX, UINT64_MAX) && !History::Overlaps(boundary, 0, 0), "binding range overlap does not wrap at UINT64_MAX");
    boundary.size = 0;
    Require(!History::Overlaps(boundary, 0, UINT64_MAX), "empty bindings never overlap");
    const auto output = std::unique_ptr<FILE, decltype(&std::fclose)>(std::tmpfile(), std::fclose);
    Require(output != nullptr, "binding diagnostic capture file");
    const auto beforeReport = std::chrono::steady_clock::now();
    history->Report({VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0x12345678, 0x1000}, output.get());
    const auto afterReport = std::chrono::steady_clock::now();
    history->Report({VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, UINT64_MAX, 0}, output.get());
    history->Report({VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, UINT64_MAX, 3}, output.get());
    std::rewind(output.get());
    std::array<char, 8192> bytes{};
    const auto size = std::fread(bytes.data(), 1, bytes.size(), output.get());
    const std::string_view text(bytes.data(), size);
    Require(text.find("instance=73 interval=0x12345000..0x12345fff newest overlaps=3/3") != text.npos, "fault precision uses aligned inclusive bounds, not reportedAddress plus precision");
    Require(text.find("invalid precision=0x0") != text.npos && text.find("invalid precision=0x3") != text.npos, "invalid fault precision is reported explicitly");
    Require(text.find("CPU callback order; no GPU progress or fault causality") != text.npos && text.find("No match does not prove no allocation") != text.npos, "binding reports retain their evidence limits");
    Require(text.find("CPU ages measure callback recording to diagnostic collection, not GPU fault time") != text.npos, "binding report ages do not claim GPU execution timing");
    for (std::size_t i = 0; i < matches.count; ++i) {
        const auto& entry = matches.entries[i];
        const auto line = text.find("instance=73 serial=" + std::to_string(entry.serial) + " ");
        Require(line != text.npos, "binding report includes the timestamped object");
        const auto ageField = text.find("cpu-age-at-report-ms=", line);
        unsigned long long ageMs = 0;
        Require(ageField != text.npos && ageField < text.find('\n', line) && std::sscanf(text.data() + ageField, "cpu-age-at-report-ms=%llu", &ageMs) == 1, "each binding record reports its CPU age");
        const auto minimum = std::chrono::duration_cast<std::chrono::milliseconds>(beforeReport - entry.observedAt).count();
        const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(afterReport - entry.observedAt).count();
        Require(ageMs >= static_cast<unsigned long long>(minimum) && ageMs <= static_cast<unsigned long long>(maximum), "binding ages are relative to diagnostic collection without timing thresholds");
    }
    data.objectCount = objects.size();
    emit();
    data.pNext = nullptr;
    emit();
    data.pNext = &prefix;
    data.pObjects = nullptr;
    emit();
    matches = history->Find(0, UINT64_MAX);
    Require(matches.omittedObjects == 2 && matches.droppedCallbacks == 2, "binding diagnostic accounts separately for object caps and malformed callbacks");
    data.objectCount = 1;
    data.pObjects = objects.data();
    for (std::size_t i = 0; i < History::MatchLimit + 1; ++i) emit();
    const auto filtered = history->Find(0, UINT64_MAX, VK_OBJECT_TYPE_IMAGE, 0xa1);
    Require(filtered.total == 2 && filtered.count == 2 && filtered.entries[0].object == 0xa1 && filtered.entries[1].object == 0xa1, "native smoke object filtering precedes the diagnostic output cap");
    for (std::size_t i = 0; i < History::Capacity + 3; ++i) emit();
    matches = history->Find(0, UINT64_MAX);
    Require(matches.total == History::Capacity && matches.count == History::MatchLimit && matches.overwritten == 48 && matches.oldest == 49, "binding ring and match output remain bounded and count overwritten records");
    Require(matches.entries[0].serial == matches.newest && matches.entries.back().serial == matches.newest - History::MatchLimit + 1 && history->Find(0x12345000, 0x12345fff).total == 0, "binding ring reports the newest retained matches and never stale replacements");
    auto concurrent = std::make_shared<History>(74);
    const auto write = [&] {
        for (int i = 0; i < 256; ++i) History::Callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT, &data, concurrent.get());
    };
    std::thread first(write), second(write);
    first.join();
    second.join();
    const auto concurrentMatches = concurrent->Find(0, UINT64_MAX);
    Require(concurrentMatches.total == 512 && history->Find(0, UINT64_MAX).newest == matches.newest, "concurrent callbacks are serialized per instance without shared history");
    for (std::size_t i = 1; i < concurrentMatches.count; ++i) Require(concurrentMatches.entries[i - 1].observedAt >= concurrentMatches.entries[i].observedAt, "concurrent binding timestamps follow the serialized record order");
}

}

int main() {
    try {
        FaultInfo();
        FaultInfoFails();
        Checkpoints();
        Nothing();
        AddressKinds();
        BindingHistory();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_device_fault: %s\n", error.what());
        return 1;
    }
    std::puts("agc_device_fault: passed");
    return 0;
}
