#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include <cstdio>
#include <cstdlib>

extern "C" {
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
}

static void Require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

static void* APS5_VABI Worker(void*) {
    return nullptr;
}

int main() {
    for (const char* name : {"BPE JobWorkerThread CPU0", "BPE JobWorkerThread CPU1", "ordinary-worker"}) {
        Pthread thread = nullptr;
        Require(scePthreadCreate(&thread, nullptr, Worker, nullptr, name) == 0, "Cannot create worker");
        Require(scePthreadJoin(thread, nullptr) == 0, "Cannot join worker");
    }
    std::fprintf(stderr, "Job idle wait: passed\n");
}
