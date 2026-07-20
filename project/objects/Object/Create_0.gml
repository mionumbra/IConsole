// 打开控制台
iconsole_open();
iconsole_set_color(1);
iconsole_set_title("我的游戏控制台");

// 基础输出
iconsole_print_line("游戏启动！");

// 日志级别
iconsole_set_log_level(1);  // 只显示 INFO 及以上
iconsole_log(0, "这行不会显示");   // DEBUG - 被过滤
iconsole_log(1, "游戏启动");       // INFO  - 白色
iconsole_log(2, "内存不足");       // WARN  - 黄色
iconsole_log(3, "加载失败");       // ERROR - 红色

// 时间戳
iconsole_set_timestamp(true);
iconsole_log(1, "带时间戳的日志");
// 输出: [09:57:35] [INFO]  带时间戳的日志

// 文件日志
iconsole_set_log_file("game.log");
