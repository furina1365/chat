#pragma once

#include "Types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace db
{
  class Redis;
}

namespace auth
{
  enum class SmtpSecurity
  {
    Tls,
    StartTls
  };

  struct SmtpConfig
  {
    std::string host;
    std::uint16_t port = 465;
    SmtpSecurity security = SmtpSecurity::Tls;
    std::string username;
    std::string password_env;
    std::string from_address;
    std::string from_name;
    std::string ca_file; // 留空使用系统证书；私有 SMTP 可指定 CA 文件。
    std::chrono::milliseconds timeout{10000};
  };

  struct EmailConfig
  {
    SmtpConfig smtp;
    std::string key_namespace = "auth:email";
    std::chrono::seconds code_timeout{600};
    std::chrono::seconds resend_interval{60};
    std::size_t max_code_attempts = 5;
  };

  // 管理邮箱验证码和 SMTP 投递，Redis 须比本对象活得更久。
  // 方法允许并发调用，在业务 worker 执行；账号绑定由 Account 修改。
  class Email
  {
  public:
    explicit Email(db::Redis &redis, const EmailConfig &config);
    ~Email();
    Email(const Email &) = delete;
    Email &operator=(const Email &) = delete;

    Status SendCode(UserId user_id, const std::string &email, EmailPurpose purpose);
    // 原子验证并单次消费，绑定用户、目标邮箱和用途。之后数据库操作失败需重新申请。
    Status ConsumeCode(UserId user_id, const std::string &email,
                       EmailPurpose purpose, const std::string &code);
    Status InvalidateCodes(UserId user_id);
    Status NotifyPasswordChanged(const std::string &email);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
} // namespace auth
