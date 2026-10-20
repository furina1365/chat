#pragma once

#include <cstdint>

struct ServerConfig
{
  std::uint16_t port = 8080;
  int io_threads = 0; // 0：按 CPU 逻辑核数自动配置，不包含主线程。
  int worker_threads = 2;
};

ServerConfig LoadServerConfig(const char *path = "config.json");
