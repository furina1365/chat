#pragma once

#include "Types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace db
{
  struct MySqlConfig
  {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3306;
    std::string database;
    std::string username;
    std::string password;
    std::size_t pool_size = 8;
    std::chrono::milliseconds acquire_timeout{1000};
    std::chrono::seconds connect_timeout{5};
    std::chrono::seconds io_timeout{5};
  };

  class MySql
  {
  public:
    // 独占一个连接；析构时回滚未提交事务并归还健康连接。
    class Connection
    {
    public:
      ~Connection();
      Connection(Connection &&) noexcept;
      Connection &operator=(Connection &&) noexcept;
      Connection(const Connection &) = delete;
      Connection &operator=(const Connection &) = delete;

      // 执行单条 SQL，? 占位参数交给 SOCI use() 绑定（MySQL 后端为客户端模拟绑定）。
      // SOCI MySQL 缓冲完整结果；Rows 限 2000 行、累计 16 MiB，调用方须在 SQL 中分页。
      Result<Rows> Query(const std::string &sql, const SqlParams &params = {});
      Result<WriteResult> Execute(const std::string &sql, const SqlParams &params = {});
      Status Begin();
      // 提交响应丢失时返回 OutcomeUnknown，不自动重试写入。
      Status Commit();
      Status Rollback();

    private:
      friend class MySql;
      struct Impl;
      explicit Connection(std::unique_ptr<Impl> impl);
      std::unique_ptr<Impl> impl_;
    };

    // 配置与凭据加载失败抛出启动错误；运行期错误通过 Result 返回。
    explicit MySql(const MySqlConfig &config);
    ~MySql();
    MySql(const MySql &) = delete;
    MySql &operator=(const MySql &) = delete;

    // 可并发借用，等待上限由 acquire_timeout 控制；池须比连接活得更久。
    Result<Connection> Acquire();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
} // namespace db
