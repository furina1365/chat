#pragma once

#include "db/MySql.hpp"
#include "db/Redis.hpp"
#include "auth/Config.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct ServerConfig
{
  std::string host = "0.0.0.0";
  std::uint16_t port = 53411;
  int io_threads = 0; // 0：按 CPU 逻辑核数自动配置，不包含主线程。
  int worker_threads = 2;
  int business_threads = 4;
  int business_queue_size = 64;
  int model_queue_size = 8;
  db::MySqlConfig mysql;
  db::RedisConfig redis;
  auth::Config auth;
  std::string cookie_name = "session";
  bool secure_cookie = true;
  std::vector<std::string> allowed_origins;
};

ServerConfig LoadServerConfig(const char *path = "config.json");
