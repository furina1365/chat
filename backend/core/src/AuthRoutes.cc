#include "AuthRoutes.hpp"
#include "HttpSupport.hpp"
#include "auth/Auth.hpp"
#include "http/HttpRouter.hpp"

namespace chat
{
void RegisterAuthRoutes(HttpRouter &router, auth::Auth &auth, SubmitTask submit, const AuthHttpConfig &config)
{
  auto route = [&](const char *method, const char *path, Access access, AuthenticatedHandler handler) {
    router.Add(method, path, ApiHandler(auth, submit, config, access, std::move(handler)));
  };
  route("POST", "/api/auth/register", Access::Public, [&auth](const auto &, const HttpRequest &request, const auto &response) {
    auto body = detail::Body(request, {"username", "password", "email"});
    std::optional<std::string> email;
    if (body.contains("email") && !body["email"].is_null()) email = detail::String(body, "email");
    auto result = detail::Take(auth.Register(detail::String(body, "username"), detail::String(body, "password"), email));
    response->Reply("201 Created", detail::Json{{"user", detail::User(result.user)}, {"email_delivery", result.email_delivery == auth::EmailDelivery::Sent ? "sent" : result.email_delivery == auth::EmailDelivery::Failed ? "failed" : "not_requested"}}.dump());
  });
  route("POST", "/api/auth/login", Access::Public, [&auth, config](const auto &, const HttpRequest &request, const auto &response) {
    auto body = detail::Body(request, {"identifier", "password"});
    auto result = detail::Take(auth.Login(detail::String(body, "identifier"), detail::String(body, "password"), detail::Header(request, "user-agent")));
    detail::Cookie(response, config, result.session.token, config.session_idle_timeout.count());
    response->Reply("200 OK", detail::Json{{"user", detail::User(result.user)}}.dump());
  });
  // 先响应统一的受理结果，再在同一业务任务中查账号、投递邮件。
  route("POST", "/api/auth/password/forgot", Access::Public, [&auth](const auto &, const HttpRequest &request, const auto &response) {
    auto body = detail::Body(request, {"email"});
    auto email = detail::String(body, "email");
    if (email.size() > 254) throw std::invalid_argument("email");
    response->Reply("202 Accepted", "{\"message\":\"如果该邮箱已绑定账号，将会收到找回验证码。\"}");
    auth.ForgotPassword(email);
  });
  route("POST", "/api/auth/password/reset", Access::Public, [&auth, config](const auto &, const HttpRequest &request, const auto &response) {
    auto body = detail::Body(request, {"email", "code", "new_password"});
    detail::Take(auth.ResetPassword(detail::String(body, "email"), detail::String(body, "code"), detail::String(body, "new_password")));
    detail::Cookie(response, config, "", 0);
    response->Reply("200 OK", "{\"ok\":true}");
  });
  route("GET", "/api/auth/me", Access::Authenticated, [&auth](const auto &context, const auto &, const auto &response) {
    response->Reply("200 OK", detail::Json{{"user", detail::User(detail::Take(auth.Get(context)))}}.dump());
  });
  route("POST", "/api/auth/logout", Access::Authenticated, [&auth, config](const auto &context, const auto &request, const auto &response) {
    detail::Body(request, {});
    detail::Take(auth.Logout(context));
    detail::Cookie(response, config, "", 0);
    response->Reply("200 OK", "{\"ok\":true}");
  });
  route("POST", "/api/auth/logout/all", Access::Authenticated, [&auth, config](const auto &context, const auto &request, const auto &response) {
    detail::Body(request, {});
    detail::Take(auth.LogoutAll(context));
    detail::Cookie(response, config, "", 0);
    response->Reply("200 OK", "{\"ok\":true}");
  });
  route("GET", "/api/auth/sessions", Access::Authenticated, [&auth](const auto &context, const auto &request, const auto &response) {
    auto query = detail::Query(request, {"cursor"});
    auto page = detail::Take(auth.ListSessions(context, query.value("cursor", std::string("0"))));
    auto items = detail::Json::array();
    for (const auto &session : page.items)
      items.push_back({{"id", session.id}, {"user_agent", session.user_agent}, {"created_at", detail::Millis(session.created_at)},
          {"last_active_at", detail::Millis(session.last_active_at)}, {"expires_at", detail::Millis(session.expires_at)}, {"current", session.current}});
    response->Reply("200 OK", detail::Json{{"items", items}, {"next_cursor", page.next_cursor}}.dump());
  });
  route("POST", "/api/auth/sessions/revoke", Access::Authenticated, [&auth, config](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"session_id"});
    auto id = detail::String(body, "session_id");
    detail::Take(auth.LogoutDevice(context, id));
    if (id == context.session_id) detail::Cookie(response, config, "", 0);
    response->Reply("200 OK", "{\"ok\":true}");
  });
  route("POST", "/api/auth/username", Access::Authenticated, [&auth](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"username"});
    auto user = detail::Take(auth.ChangeUsername(context, detail::String(body, "username")));
    response->Reply("200 OK", detail::Json{{"user", detail::User(user)}}.dump());
  });
  route("POST", "/api/auth/password", Access::Authenticated, [&auth, config](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"old_password", "new_password"});
    detail::Take(auth.ChangePassword(context, detail::String(body, "old_password"), detail::String(body, "new_password")));
    detail::Cookie(response, config, "", 0);
    response->Reply("200 OK", "{\"ok\":true}");
  });
  route("POST", "/api/auth/password/verify", Access::Authenticated, [&auth](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"password"});
    auto lifetime = detail::Take(auth.VerifyPassword(context, detail::String(body, "password")));
    response->Reply("200 OK", detail::Json{{"ok", true}, {"expires_in", lifetime.count()}}.dump());
  });
  route("POST", "/api/auth/email/code", Access::Authenticated, [&auth](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"email"});
    detail::Take(auth.SendEmailCode(context, detail::String(body, "email")));
    response->Reply("200 OK", "{\"ok\":true}");
  });
  route("POST", "/api/auth/email/bind", Access::Authenticated, [&auth](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"email", "code"});
    detail::Take(auth.BindEmail(context, detail::String(body, "email"), detail::String(body, "code")));
    response->Reply("200 OK", detail::Json{{"user", detail::User(detail::Take(auth.Get(context)))}}.dump());
  });
  route("POST", "/api/auth/email/unbind", Access::Authenticated, [&auth](const auto &context, const auto &request, const auto &response) {
    detail::Body(request, {});
    detail::Take(auth.UnbindEmail(context));
    response->Reply("200 OK", detail::Json{{"user", detail::User(detail::Take(auth.Get(context)))}}.dump());
  });
}
} // namespace chat
