if (iconsole_has_input()) {
    var input = iconsole_read_line(100);
    if (input != "") {
        show_debug_message(input);
        iconsole_print_line("echo: " + input);
    }
}
