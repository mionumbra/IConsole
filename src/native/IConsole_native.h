#pragma once
#include "native/IConsoleInternal_native.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#ifdef OS_WINDOWS
#include <windows.h>
#else
#include <cstdio>
#endif
