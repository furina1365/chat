#include "db/Redis.hpp"
#include "Support.hpp"
#include <sw/redis++/redis++.h>
#include <sw/redis++/command_args.h>

namespace db
{
namespace
{
RedisReply Convert(const redisReply &reply)
{
  RedisReply value;
  switch (reply.type)
  {
    case REDIS_REPLY_NIL: break;
    case REDIS_REPLY_INTEGER: value.type = RedisReply::Type::Integer; value.integer = reply.integer; break;
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_STATUS: value.type = RedisReply::Type::String; value.text.assign(reply.str, reply.len); break;
    case REDIS_REPLY_ARRAY:
      value.type = RedisReply::Type::Array;
      for (std::size_t i = 0; i < reply.elements; ++i) value.elements.push_back(Convert(*reply.element[i]));
      break;
    default: throw std::runtime_error("Redis 返回了错误或不支持的结果");
  }
  return value;
}
}
struct Redis::Impl
{
  sw::redis::ConnectionPoolSPtr pool;
  static sw::redis::ConnectionOptions Options(const RedisConfig &config)
  {
    sw::redis::ConnectionOptions options;
    options.host = config.host;
    options.port = config.port;
    if (!config.username.empty()) options.user = config.username;
    options.password = Password(config.password_env);
    options.db = config.database;
    options.connect_timeout = config.connect_timeout;
    options.socket_timeout = config.io_timeout;
    return options;
  }
  static sw::redis::ConnectionPoolOptions PoolOptions(const RedisConfig &config)
  {
    if (!config.pool_size || config.acquire_timeout.count() <= 0)
      throw std::invalid_argument("无效的 Redis 连接池配置");
    sw::redis::ConnectionPoolOptions options;
    options.size = config.pool_size;
    options.wait_timeout = config.acquire_timeout;
    return options;
  }
  explicit Impl(const RedisConfig &config)
      : pool(std::make_shared<sw::redis::ConnectionPool>(PoolOptions(config), Options(config)))
  {
    sw::redis::SafeConnection lease(*pool);
    sw::redis::CmdArgs command;
    command.append("PING");
    lease.connection().send(command);
    lease.connection().recv();
  }
};
struct Redis::Connection::Impl
{
  sw::redis::GuardedConnection lease;
  explicit Impl(const sw::redis::ConnectionPoolSPtr &pool) : lease(pool) {}
};
Redis::Redis(const RedisConfig &config) : impl_(std::make_unique<Impl>(config)) {}
Redis::~Redis() = default;
Redis::Connection::Connection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Redis::Connection::~Connection() = default;
Redis::Connection::Connection(Connection &&) noexcept = default;
Redis::Connection &Redis::Connection::operator=(Connection &&) noexcept = default;
Result<Redis::Connection> Redis::Acquire()
{
  try
  {
    return Connection(std::make_unique<Connection::Impl>(impl_->pool));
  }
  catch (const sw::redis::TimeoutError &) { return Error{ErrorCode::Timeout, "Redis 连接超时"}; }
  catch (const sw::redis::Error &error)
  {
    // 1.3.15 的连接池等待超时抛通用 Error，尚未提供独立异常类型。
    if (std::string(error.what()).find("Failed to fetch a connection in ") == 0)
      return Error{ErrorCode::PoolExhausted, "Redis 连接池繁忙"};
    return Error{ErrorCode::Unavailable, "Redis 连接不可用"};
  }
  catch (...) { return Error{ErrorCode::Unavailable, "Redis 连接不可用"}; }
}
Result<RedisReply> Redis::Connection::Command(const std::vector<std::string> &args)
{
  if (args.empty() || args.size() > 1024) return Error{ErrorCode::InvalidArgument, "无效 Redis 命令"};
  if (impl_->lease.connection().broken()) return Error{ErrorCode::Unavailable, "Redis 连接不可用"};
  try
  {
    sw::redis::CmdArgs command;
    for (const auto &arg : args) command.append(arg);
    impl_->lease.connection().send(command);
    auto reply = impl_->lease.connection().recv();
    return Convert(*reply);
  }
  catch (const sw::redis::ReplyError &) { return Error{ErrorCode::Query, "Redis 命令执行失败"}; }
  catch (const sw::redis::Error &)
  {
    impl_->lease.connection().invalidate();
    return Error{ErrorCode::OutcomeUnknown, "Redis 命令结果未知"};
  }
  catch (...) { return Error{ErrorCode::Internal, "读取 Redis 结果失败"}; }
}
} // namespace db
