#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace db
{
  enum class ErrorCode
  {
    InvalidArgument,
    Unavailable,
    PoolExhausted,
    Timeout,
    Conflict,
    Query,
    OutcomeUnknown,
    Internal
  };

  struct Error
  {
    ErrorCode code;
    std::string message; // 内部诊断，不直接作为 HTTP 错误返回。
    unsigned int native_code = 0;
    std::string sql_state{};
  };

  template <typename T>
  using Result = std::variant<T, Error>;
  using Status = Result<std::monostate>;

  using SqlValue = std::variant<std::nullptr_t, std::int64_t, std::uint64_t,
                                double, std::string>;
  using SqlParams = std::vector<SqlValue>;

  struct Rows
  {
    std::vector<std::string> columns;
    std::vector<std::vector<std::optional<std::string>>> values;
  };

  struct WriteResult
  {
    std::uint64_t affected_rows = 0; // SOCI MySQL 的 UPDATE 为匹配行数（CLIENT_FOUND_ROWS）。
    std::uint64_t insert_id = 0;
  };

  struct RedisReply
  {
    enum class Type
    {
      Nil,
      String,
      Integer,
      Array
    };
    Type type = Type::Nil;
    std::string text;
    std::int64_t integer = 0;
    std::vector<RedisReply> elements;
  };
} // namespace db
