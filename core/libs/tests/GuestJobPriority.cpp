#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <windows.h>

extern "C" {
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
int APS5_VABI scePthreadRename(Pthread thread, const char* name);
int APS5_VABI scePthreadGetprio(Pthread thread, int* priority);
}

static void Require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

struct Context {
    std::promise<int> started;
    std::promise<void> release;
};

static void* APS5_VABI Worker(void* argument) {
    auto& context = *static_cast<Context*>(argument);
    context.started.set_value(GetThreadPriority(GetCurrentThread()));
    context.release.get_future().wait();
    return nullptr;
}

static void Check(const char* name, int expected, bool enabled) {
    Context context;
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, nullptr, Worker, &context, name) == 0, "Cannot create worker");
    Require(context.started.get_future().get() == expected, "Wrong initial host priority");
    int guestPriority = 0;
    Require(scePthreadGetprio(thread, &guestPriority) == 0 && guestPriority == DEFAULT_THREAD_PRIORITY, "Host priority changed guest scheduling state");
    Require(scePthreadRename(thread, "ordinary-worker") == 0, "Cannot rename out of job role");
    Require(GetThreadPriority(thread->nativeHandle) == THREAD_PRIORITY_NORMAL, "Renaming out of job role did not restore priority");
    Require(SetThreadPriority(thread->nativeHandle, THREAD_PRIORITY_ABOVE_NORMAL), "Cannot set original host priority");
    Require(scePthreadRename(thread, "BPE JobWorkerThread CPU4") == 0, "Cannot rename into job role");
    const int wanted = enabled ? expected : THREAD_PRIORITY_ABOVE_NORMAL;
    Require(GetThreadPriority(thread->nativeHandle) == wanted, "Renaming into job role used wrong priority");
    Require(scePthreadRename(thread, "BPE JobWorkerThread CPU8") == 0, "Cannot rename within job role");
    Require(GetThreadPriority(thread->nativeHandle) == wanted, "Repeated job rename changed priority");
    Require(scePthreadRename(thread, "ordinary-worker") == 0, "Cannot restore unrelated role");
    Require(GetThreadPriority(thread->nativeHandle) == THREAD_PRIORITY_ABOVE_NORMAL, "Original nondefault host priority was not restored");
    context.release.set_value();
    Require(scePthreadJoin(thread, nullptr) == 0, "Cannot join worker");
}

int main(int argc, char** argv) {
    Require(argc == 2, "Expected mode argument");
    const int requested = std::strcmp(argv[1], "below") == 0 ? THREAD_PRIORITY_BELOW_NORMAL : std::strcmp(argv[1], "lowest") == 0 ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_NORMAL;
    Check("BPE JobWorkerThread CPU0", requested, requested != THREAD_PRIORITY_NORMAL);
    Context context;
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, nullptr, Worker, &context, "not-BPE JobWorkerThread") == 0, "Cannot create unrelated worker");
    Require(context.started.get_future().get() == THREAD_PRIORITY_NORMAL, "Unrelated worker priority changed");
    context.release.set_value();
    Require(scePthreadJoin(thread, nullptr) == 0, "Cannot join unrelated worker");
    std::puts("Job priority: creation, remote rename, restoration and unrelated threads passed");
}
