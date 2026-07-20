// Open console and demo logging
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
