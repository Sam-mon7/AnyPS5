#include "prx/libc/include/GuestArena.hpp"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

int main() {
    const char* value = std::getenv("APS5_TRACE_GPU_FAULTS");
    const bool expected = value != nullptr && std::strcmp(value, "1") == 0;
    if (GuestArena::GuestArenaFaultTraceEnabled_nid_no_patch() != expected) {
        std::cerr << "APS5_TRACE_GPU_FAULTS did not follow its exact value\n";
        return 1;
    }
    std::uint64_t calls = 1, accepted = 1;
    GuestArena::GuestArenaTakeFaultRetries_nid_no_patch(&calls, &accepted);
    if (calls != 0 || accepted != 0) {
        std::cerr << "GPU fault retries were counted without any guest fault\n";
        return 1;
    }
    std::uint64_t queries = 1, queryNs = 1;
    GuestArena::GuestArenaTakeQueryCounts_nid_no_patch(&queries, &queryNs);
    if (!expected && (queries != 0 || queryNs != 0)) {
        std::cerr << "VirtualQuery calls were timed with the trace off\n";
        return 1;
    }
    std::cout << "GPU fault trace " << (expected ? "on" : "off") << '\n';
    return 0;
}
