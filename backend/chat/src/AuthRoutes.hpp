#pragma once

#include "auth/Types.hpp"
#include "http/Http.hpp"

#include <functional>
#include <string>
#include <vector>

class HttpRouter;
namespace auth { class Account; class Session; }

namespace chat
{
// 投递到有界业务队列；队列满或已关闭时返回 false。
using SubmitTask = std::function<bool(std::function<void()>)>;
using AuthenticatedHandler = std::function<void(
    const auth::AuthContext &, const HttpRequest &, const std::shared_ptr<HttpResponse> &)>;

struct AuthHttpConfig
{
  std::string cookie_name = "chat_session";
  bool secure_cookie = true;
  std::vector<std::string> allowed_origins; // 空列表仅允许同源。
};

// HTTP 层提取 Cookie，调 Session 鉴权后才调用 handler。
// 异步执行时须持有请求和身份副本；身份缺失返回 401，依赖不可用返回 503。
HttpHandler RequireSession(auth::Session &sessions, SubmitTask submit,
                           const AuthHttpConfig &config, AuthenticatedHandler handler);

// 注册/登录/找回允许匿名；账号管理路由统一通过 RequireSession 包装。
void RegisterAuthRoutes(HttpRouter &router, auth::Account &accounts,
                        auth::Session &sessions, SubmitTask submit,
                        const AuthHttpConfig &config);
} // namespace chat
