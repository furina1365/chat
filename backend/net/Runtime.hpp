#pragma once

#include <cstdint>

// 单调时钟毫秒数，供定时器计算到期时间。
std::uint64_t NowMs();
// 安装退出信号处理、忽略 SIGPIPE，并尝试提高 fd 上限。
void PrepareServerProcess();
