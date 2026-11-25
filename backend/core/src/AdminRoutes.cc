#include "AdminRoutes.hpp"
#include "HttpSupport.hpp"
#include "auth/Auth.hpp"
#include "http/HttpRouter.hpp"
#include <cctype>

namespace chat
{
namespace
{
std::string DecodeSearch(const std::string &input)
{
  std::string result;
  const std::string digits = "0123456789abcdef";
  for (std::size_t i = 0; i < input.size(); ++i)
  {
    if (input[i] == '+') result += ' ';
    else if (input[i] == '%')
    {
      if (i + 2 >= input.size()) throw std::invalid_argument("search");
      auto hi = digits.find(static_cast<char>(std::tolower(static_cast<unsigned char>(input[i + 1]))));
      auto lo = digits.find(static_cast<char>(std::tolower(static_cast<unsigned char>(input[i + 2]))));
      if (hi == digits.npos || lo == digits.npos) throw std::invalid_argument("search");
      result += static_cast<char>(hi * 16 + lo);
      i += 2;
    }
    else result += input[i];
  }
  return result;
}
detail::Json AdminUser(const auth::User &user)
{
  auto result = detail::User(user);
  result["disabled"] = user.disabled;
  result["created_at"] = detail::Millis(user.created_at);
  result["updated_at"] = detail::Millis(user.updated_at);
  return result;
}
}
void RegisterAdminRoutes(HttpRouter &router, auth::Auth &auth, SubmitTask submit, AuthHttpConfig config)
{
  config.cookie_name += "_admin";
  config.session_idle_timeout = std::chrono::seconds(3600);
  auto route = [&](const char *method, const char *path, Access access, AuthenticatedHandler handler) {
    router.Add(method, path, ApiHandler(auth, submit, config, access, std::move(handler)));
  };
  route("POST", "/api/admin/login", Access::Public, [&auth, config](const auto &, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"password"});
    auto ticket = detail::Take(auth.LoginAdmin(detail::String(body, "password")));
    detail::Cookie(response, config, ticket.token, config.session_idle_timeout.count());
    response->Reply("200 OK", "{\"ok\":true}");
  });
  route("GET", "/api/admin/me", Access::Administrator, [](const auto &, const auto &, const auto &response) {
    response->Reply("200 OK", "{\"ok\":true}");
  });
  route("POST", "/api/admin/logout", Access::Administrator, [&auth, config](const auto &context, const auto &request, const auto &response) {
    detail::Body(request, {});
    detail::Take(auth.LogoutAdmin(context));
    detail::Cookie(response, config, "", 0);
    response->Reply("200 OK", "{\"ok\":true}");
  });
  route("GET", "/api/admin/users", Access::Administrator, [&auth](const auto &context, const auto &request, const auto &response) {
    detail::Json query = detail::Query(request, {"search", "before_id"});
    auto page = detail::Take(auth.ListUsers(context, DecodeSearch(query.value("search", "")),
        query.contains("before_id") ? detail::Number(query.at("before_id").get<std::string>()) : 0));
    auto items = detail::Json::array();
    for (const auto &user : page.items) items.push_back(AdminUser(user));
    response->Reply("200 OK", detail::Json{{"items", items}, {"next_before_id",
        page.next_before_id ? detail::Json(std::to_string(page.next_before_id)) : detail::Json(nullptr)}}.dump());
  });
  route("POST", "/api/admin/users/update", Access::Administrator, [&auth](const auto &context, const auto &request, const auto &response) {
    detail::Json body = detail::Body(request, {"user_id", "username", "disabled"});
    auto username = body.contains("username") ? std::optional<std::string>(detail::String(body, "username")) : std::nullopt;
    auto disabled = body.contains("disabled") ? std::optional<bool>(body.at("disabled").get<bool>()) : std::nullopt;
    auto user = detail::Take(auth.UpdateUser(context, detail::Id(body, "user_id"), username, disabled));
    response->Reply("200 OK", detail::Json{{"user", AdminUser(user)}}.dump());
  });
  route("POST", "/api/admin/users/logout", Access::Administrator, [&auth](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"user_id"});
    detail::Take(auth.RevokeUserSessions(context, detail::Id(body, "user_id")));
    response->Reply("200 OK", "{\"ok\":true}");
  });
}
}
