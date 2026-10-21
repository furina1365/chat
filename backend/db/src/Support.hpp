#pragma once
#include "db/MySql.hpp"
#include "db/Redis.hpp"
#include <chrono>
#include <cstdlib>
#include <stdexcept>

namespace db
{
template<class T> T Take(Result<T> result)
{
  if (auto *error = std::get_if<Error>(&result)) throw *error;
  return std::get<T>(std::move(result));
}
inline std::int64_t NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
inline std::string Password(const std::string &name)
{
  if (name.empty()) return {};
  auto *value = std::getenv(name.c_str());
  if (!value) throw std::runtime_error("未设置凭据环境变量：" + name);
  return value;
}
inline std::string Text(const std::vector<std::optional<std::string>> &row, std::size_t i)
{ return row.at(i).value_or(""); }
inline std::uint64_t Number(const std::vector<std::optional<std::string>> &row, std::size_t i)
{ return std::stoull(Text(row, i)); }
inline RedisReply Command(Redis &redis, const std::vector<std::string> &args)
{
  auto connection = Take(redis.Acquire());
  return Take(connection.Command(args));
}
} // namespace db
