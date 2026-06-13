// ##### extgen :: Auto-generated file do not edit!! #####

#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
#include <array>
#include <optional>
#include "core/GMExtWire.h"

namespace gm_consts
{
}


namespace gm_enums
{
}


namespace gm_structs
{

}

namespace gm::wire::codec
{
}

namespace gm::wire::details
{
}

void iconsole_open();
void iconsole_print(std::string_view text);
void iconsole_print_line(std::string_view text);
void iconsole_clear();
void iconsole_close();
bool iconsole_is_open();
void iconsole_set_title(std::string_view title);
void iconsole_set_color(std::int32_t color);
void iconsole_set_log_level(std::int32_t level);
std::int32_t iconsole_get_log_level();
void iconsole_log(std::int32_t level, std::string_view text);
void iconsole_set_timestamp(bool enabled);
bool iconsole_timestamp_enabled();
void iconsole_set_log_file(std::string_view path);
std::string iconsole_get_log_file();
void iconsole_close_log_file();
bool iconsole_log_file_open();
bool iconsole_has_input();
std::string iconsole_read_line(std::int32_t timeout_ms);
std::int32_t iconsole_read_key();
