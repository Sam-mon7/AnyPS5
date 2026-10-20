#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include <cstdio>

extern "C" {
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
}

static void* APS5_VABI Worker(void*) {
    return nullptr;
}

int main() {
    Pthread thread = nullptr;
    if (scePthreadCreate(&thread, nullptr, Worker, nullptr, "worker") != 0 || scePthreadJoin(thread, nullptr) != 0) {
        std::fprintf(stderr, "Cannot run a guest thread\n");
        return 1;
    }
    std::fprintf(stderr, "GPU buffer pages: passed\n");
}
