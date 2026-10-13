#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HOSTSTACK_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HOSTSTACK_HPP

#include <exception>
#include <type_traits>
#include <utility>

namespace AgcDriver {

bool OnHostStack();
void RunOnHostStack(void (*function)(void*), void* argument);

template<typename TFunction>
void WithHostStack(TFunction&& function) {
    static_assert(std::is_void_v<std::invoke_result_t<TFunction&>>);
    if (OnHostStack()) return std::forward<TFunction>(function)();
    std::exception_ptr error;
    struct Call {
        std::remove_reference_t<TFunction>* function;
        std::exception_ptr* error;
    } call{&function, &error};
    RunOnHostStack([](void* raw) {
        auto* call = static_cast<Call*>(raw);
        try {
            (*call->function)();
        } catch (...) {
            *call->error = std::current_exception();
        }
    }, &call);
    if (error) std::rethrow_exception(error);
}

}

#endif
