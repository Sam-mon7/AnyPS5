#ifndef ANYPS5_KYTY_ADAPTERS_COMMON_ASSERT_H
#define ANYPS5_KYTY_ADAPTERS_COMMON_ASSERT_H

#include "common/common.h"
#include <string>

namespace Common {

[[noreturn]] void KytyExit(const char* file, int line, const std::string& text);
std::string KytyFormat(const char* format, ...);

}

#define EXIT(...) ::Common::KytyExit(__FILE__, __LINE__, ::Common::KytyFormat(__VA_ARGS__))
#define EXIT_IF(x) ((x) ? ::Common::KytyExit(__FILE__, __LINE__, #x) : static_cast<void>(0))
#define EXIT_NOT_IMPLEMENTED(x) ((x) ? ::Common::KytyExit(__FILE__, __LINE__, "not implemented: " #x) : static_cast<void>(0))
#define KYTY_NOT_IMPLEMENTED EXIT_NOT_IMPLEMENTED(true)

#endif
