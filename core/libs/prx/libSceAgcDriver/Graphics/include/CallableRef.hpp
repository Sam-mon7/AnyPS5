#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_CALLABLEREF_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_CALLABLEREF_HPP

#include <memory>
#include <type_traits>
#include <utility>

namespace AgcDriver::Graphics {

template<typename Signature>
class CallableRef;

template<typename Result, typename... Arguments>
class CallableRef<Result(Arguments...)> {
public:
    template<typename Callable, typename = std::enable_if_t<!std::is_same_v<std::remove_cvref_t<Callable>, CallableRef>>>
    explicit CallableRef(const Callable& callable) : object(std::addressof(callable)), call([](const void* target, Arguments... arguments) -> Result { return (*static_cast<const Callable*>(target))(std::forward<Arguments>(arguments)...); }) {}

    Result operator()(Arguments... arguments) const { return call(object, std::forward<Arguments>(arguments)...); }

private:
    const void* object;
    Result (*call)(const void*, Arguments...);
};

}

#endif
