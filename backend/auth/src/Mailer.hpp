#pragma once

#include "auth/Config.hpp"
#include "auth/Types.hpp"

namespace auth::detail
{
// 仅负责 SMTP 投递；验证码状态和用途由 Auth 管理。
class Mailer
{
public:
  explicit Mailer(const SmtpConfig &config);
  Status Send(const std::string &recipient, const std::string &subject, const std::string &text);

private:
  SmtpConfig config_;
};
} // namespace auth::detail
