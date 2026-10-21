#pragma once

#include "Types.hpp"

#include <cstddef>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace db
{
  class MySql;
  class Redis;
}

namespace auth
{
  class Session;
  class Email;

  struct AccountConfig
  {
    bool registration_enabled = true;
    std::size_t min_password_length = 15;
    std::size_t max_password_length = 128;
    std::string key_namespace = "auth:account";
    std::size_t login_attempts = 10;
    std::chrono::seconds login_window{300};
    std::size_t operation_attempts = 20;
    std::chrono::seconds operation_window{60};
    std::chrono::seconds password_verification_timeout{300};
  };

  // 依赖对象须比 Account 活得更久。同步接口可并发调用，应在业务 worker 执行。
  // Redis 用于账号操作限流和短时密码验证；登录会话统一调用 Session。
  class Account
  {
  public:
    Account(db::MySql &mysql, db::Redis &redis, Session &sessions, Email &email,
            const AccountConfig &config = {});
    ~Account();
    Account(const Account &) = delete;
    Account &operator=(const Account &) = delete;

    // 邮件失败不撤销已创建账号，通过 Registration.email_delivery 表达结果。
    Result<Registration> Register(const std::string &username, const std::string &password,
                                  const std::optional<std::string> &email = std::nullopt);
    Result<LoginResult> Login(const std::string &identifier, const std::string &password,
                              const std::string &user_agent = {});

    // 受保护操作的身份均来自 Session::Authenticate。
    Result<User> Get(const AuthContext &context);
    Status Logout(const AuthContext &context);
    // 撤销该账号全部设备的会话，包括当前设备。
    Status LogoutAll(const AuthContext &context);
    Result<SessionPage> ListSessions(const AuthContext &context, const std::string &cursor = "0");
    Status LogoutDevice(const AuthContext &context, const SessionId &id);
    Status ChangePassword(const AuthContext &context, const std::string &old_password,
                          const std::string &new_password);
    // 验证记录仅授权当前会话的邮箱管理，返回有效时长。
    Result<std::chrono::seconds> VerifyPassword(const AuthContext &context, const std::string &password);
    Status SendEmailCode(const AuthContext &context, const std::string &email);
    Status BindEmail(const AuthContext &context, const std::string &email, const std::string &code);
    Status UnbindEmail(const AuthContext &context);

    // 对存在和不存在的邮箱提供一致的申请结果，不暴露账号是否存在。
    Status ForgotPassword(const std::string &email);
    Status ResetPassword(const std::string &email, const std::string &code,
                         const std::string &new_password);

    // 修改或重置密码成功时，全部旧会话已失效。
    // OutcomeUnknown 表示变更结果需确认，不能据此认定账号数据未改变。

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
} // namespace auth
