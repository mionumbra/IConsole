// ##### extgen :: Auto-generated file do not edit!! #####

#include "IConsoleInternal_native.h"
#include "IConsoleInternal_exports.h"

using namespace gm_structs;
using namespace gm::wire::codec;

GMEXPORT double __EXT_NATIVE__iconsole_open()
{
    iconsole_open();
    return 0;
}

GMEXPORT double __EXT_NATIVE__iconsole_print(char* text)
{
    iconsole_print(text);
    return 0;
}

GMEXPORT double __EXT_NATIVE__iconsole_print_line(char* text)
{
    iconsole_print_line(text);
    return 0;
}

GMEXPORT double __EXT_NATIVE__iconsole_clear()
{
    iconsole_clear();
    return 0;
}

GMEXPORT double __EXT_NATIVE__iconsole_close()
{
    iconsole_close();
    return 0;
}

GMEXPORT double __EXT_NATIVE__iconsole_is_open()
{
    auto&& __result = iconsole_is_open();
    return static_cast<double>(__result);
}

GMEXPORT double __EXT_NATIVE__iconsole_set_title(char* title)
{
    iconsole_set_title(title);
    return 0;
}

GMEXPORT double __EXT_NATIVE__iconsole_set_color(double color)
{
    iconsole_set_color(static_cast<std::int32_t>(color));
    return 0;
}

GMEXPORT double __EXT_NATIVE__iconsole_set_log_level(double level)
{
    iconsole_set_log_level(static_cast<std::int32_t>(level));
    return 0;
}

GMEXPORT double __EXT_NATIVE__iconsole_get_log_level()
{
    auto&& __result = iconsole_get_log_level();
    return static_cast<double>(__result);
}

GMEXPORT double __EXT_NATIVE__iconsole_log(double level, char* text)
{
    iconsole_log(static_cast<std::int32_t>(level), text);
    return 0;
}

GMEXPORT double __EXT_NATIVE__iconsole_set_timestamp(double enabled)
{
    iconsole_set_timestamp(static_cast<bool>(enabled));
    return 0;
}

GMEXPORT double __EXT_NATIVE__iconsole_timestamp_enabled()
{
    auto&& __result = iconsole_timestamp_enabled();
    return static_cast<double>(__result);
}

GMEXPORT double __EXT_NATIVE__iconsole_set_log_file(char* path)
{
    iconsole_set_log_file(path);
    return 0;
}

GMEXPORT char* __EXT_NATIVE__iconsole_get_log_file()
{
    static std::string __result;
    __result = iconsole_get_log_file();
    return (char*)__result.c_str();
}

GMEXPORT double __EXT_NATIVE__iconsole_close_log_file()
{
    iconsole_close_log_file();
    return 0;
}

GMEXPORT double __EXT_NATIVE__iconsole_log_file_open()
{
    auto&& __result = iconsole_log_file_open();
    return static_cast<double>(__result);
}

GMEXPORT double __EXT_NATIVE__iconsole_has_input()
{
    auto&& __result = iconsole_has_input();
    return static_cast<double>(__result);
}

GMEXPORT char* __EXT_NATIVE__iconsole_read_line(double timeout_ms)
{
    static std::string __result;
    __result = iconsole_read_line(static_cast<std::int32_t>(timeout_ms));
    return (char*)__result.c_str();
}

GMEXPORT double __EXT_NATIVE__iconsole_read_key()
{
    auto&& __result = iconsole_read_key();
    return static_cast<double>(__result);
}

