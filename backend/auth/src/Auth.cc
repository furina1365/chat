#include "AuthImpl.hpp"
#include "Support.hpp"
#include <iostream>

namespace auth
{
  namespace
  {
    const std::string user_columns = "id,username,email,created_at,updated_at,password_hash,disabled";
    void ValidPassword(const std::string &password, const Config &config)
    {
      auto count = std::count_if(password.begin(), password.end(), [](unsigned char c)
                                 { return (c & 0xc0) != 0x80; });
      if (password.size() > 512 || count < static_cast<long>(config.min_password_length) || count > static_cast<long>(config.max_password_length))
        throw Error{ErrorCode::InvalidRequest, "密码长度须为 " + std::to_string(config.min_password_length) + "–" + std::to_string(config.max_password_length) + " 个字符。"};
    }
    void RequirePassword(const std::vector<std::optional<std::string>> &row, const std::string &password)
    {
      if (!detail::VerifyPassword(password, db::Text(row, 5)))
        throw Error{ErrorCode::InvalidCredentials, "账号或密码不正确。"};
    }
  }
  Auth::Impl::Impl(db::MySql &mysql, db::Redis &redis, const Config &config)
      : mysql(mysql), redis(redis), config(config), mailer(config.smtp), dummy_hash(detail::PasswordHash(detail::Random()))
  {
    if (!config.min_password_length || config.max_password_length < config.min_password_length || config.max_password_length > 128)
      throw std::invalid_argument("Invalid password length configuration");
    if (config.password_verification_timeout.count() <= 0 || config.password_verification_timeout.count() > 3600)
      throw std::invalid_argument("Invalid password verification timeout configuration");
    if (config.session_namespace.empty() || config.session_idle_timeout.count() <= 0)
      throw std::invalid_argument("Invalid session configuration");
    if (config.code_timeout.count() <= 0 || config.resend_interval.count() <= 0 || !config.max_code_attempts)
      throw std::invalid_argument("Invalid verification code configuration");
  }
  std::vector<std::optional<std::string>> Auth::Impl::LockedUser(db::MySql::Connection &connection, UserId id,
                                                             const std::string &password)
  {
    auto row = LockedUser(connection, id);
    RequirePassword(row, password);
    return row;
  }
  void Auth::Impl::Limit(const std::string &operation, const std::string &identifier, bool login)
  {
    if (login)
      detail::RateLimit(redis, config.key_namespace + ":login:global", config.login_attempts * 100, config.login_window);
    detail::RateLimit(redis, config.key_namespace + ":" + operation + ":" + detail::Hash(identifier),
                      login ? config.login_attempts : config.operation_attempts, login ? config.login_window : config.operation_window);
  }
  std::vector<std::optional<std::string>> Auth::Impl::LockedUser(db::MySql::Connection &connection, UserId id)
  {
    auto rows = db::Take(connection.Query("SELECT " + user_columns + " FROM users WHERE id=? FOR UPDATE", {id}));
    if (rows.values.empty())
      throw Error{ErrorCode::Unauthenticated, "账号不存在，请重新登录。"};
    return rows.values.front();
  }
  std::string Auth::Impl::VerificationKey(const AuthContext &context)
  {
    return config.key_namespace + ":email-verification:" + std::to_string(context.user_id) + ":" + context.session_id;
  }
  void Auth::Impl::RequireVerification(db::MySql::Connection &connection, const AuthContext &context)
  {
    auto row = LockedUser(connection, context.user_id);
    auto proof = db::Command(redis, {"GET", VerificationKey(context)});
    if (proof.type != db::RedisReply::Type::String || proof.text != detail::Hash(db::Text(row, 5)))
      throw Error{ErrorCode::ReauthenticationRequired, "请先验证当前密码，再管理邮箱。"};
  }
  Auth::Auth(db::MySql &mysql, db::Redis &redis, const Config &config)
      : impl_(std::make_unique<Impl>(mysql, redis, config)) {}
  Auth::~Auth() = default;
  Result<Registration> Auth::Register(const std::string &input, const std::string &password, const std::optional<std::string> &input_email)
  {
    return detail::Guard<Registration>([&]
                                       {
    if (!impl_->config.registration_enabled) throw Error{ErrorCode::RegistrationClosed, "暂未开放注册。"};
    auto username = detail::Username(input);
    ValidPassword(password, impl_->config);
    std::optional<std::string> email;
    if (input_email && !input_email->empty()) email = detail::EmailAddress(*input_email);
    impl_->Limit("register", "global");
    auto hash = detail::PasswordHash(password);
    auto now = db::NowMs();
    User user;
    {
      auto connection = db::Take(impl_->mysql.Acquire());
      auto result = db::Take(connection.Execute("INSERT INTO users(username,password_hash,created_at,updated_at) VALUES(?,?,?,?)", {username, hash, now, now}));
      user = {result.insert_id, username, std::nullopt, TimePoint(std::chrono::milliseconds(now)), TimePoint(std::chrono::milliseconds(now))};
    }
    auto delivery = EmailDelivery::NotRequested;
    if (email) delivery = std::holds_alternative<Error>(impl_->SendCode(user.id, *email, EmailPurpose::Bind)) ? EmailDelivery::Failed : EmailDelivery::Sent;
    return Registration{user, delivery}; });
  }
  Result<LoginResult> Auth::Login(const std::string &input, const std::string &password, const std::string &user_agent)
  {
    return detail::Guard<LoginResult>([&]
                                      {
    if (input.size() > 254 || password.size() > 512) throw Error{ErrorCode::InvalidCredentials, "账号或密码不正确。"};
    auto identifier = detail::Lower(input);
    impl_->Limit("login", identifier, true);
    auto connection = db::Take(impl_->mysql.Acquire());
    db::Take(connection.Begin());
    const auto column = identifier.find('@') == identifier.npos ? "username" : "email";
    auto rows = db::Take(connection.Query("SELECT " + user_columns + " FROM users WHERE " + column + "=? FOR UPDATE", {identifier}));
    if (rows.values.empty())
    {
      detail::VerifyPassword(password, impl_->dummy_hash);
      throw Error{ErrorCode::InvalidCredentials, "账号或密码不正确。"};
    }
    RequirePassword(rows.values.front(), password);
    auto user = detail::ReadUser(rows.values.front());
    if (user.disabled) throw Error{ErrorCode::Forbidden, "账号已停用。"};
    auto ticket = detail::Take(impl_->CreateSession(user.id, user_agent));
    db::Take(connection.Commit());
    return LoginResult{user, ticket}; });
  }
  Result<User> Auth::Get(const AuthContext &context)
  {
    return detail::Guard<User>([&]
                               {
    auto connection = db::Take(impl_->mysql.Acquire());
    auto rows = db::Take(connection.Query("SELECT " + user_columns + " FROM users WHERE id=?", {context.user_id}));
    if (rows.values.empty()) throw Error{ErrorCode::Unauthenticated, "请重新登录。"};
    return detail::ReadUser(rows.values.front()); });
  }
  Status Auth::LogoutAll(const AuthContext &context)
  {
    return detail::Guard<std::monostate>([&]
                                         {
    impl_->Limit("logout_all", std::to_string(context.user_id));
    return detail::Take(impl_->RevokeAll(context.user_id)); });
  }
  Result<User> Auth::ChangeUsername(const AuthContext &context, const std::string &input)
  {
    return detail::Guard<User>([&]
                              {
    auto username = detail::Username(input);
    impl_->Limit("username", std::to_string(context.user_id));
    auto connection = db::Take(impl_->mysql.Acquire());
    db::Take(connection.Begin());
    auto row = impl_->LockedUser(connection, context.user_id);
    auto user = detail::ReadUser(row);
    auto now = db::NowMs();
    db::Take(connection.Execute("UPDATE users SET username=?,updated_at=? WHERE id=?", {username, now, context.user_id}));
    db::Take(connection.Commit());
    user.username = username;
    user.updated_at = TimePoint(std::chrono::milliseconds(now));
    return user; });
  }
  Status Auth::ChangePassword(const AuthContext &context, const std::string &old_password, const std::string &new_password)
  {
    return detail::Guard<std::monostate>([&]
                                         {
    ValidPassword(new_password, impl_->config);
    impl_->Limit("password", std::to_string(context.user_id));
    auto hash = detail::PasswordHash(new_password);
    std::optional<std::string> email;
    {
      auto connection = db::Take(impl_->mysql.Acquire());
      db::Take(connection.Begin());
      auto row = impl_->LockedUser(connection, context.user_id, old_password);
      email = row[2];
      detail::Take(impl_->RevokeAll(context.user_id));
      detail::Take(impl_->InvalidateCodes(context.user_id));
      db::Take(connection.Execute("UPDATE users SET password_hash=?,updated_at=? WHERE id=?", {hash, db::NowMs(), context.user_id}));
      db::Take(connection.Commit());
    }
    if (email && std::holds_alternative<Error>(impl_->NotifyPasswordChanged(*email))) std::cerr << "Failed to send the password change notification\n";
    return std::monostate{}; });
  }
  Result<std::chrono::seconds> Auth::VerifyPassword(const AuthContext &context, const std::string &password)
  {
    return detail::Guard<std::chrono::seconds>([&]
                                               {
    if (password.size() > 512) throw Error{ErrorCode::InvalidCredentials, "账号或密码不正确。"};
    impl_->Limit("verify_password", std::to_string(context.user_id));
    auto connection = db::Take(impl_->mysql.Acquire());
    db::Take(connection.Begin());
    auto row = impl_->LockedUser(connection, context.user_id, password);
    db::Command(impl_->redis, {"SET", impl_->VerificationKey(context), detail::Hash(db::Text(row, 5)),
        "EX", std::to_string(impl_->config.password_verification_timeout.count())});
    db::Take(connection.Commit());
    return impl_->config.password_verification_timeout; });
  }
  Status Auth::SendEmailCode(const AuthContext &context, const std::string &input)
  {
    return detail::Guard<std::monostate>([&]
                                         {
    auto email = detail::EmailAddress(input);
    impl_->Limit("email", std::to_string(context.user_id));
    {
      auto connection = db::Take(impl_->mysql.Acquire());
      db::Take(connection.Begin());
      impl_->RequireVerification(connection, context);
      db::Take(connection.Commit());
    }
    return detail::Take(impl_->SendCode(context.user_id, email, EmailPurpose::Bind)); });
  }
  Status Auth::BindEmail(const AuthContext &context, const std::string &input, const std::string &code)
  {
    return detail::Guard<std::monostate>([&]
                                         {
    auto email = detail::EmailAddress(input);
    impl_->Limit("bind", std::to_string(context.user_id));
    auto connection = db::Take(impl_->mysql.Acquire());
    db::Take(connection.Begin());
    impl_->RequireVerification(connection, context);
    detail::Take(impl_->ConsumeCode(context.user_id, email, EmailPurpose::Bind, code));
    detail::Take(impl_->InvalidateCodes(context.user_id));
    db::Take(connection.Execute("UPDATE users SET email=?,updated_at=? WHERE id=?", {email, db::NowMs(), context.user_id}));
    db::Take(connection.Commit());
    return std::monostate{}; });
  }
  Status Auth::UnbindEmail(const AuthContext &context)
  {
    return detail::Guard<std::monostate>([&]
                                         {
    impl_->Limit("unbind", std::to_string(context.user_id));
    auto connection = db::Take(impl_->mysql.Acquire());
    db::Take(connection.Begin());
    impl_->RequireVerification(connection, context);
    detail::Take(impl_->InvalidateCodes(context.user_id));
    db::Take(connection.Execute("UPDATE users SET email=NULL,updated_at=? WHERE id=?", {db::NowMs(), context.user_id}));
    db::Take(connection.Commit());
    return std::monostate{}; });
  }
  Status Auth::ForgotPassword(const std::string &input)
  {
    return detail::Guard<std::monostate>([&]
                                         {
    auto email = detail::EmailAddress(input);
    impl_->Limit("forgot", email);
    UserId id = 0;
    {
      auto connection = db::Take(impl_->mysql.Acquire());
      auto rows = db::Take(connection.Query("SELECT id FROM users WHERE email=?", {email}));
      if (!rows.values.empty()) id = db::Number(rows.values.front(), 0);
    }
    if (id) detail::Take(impl_->SendCode(id, email, EmailPurpose::ResetPassword));
    return std::monostate{}; });
  }
  Status Auth::ResetPassword(const std::string &input, const std::string &code, const std::string &new_password)
  {
    return detail::Guard<std::monostate>([&]
                                         {
    auto email = detail::EmailAddress(input);
    ValidPassword(new_password, impl_->config);
    impl_->Limit("reset", email);
    auto hash = detail::PasswordHash(new_password);
    {
      auto connection = db::Take(impl_->mysql.Acquire());
      db::Take(connection.Begin());
      auto rows = db::Take(connection.Query("SELECT id FROM users WHERE email=? FOR UPDATE", {email}));
      if (rows.values.empty()) throw Error{ErrorCode::InvalidCode, "验证码无效或已过期，请重新申请。"};
      auto id = db::Number(rows.values.front(), 0);
      detail::Take(impl_->ConsumeCode(id, email, EmailPurpose::ResetPassword, code));
      detail::Take(impl_->RevokeAll(id));
      detail::Take(impl_->InvalidateCodes(id));
      db::Take(connection.Execute("UPDATE users SET password_hash=?,updated_at=? WHERE id=? AND email=?", {hash, db::NowMs(), id, email}));
      db::Take(connection.Commit());
    }
    if (std::holds_alternative<Error>(impl_->NotifyPasswordChanged(email))) std::cerr << "Failed to send the password change notification\n";
    return std::monostate{}; });
  }
} // namespace auth
