#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

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
    std::string password;
    std::string from_address;
    std::string from_name;
    std::string ca_file; // 留空使用系统证书；私有 SMTP 可指定 CA 文件。
    std::chrono::milliseconds timeout{10000};
  };

  struct Config
  {
    bool registration_enabled = true;
    std::string admin_password_hash;
    std::size_t min_password_length = 8;
    std::size_t max_password_length = 128;
    std::string key_namespace = "auth:account";
    std::size_t login_attempts = 10;
    std::chrono::seconds login_window{300};
    std::size_t operation_attempts = 20;
    std::chrono::seconds operation_window{60};
    std::chrono::seconds password_verification_timeout{300};
    std::string session_namespace = "auth:session";
    std::chrono::seconds session_idle_timeout{24 * 60 * 60};
    std::string email_namespace = "auth:email";
    std::chrono::seconds code_timeout{600};
    std::chrono::seconds resend_interval{60};
    std::size_t max_code_attempts = 5;
    SmtpConfig smtp;
  };

} // namespace auth
