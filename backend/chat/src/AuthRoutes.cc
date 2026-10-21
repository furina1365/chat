#include "AuthRoutes.hpp"
#include "HttpSupport.hpp"
#include "auth/Account.hpp"
#include "auth/Session.hpp"
#include "http/HttpRouter.hpp"

namespace chat
{
namespace
{
std::string Token(const HttpRequest &request, const AuthHttpConfig &config)
{
  auto cookies = detail::Header(request, "cookie");
  std::string result;
  for (std::size_t start = 0; start < cookies.size();)
  {
    auto end = cookies.find(';', start);
    auto part = cookies.substr(start, end == cookies.npos ? end : end - start);
    auto first = part.find_first_not_of(' ');
    if (first != part.npos) part.erase(0, first);
    auto prefix = config.cookie_name + "=";
    if (part.rfind(prefix, 0) == 0)
    {
      if (!result.empty()) return {};
      result = part.substr(prefix.size());
    }
    if (end == cookies.npos) break;
    start = end + 1;
  }
  return result;
}
void Cookie(const std::shared_ptr<HttpResponse> &response, const AuthHttpConfig &config, const std::string &token, std::int64_t seconds)
{
  response->SetHeader("Set-Cookie", config.cookie_name + "=" + token + "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" +
                      std::to_string(std::max<std::int64_t>(0, seconds)) + (config.secure_cookie ? "; Secure" : ""));
}
HttpHandler Anonymous(SubmitTask submit, const AuthHttpConfig &config, HttpHandler handler)
{
  return [submit, config, handler](const HttpRequest &request, const auto &response) {
    if (!detail::Prepare(request, response, config)) return;
    if (!submit([request, response, handler] { detail::Safely(response, [&] { handler(request, response); }); }))
      detail::ReplyError(response, "503 Service Unavailable", "busy", "当前请求较多，请稍后重试。");
  };
}
}
HttpHandler RequireSession(auth::Session &sessions, SubmitTask submit, const AuthHttpConfig &config, AuthenticatedHandler handler)
{
  return [&sessions, submit, config, handler](const HttpRequest &request, const auto &response) {
    if (!detail::Prepare(request, response, config)) return;
    if (!submit([&sessions, config, request, response, handler] {
      detail::Safely(response, [&] {
        auto context = detail::Take(sessions.Authenticate(Token(request, config), detail::Header(request, "user-agent")));
        if (!response->cancelToken()->load()) handler(context, request, response);
      });
    })) detail::ReplyError(response, "503 Service Unavailable", "busy", "当前请求较多，请稍后重试。");
  };
}
void RegisterAuthRoutes(HttpRouter &router, auth::Account &accounts, auth::Session &sessions, SubmitTask submit, const AuthHttpConfig &config)
{
  auto anonymous = [&](const char *path, HttpHandler handler) { router.Add("POST", path, Anonymous(submit, config, std::move(handler))); };
  auto protect = [&](const char *method, const char *path, AuthenticatedHandler handler) {
    router.Add(method, path, RequireSession(sessions, submit, config, std::move(handler)));
  };
  anonymous("/api/auth/register", [&accounts](const HttpRequest &request, const auto &response) {
    auto body = detail::Body(request, {"username", "password", "email"});
    std::optional<std::string> email;
    if (body.contains("email") && !body["email"].is_null()) email = detail::String(body, "email");
    auto result = detail::Take(accounts.Register(detail::String(body, "username"), detail::String(body, "password"), email));
    response->Reply("201 Created", detail::Json{{"user", detail::User(result.user)}, {"email_delivery", result.email_delivery == auth::EmailDelivery::Sent ? "sent" : result.email_delivery == auth::EmailDelivery::Failed ? "failed" : "not_requested"}}.dump());
  });
  anonymous("/api/auth/login", [&accounts, config](const HttpRequest &request, const auto &response) {
    auto body = detail::Body(request, {"identifier", "password"});
    auto result = detail::Take(accounts.Login(detail::String(body, "identifier"), detail::String(body, "password"), detail::Header(request, "user-agent")));
    Cookie(response, config, result.session.token, std::chrono::duration_cast<std::chrono::seconds>(result.session.expires_at - std::chrono::system_clock::now()).count());
    response->Reply("200 OK", detail::Json{{"user", detail::User(result.user)}}.dump());
  });
  // 返回接收结果后再投递邮件；邮箱是否存在和 SMTP 耗时不会改变公开响应。
  router.Add("POST", "/api/auth/password/forgot", [&accounts, submit, config](const HttpRequest &request, const auto &response) {
    if (!detail::Prepare(request, response, config)) return;
    detail::Safely(response, [&] {
      auto body = detail::Body(request, {"email"});
      auto email = detail::String(body, "email");
      if (email.size() > 254) throw std::invalid_argument("email");
      if (!submit([&accounts, email] { accounts.ForgotPassword(email); }))
        detail::ReplyError(response, "503 Service Unavailable", "busy", "当前请求较多，请稍后重试。");
      else response->Reply("202 Accepted", "{\"message\":\"如果该邮箱已绑定账号，将会收到找回验证码。\"}");
    });
  });
  anonymous("/api/auth/password/reset", [&accounts, config](const HttpRequest &request, const auto &response) {
    auto body = detail::Body(request, {"email", "code", "new_password"});
    detail::Take(accounts.ResetPassword(detail::String(body, "email"), detail::String(body, "code"), detail::String(body, "new_password")));
    Cookie(response, config, "", 0);
    response->Reply("200 OK", "{\"ok\":true}");
  });
  protect("GET", "/api/auth/me", [&accounts](const auto &context, const auto &, const auto &response) {
    response->Reply("200 OK", detail::Json{{"user", detail::User(detail::Take(accounts.Get(context)))}}.dump());
  });
  protect("POST", "/api/auth/logout", [&accounts, config](const auto &context, const auto &request, const auto &response) {
    detail::Body(request, {});
    detail::Take(accounts.Logout(context));
    Cookie(response, config, "", 0);
    response->Reply("200 OK", "{\"ok\":true}");
  });
  protect("POST", "/api/auth/logout/all", [&accounts, config](const auto &context, const auto &request, const auto &response) {
    detail::Body(request, {});
    detail::Take(accounts.LogoutAll(context));
    Cookie(response, config, "", 0);
    response->Reply("200 OK", "{\"ok\":true}");
  });
  protect("GET", "/api/auth/sessions", [&accounts](const auto &context, const auto &request, const auto &response) {
    auto query = detail::Query(request, {"cursor"});
    auto page = detail::Take(accounts.ListSessions(context, query.value("cursor", std::string("0"))));
    auto items = detail::Json::array();
    for (const auto &session : page.items)
      items.push_back({{"id", session.id}, {"user_agent", session.user_agent}, {"created_at", detail::Millis(session.created_at)},
          {"last_active_at", detail::Millis(session.last_active_at)}, {"expires_at", detail::Millis(session.expires_at)}, {"current", session.current}});
    response->Reply("200 OK", detail::Json{{"items", items}, {"next_cursor", page.next_cursor}}.dump());
  });
  protect("POST", "/api/auth/sessions/revoke", [&accounts, config](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"session_id"});
    auto id = detail::String(body, "session_id");
    detail::Take(accounts.LogoutDevice(context, id));
    if (id == context.session_id) Cookie(response, config, "", 0);
    response->Reply("200 OK", "{\"ok\":true}");
  });
  protect("POST", "/api/auth/password", [&accounts, config](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"old_password", "new_password"});
    detail::Take(accounts.ChangePassword(context, detail::String(body, "old_password"), detail::String(body, "new_password")));
    Cookie(response, config, "", 0);
    response->Reply("200 OK", "{\"ok\":true}");
  });
  protect("POST", "/api/auth/password/verify", [&accounts](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"password"});
    auto lifetime = detail::Take(accounts.VerifyPassword(context, detail::String(body, "password")));
    response->Reply("200 OK", detail::Json{{"ok", true}, {"expires_in", lifetime.count()}}.dump());
  });
  protect("POST", "/api/auth/email/code", [&accounts](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"email"});
    detail::Take(accounts.SendEmailCode(context, detail::String(body, "email")));
    response->Reply("200 OK", "{\"ok\":true}");
  });
  protect("POST", "/api/auth/email/bind", [&accounts](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"email", "code"});
    detail::Take(accounts.BindEmail(context, detail::String(body, "email"), detail::String(body, "code")));
    response->Reply("200 OK", detail::Json{{"user", detail::User(detail::Take(accounts.Get(context)))}}.dump());
  });
  protect("POST", "/api/auth/email/unbind", [&accounts](const auto &context, const auto &request, const auto &response) {
    detail::Body(request, {});
    detail::Take(accounts.UnbindEmail(context));
    response->Reply("200 OK", detail::Json{{"user", detail::User(detail::Take(accounts.Get(context)))}}.dump());
  });
}
} // namespace chat
