#include "AuthImpl.hpp"
#include "Support.hpp"

namespace auth
{
  namespace
  {
    constexpr int admin_idle_seconds = 3600;
    const std::string columns = "id,username,email,created_at,updated_at,password_hash,disabled";
    User Target(db::MySql::Connection &connection, UserId id)
    {
      auto rows = db::Take(connection.Query("SELECT " + columns + " FROM users WHERE id=? FOR UPDATE", {id}));
      if (rows.values.empty()) throw Error{ErrorCode::NotFound, "用户不存在。"};
      return detail::ReadUser(rows.values.front());
    }
  }

  void Auth::Impl::RequireAdmin(const AuthContext &context)
  {
    if (config.admin_password_hash.empty() || context.user_id || context.session_id.size() != 64 ||
        context.session_id.find_first_not_of("0123456789abcdef") != std::string::npos)
      throw Error{ErrorCode::Unauthenticated, "请先登录管理后台。"};
    static const std::string script = R"(
      if redis.call('GET',KEYS[1])~=ARGV[1] then return 0 end
      redis.call('EXPIRE',KEYS[1],ARGV[2]); return 1
    )";
    auto reply = db::Command(redis, {"EVAL", script, "1", config.session_namespace + ":admin:" + context.session_id,
        detail::Hash(config.admin_password_hash), std::to_string(admin_idle_seconds)});
    if (reply.integer != 1) throw Error{ErrorCode::Unauthenticated, "管理登录已过期，请重新输入密码。"};
  }

  Result<SessionTicket> Auth::LoginAdmin(const std::string &password)
  {
    return detail::Guard<SessionTicket>([&] {
      impl_->Limit("admin_login", "global", true);
      if (impl_->config.admin_password_hash.empty())
        throw Error{ErrorCode::Forbidden, "请先设置管理密码。"};
      if (!detail::VerifyPassword(password, impl_->config.admin_password_hash))
        throw Error{ErrorCode::InvalidCredentials, "管理密码不正确。"};
      auto token = detail::Random();
      auto id = detail::Hash(token);
      db::Command(impl_->redis, {"SET", impl_->config.session_namespace + ":admin:" + id,
          detail::Hash(impl_->config.admin_password_hash), "EX", std::to_string(admin_idle_seconds)});
      return SessionTicket{{0, id}, token, std::chrono::system_clock::now() + std::chrono::seconds(admin_idle_seconds)};
    });
  }

  Result<AuthContext> Auth::AuthenticateAdmin(const std::string &token)
  {
    return detail::Guard<AuthContext>([&] {
      if (token.size() != 64 || token.find_first_not_of("0123456789abcdef") != token.npos)
        throw Error{ErrorCode::Unauthenticated, "请先登录管理后台。"};
      AuthContext context{0, detail::Hash(token)};
      impl_->RequireAdmin(context);
      return context;
    });
  }

  Status Auth::LogoutAdmin(const AuthContext &context)
  {
    return detail::Guard<std::monostate>([&] {
      impl_->RequireAdmin(context);
      db::Command(impl_->redis, {"DEL", impl_->config.session_namespace + ":admin:" + context.session_id});
      return std::monostate{};
    });
  }

  Result<UserPage> Auth::ListUsers(const AuthContext &context, const std::string &search, UserId before_id)
  {
    return detail::Guard<UserPage>([&] {
      impl_->RequireAdmin(context);
      if (search.size() > 254) throw Error{ErrorCode::InvalidRequest, "搜索内容过长。"};
      auto query = detail::Lower(search);
      auto connection = db::Take(impl_->mysql.Acquire());
      auto rows = db::Take(connection.Query("SELECT " + columns + " FROM users WHERE "
          "(?=0 OR id<?) AND (?='' OR LOCATE(?,username)>0 OR LOCATE(?,email)>0 OR CAST(id AS CHAR)=?) "
          "ORDER BY id DESC LIMIT 51", {before_id, before_id, query, query, query, query}));
      UserPage page;
      for (const auto &row : rows.values) page.items.push_back(detail::ReadUser(row));
      if (page.items.size() > 50)
      {
        page.items.pop_back();
        page.next_before_id = page.items.back().id;
      }
      return page;
    });
  }

  Result<User> Auth::UpdateUser(const AuthContext &context, UserId id, const std::optional<std::string> &input,
                                std::optional<bool> disabled)
  {
    return detail::Guard<User>([&] {
      impl_->RequireAdmin(context);
      impl_->Limit("admin_update", "global");
      if (!input && !disabled.has_value()) throw Error{ErrorCode::InvalidRequest, "请选择要修改的内容。"};
      auto username = input ? std::optional<std::string>(detail::Username(*input)) : std::nullopt;
      auto connection = db::Take(impl_->mysql.Acquire());
      db::Take(connection.Begin());
      auto user = Target(connection, id);
      if (disabled.has_value() && *disabled != user.disabled)
      {
        detail::Take(impl_->RevokeAll(id));
        detail::Take(impl_->InvalidateCodes(id));
        user.disabled = *disabled;
      }
      if (username) user.username = *username;
      auto now = db::NowMs();
      db::Take(connection.Execute("UPDATE users SET username=?,disabled=?,updated_at=? WHERE id=?",
          {user.username, std::int64_t(user.disabled), now, id}));
      db::Take(connection.Commit());
      user.updated_at = TimePoint(std::chrono::milliseconds(now));
      return user;
    });
  }

  Status Auth::RevokeUserSessions(const AuthContext &context, UserId id)
  {
    return detail::Guard<std::monostate>([&] {
      impl_->RequireAdmin(context);
      impl_->Limit("admin_revoke", "global");
      auto connection = db::Take(impl_->mysql.Acquire());
      db::Take(connection.Begin());
      Target(connection, id);
      detail::Take(impl_->RevokeAll(id));
      db::Take(connection.Commit());
      return std::monostate{};
    });
  }
}
