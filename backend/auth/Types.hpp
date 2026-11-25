#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace auth
{
  using UserId = std::uint64_t;
  using SessionId = std::string;
  using TimePoint = std::chrono::system_clock::time_point;

  enum class ErrorCode
  {
    InvalidRequest,
    Unauthenticated,
    Forbidden,
    InvalidCredentials,
    ReauthenticationRequired,
    InvalidCode,
    NotFound,
    Conflict,
    RegistrationClosed,
    RateLimited,
    Unavailable,
    Timeout,
    OutcomeUnknown,
    Internal
  };

  struct Error
  {
    ErrorCode code;
    std::string message; // 可向调用方展示的脱敏说明。
  };

  template <typename T>
  using Result = std::variant<T, Error>;
  using Status = Result<std::monostate>;

  struct User
  {
    UserId id = 0;
    std::string username;
    std::optional<std::string> email; // 仅包含已验证并正式绑定的邮箱。
    TimePoint created_at{};
    TimePoint updated_at{};
    bool disabled = false;
  };

  struct UserPage
  {
    std::vector<User> items;
    UserId next_before_id = 0;
  };

  // 由 Auth 鉴权产生，不能用请求体中的用户 ID 构造可信身份。
  struct AuthContext
  {
    UserId user_id = 0;
    SessionId session_id;
  };

  struct SessionTicket
  {
    AuthContext context;
    std::string token;      // 仅创建时交付，供 HTTP 层设置 Cookie，不记日志。
    TimePoint expires_at{}; // 本次闲置到期时间；有效请求会续期。
  };

  struct DeviceSession
  {
    SessionId id;
    std::string user_agent;
    TimePoint created_at{}, last_active_at{}, expires_at{};
    bool current = false;
  };

  struct SessionPage
  {
    std::vector<DeviceSession> items;
    std::string next_cursor = "0";
  };

  enum class EmailDelivery
  {
    NotRequested,
    Sent,
    Failed
  };

  struct Registration
  {
    User user;
    EmailDelivery email_delivery = EmailDelivery::NotRequested;
  };

  struct LoginResult
  {
    User user;
    SessionTicket session;
  };

  enum class EmailPurpose
  {
    Bind,
    ResetPassword
  };
} // namespace auth
