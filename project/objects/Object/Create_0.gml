var _self_test = environment_get_variable("ICONSOLE_SELF_TEST") == "1";

if (_self_test) {
    var _failures = 0;
    var _check = function(_condition, _message) {
        if (!_condition) {
            ++_failures;
            show_debug_message("ICONSOLE_SELF_TEST_FAIL: " + _message);
        }
    };
    var _read_text = function(_path) {
        if (!file_exists(_path)) {
            return "";
        }
        var _file = file_text_open_read(_path);
        var _text = "";
        while (!file_text_eof(_file)) {
            _text += file_text_read_string(_file) + "\n";
            file_text_readln(_file);
        }
        file_text_close(_file);
        return _text;
    };

    iconsole_open();
    _check(iconsole_is_open(), "console opened");
    iconsole_print_line("ICONSOLE_REDIRECTED_OUTPUT_PASS");
    _check(!iconsole_has_input(), "no completed input line");
    _check(iconsole_read_line(0) == "", "zero-timeout read");
    iconsole_close();
    _check(!iconsole_is_open(), "console closed");
    iconsole_close();
    _check(!iconsole_is_open(), "repeated close");

    iconsole_set_force_ansi(false);
    _check(!iconsole_force_ansi_enabled(), "force ANSI disabled");
    iconsole_set_force_ansi(true);
    _check(iconsole_force_ansi_enabled(), "force ANSI enabled");
    iconsole_set_force_ansi(false);

    iconsole_set_log_level(-100);
    _check(iconsole_get_log_level() == 0, "minimum log level clamp");
    iconsole_set_log_level(100);
    _check(iconsole_get_log_level() == 3, "maximum log level clamp");
    iconsole_set_log_level(0);

    iconsole_set_timestamp(true);
    _check(iconsole_timestamp_enabled(), "timestamp enabled");
    iconsole_set_timestamp(false);
    _check(!iconsole_timestamp_enabled(), "timestamp disabled");

    var _log_path = temp_directory + "iconsole-" + chr(27979) + chr(35797) + ".log";
    if (file_exists(_log_path)) {
        file_delete(_log_path);
    }
    iconsole_set_log_file(_log_path);
    _check(iconsole_log_file_open(), "log file opened");
    _check(iconsole_get_log_file() == _log_path, "log file path round trip");
    iconsole_set_log_level(2);
    iconsole_debug("FILTERED_DEBUG");
    iconsole_info("FILTERED_INFO");
    iconsole_warn("VISIBLE_WARN");
    iconsole_error("VISIBLE_ERROR_" + chr(27979) + chr(35797));
    iconsole_set_log_file(temp_directory + "missing-directory/invalid.log");
    _check(iconsole_get_log_file() == _log_path, "failed replacement preserves active log");
    iconsole_flush();
    iconsole_close_log_file();
    _check(!iconsole_log_file_open(), "log file closed");
    _check(iconsole_get_log_file() == "", "log path cleared");

    var _first_log = _read_text(_log_path);
    _check(string_pos("FILTERED_DEBUG", _first_log) == 0, "debug log filtering");
    _check(string_pos("FILTERED_INFO", _first_log) == 0, "info log filtering");
    _check(string_pos("VISIBLE_WARN", _first_log) > 0, "warn log output");
    _check(string_pos("VISIBLE_ERROR_" + chr(27979) + chr(35797), _first_log) > 0, "UTF-8 log output and path");

    iconsole_set_log_file_append(_log_path);
    _check(iconsole_log_file_open(), "append log opened");
    iconsole_log(99, "APPENDED_ERROR");
    iconsole_close_log_file();
    var _appended_log = _read_text(_log_path);
    _check(string_pos("APPENDED_ERROR", _appended_log) > 0, "append log output");
    file_delete(_log_path);

    show_debug_message(_failures == 0 ? "ICONSOLE_SELF_TEST_PASS" : "ICONSOLE_SELF_TEST_FAILURES=" + string(_failures));
    game_end();
    exit;
}

// Open console and demo logging.
iconsole_open();
iconsole_set_title("IConsole Demo");
iconsole_set_color(10);
iconsole_print_line("IConsole ready.");
iconsole_reset_color();

iconsole_set_log_level(0);
iconsole_set_timestamp(true);
iconsole_set_log_file("game.log");

iconsole_debug("debug message");
iconsole_info("game started");
iconsole_warn("low memory");
iconsole_error("save failed");

iconsole_set_log_level(1);
iconsole_debug("filtered out");
iconsole_info("info only and above");
iconsole_flush();
