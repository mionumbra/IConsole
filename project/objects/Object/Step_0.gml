// 控制台输入
if (iconsole_has_input()) {
    var input = iconsole_read_line(100);
	show_debug_message(input);
    iconsole_print_line("你输入了: " + input);
}