#pragma once
#include "auth/Types.hpp"
#include "common/Crypto.hpp"
#include "db/src/Support.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <regex>
#include <sstream>

namespace auth::detail
{
template<class T, class F> Result<T> Guard(F action)
{
  try { return action(); }
  catch (const Error &error) { return error; }
  catch (const db::Error &error)
  {
    auto code = error.code == db::ErrorCode::Conflict ? ErrorCode::Conflict
              : error.code == db::ErrorCode::OutcomeUnknown ? ErrorCode::OutcomeUnknown
              : error.code == db::ErrorCode::Timeout ? ErrorCode::Timeout : ErrorCode::Unavailable;
    return Error{code, code == ErrorCode::Conflict ? "用户名或邮箱已被使用。" :
                       code == ErrorCode::OutcomeUnknown ? "操作结果尚未确认，请重新登录或刷新后确认。" : "服务暂不可用，请稍后重试。"};
  }
  catch (...) { return Error{ErrorCode::Internal, "处理请求失败，请稍后重试。"}; }
}
template<class T> T Take(Result<T> result)
{
  if (auto *error = std::get_if<Error>(&result)) throw *error;
  return std::get<T>(std::move(result));
}
using crypto::Random;
using crypto::Hash;
using crypto::Hex;
inline std::string Lower(std::string value)
{
  auto first = value.find_first_not_of(" \t\r\n");
  if (first == value.npos) return {};
  value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  return value;
}
inline std::string EmailAddress(const std::string &input)
{
  auto value = Lower(input);
  static const std::regex format(R"([a-z0-9.!#$%&'*+/=?^_`{|}~-]+@[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?\.[a-z]{2,63})");
  if (value.size() > 254 || value.find("..") != value.npos || !std::regex_match(value, format))
    throw Error{ErrorCode::InvalidRequest, "请输入有效的邮箱地址。"};
  return value;
}
inline std::string Username(const std::string &input)
{
  auto value = Lower(input);
  static const std::regex format("[a-z][a-z0-9_]{2,31}");
  if (!std::regex_match(value, format)) throw Error{ErrorCode::InvalidRequest, "用户名为 3–32 位字母、数字或下划线，以字母开头。"};
  return value;
}
inline std::string PasswordHash(const std::string &password, const std::string &salt = Random(16))
{
  std::array<unsigned char, 32> result{};
  if (EVP_PBE_scrypt(password.data(), password.size(), reinterpret_cast<const unsigned char *>(salt.data()),
                     salt.size(), 32768, 8, 3, 64 * 1024 * 1024, result.data(), result.size()) != 1)
    throw std::runtime_error("密码哈希失败");
  return "scrypt$32768$8$3$" + salt + "$" + Hex(result.data(), result.size());
}
inline bool VerifyPassword(const std::string &password, const std::string &stored)
{
  const std::string prefix = "scrypt$32768$8$3$";
  if (password.size() > 512 || stored.size() != prefix.size() + 32 + 1 + 64 || stored.compare(0, prefix.size(), prefix)) return false;
  auto actual = PasswordHash(password, stored.substr(prefix.size(), 32));
  return CRYPTO_memcmp(actual.data(), stored.data(), actual.size()) == 0;
}
inline User ReadUser(const std::vector<std::optional<std::string>> &row)
{
  return {db::Number(row, 0), db::Text(row, 1), row.at(2),
          TimePoint(std::chrono::milliseconds(db::Number(row, 3))),
          TimePoint(std::chrono::milliseconds(db::Number(row, 4)))};
}
inline void RateLimit(db::Redis &redis, const std::string &key, std::size_t maximum, std::chrono::seconds interval)
{
  static const std::string script =
      "local n=redis.call('INCR',KEYS[1]); if n==1 then redis.call('EXPIRE',KEYS[1],ARGV[1]) end; return n";
  auto reply = db::Command(redis, {"EVAL", script, "1", key, std::to_string(interval.count())});
  if (reply.type != db::RedisReply::Type::Integer) throw std::runtime_error("无效限流结果");
  if (reply.integer > static_cast<std::int64_t>(maximum)) throw Error{ErrorCode::RateLimited, "操作过于频繁，请稍后重试。"};
}
} // namespace auth::detail
