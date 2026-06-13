// ##### extgen :: Auto-generated file do not edit!! #####

#pragma once
#include "core/GMExtUtils.h"

GMEXPORT double __EXT_NATIVE__iconsole_open();
GMEXPORT double __EXT_NATIVE__iconsole_print(char* text);
GMEXPORT double __EXT_NATIVE__iconsole_print_line(char* text);
GMEXPORT double __EXT_NATIVE__iconsole_clear();
GMEXPORT double __EXT_NATIVE__iconsole_close();
GMEXPORT double __EXT_NATIVE__iconsole_is_open();
GMEXPORT double __EXT_NATIVE__iconsole_set_title(char* title);
GMEXPORT double __EXT_NATIVE__iconsole_set_color(double color);
GMEXPORT double __EXT_NATIVE__iconsole_set_log_level(double level);
GMEXPORT double __EXT_NATIVE__iconsole_get_log_level();
GMEXPORT double __EXT_NATIVE__iconsole_log(double level, char* text);
GMEXPORT double __EXT_NATIVE__iconsole_set_timestamp(double enabled);
GMEXPORT double __EXT_NATIVE__iconsole_timestamp_enabled();
GMEXPORT double __EXT_NATIVE__iconsole_set_log_file(char* path);
GMEXPORT char* __EXT_NATIVE__iconsole_get_log_file();
GMEXPORT double __EXT_NATIVE__iconsole_close_log_file();
GMEXPORT double __EXT_NATIVE__iconsole_log_file_open();
GMEXPORT double __EXT_NATIVE__iconsole_has_input();
GMEXPORT char* __EXT_NATIVE__iconsole_read_line(double timeout_ms);
GMEXPORT double __EXT_NATIVE__iconsole_read_key();

