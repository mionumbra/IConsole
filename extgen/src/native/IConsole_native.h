#pragma once
#include "native/IConsoleInternal_native.h"

// Platform-specific includes
#ifdef OS_WINDOWS
#include <windows.h>
#include <io.h>
#else
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#endif

#include <string>
#include <string_view>
#include <fstream>
#include <mutex>
#include <sstream>
#include <chrono>
