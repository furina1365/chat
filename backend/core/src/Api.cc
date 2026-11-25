#include "Api.hpp"
#include "HttpSupport.hpp"
#include "auth/Auth.hpp"

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
}
HttpHandler ApiHandler(auth::Auth &auth, SubmitTask submit, const AuthHttpConfig &config,
                       Access access, AuthenticatedHandler handler)
{
  return [&auth, submit, config, access, handler](const HttpRequest &request, const auto &response) {
    if (!detail::Prepare(request, response, config)) return;
    if (!submit([&auth, config, access, request, response, handler] {
      detail::Safely(response, [&] {
        auth::AuthContext context;
        if (access != Access::Public)
        {
          auto token = Token(request, config);
          context = access == Access::Administrator ? detail::Take(auth.AuthenticateAdmin(token))
              : detail::Take(auth.Authenticate(token, detail::Header(request, "user-agent")));
          detail::Cookie(response, config, token, config.session_idle_timeout.count());
        }
        if (access == Access::Public || !response->cancelToken()->load()) handler(context, request, response);
      });
    })) detail::ReplyError(response, "503 Service Unavailable", "busy", "当前请求较多，请稍后重试。");
  };
}
} // namespace chat
