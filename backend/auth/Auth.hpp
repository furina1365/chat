#pragma once

#include "Types.hpp"
#include "Config.hpp"

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
  // 依赖对象须比 Auth 活得更久。同步接口可并发调用，应在业务 worker 执行。
  // 统一管理账号、Redis 会话、验证码、限流和短时密码验证。
  class Auth
  {
  public:
    Auth(db::MySql &mysql, db::Redis &redis, const Config &config = {});
    ~Auth();
    Auth(const Auth &) = delete;
    Auth &operator=(const Auth &) = delete;

    // 邮件失败不撤销已创建账号，通过 Registration.email_delivery 表达结果。
    Result<Registration> Register(const std::string &username, const std::string &password,
                                  const std::optional<std::string> &email = std::nullopt);
    Result<LoginResult> Login(const std::string &identifier, const std::string &password,
                              const std::string &user_agent = {});

    // 校验会话并原子续期；受保护操作的身份均来自此接口。
    Result<AuthContext> Authenticate(const std::string &token, const std::string &user_agent = {});
    Result<User> Get(const AuthContext &context);
    Result<SessionTicket> LoginAdmin(const std::string &password);
    Result<AuthContext> AuthenticateAdmin(const std::string &token);
    Status LogoutAdmin(const AuthContext &context);
    Result<UserPage> ListUsers(const AuthContext &context, const std::string &search = {}, UserId before_id = 0);
    Result<User> UpdateUser(const AuthContext &context, UserId id, const std::optional<std::string> &username,
                            std::optional<bool> disabled);
    Status RevokeUserSessions(const AuthContext &context, UserId id);
    Status Logout(const AuthContext &context);
    // 撤销该账号全部设备的会话，包括当前设备。
    Status LogoutAll(const AuthContext &context);
    Result<SessionPage> ListSessions(const AuthContext &context, const std::string &cursor = "0");
    Status LogoutDevice(const AuthContext &context, const SessionId &id);
    Result<User> ChangeUsername(const AuthContext &context, const std::string &username);
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
