// web_panel.h - Web management panel for auto_review
// Copyright (C) 2026 YIZHIDIANBI (一支电笔)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#pragma once

#include <string>
#include <atomic>
#include <ctime>

// 服务器启动时间
extern std::time_t g_web_start_time;

// 启动 Web 服务器线程（阻塞调用，需在独立线程运行）
// port: 监听端口
// static_dir: 静态文件目录（存放 index.html）
// bind_addr: 绑定地址（默认 0.0.0.0，安全建议 127.0.0.1）
void run_web_server(int port, const std::string& static_dir, const std::string& bind_addr);
