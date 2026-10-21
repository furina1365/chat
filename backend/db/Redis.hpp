#pragma once

#include "Types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace db
{
  struct RedisConfig
  {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    std::string username;
    std::string password_env;
    int database = 0;
    std::size_t pool_size = 8;
    std::chrono::milliseconds acquire_timeout{1000};
    std::chrono::milliseconds connect_timeout{3000};
    std::chrono::milliseconds io_timeout{3000};
  };

  // 只提供连接和命令执行，业务 key 的命名与生命周期由所属模块管理。
  class Redis
  {
  public:
    class Connection
    {
    public:
      ~Connection();
      Connection(Connection &&) noexcept;
      Connection &operator=(Connection &&) noexcept;
      Connection(const Connection &) = delete;
      Connection &operator=(const Connection &) = delete;

      // 参数逐项传输，支持二进制；写入结果未知时不自动重放命令。
      Result<RedisReply> Command(const std::vector<std::string> &args);

    private:
      friend class Redis;
      struct Impl;
      explicit Connection(std::unique_ptr<Impl> impl);
      std::unique_ptr<Impl> impl_;
    };

    explicit Redis(const RedisConfig &config);
    ~Redis();
    Redis(const Redis &) = delete;
    Redis &operator=(const Redis &) = delete;

    // 可并发借用；连接独占使用，池须比借出的连接活得更久。
    Result<Connection> Acquire();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
} // namespace db
