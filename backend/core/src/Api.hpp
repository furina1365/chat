#pragma once

#include "auth/Types.hpp"
#include "http/Http.hpp"

#include <functional>
#include <string>
#include <vector>

class HttpRouter;
namespace auth { class Auth; }

namespace chat
{
// 投递到有界业务队列；队列满或已关闭时返回 false。
using SubmitTask = std::function<bool(std::function<void()>)>;
using AuthenticatedHandler = std::function<void(
    const auth::AuthContext &, const HttpRequest &, const std::shared_ptr<HttpResponse> &)>;

struct AuthHttpConfig
{
  std::string cookie_name = "session";
  bool secure_cookie = true;
  std::vector<std::string> allowed_origins; // 空列表仅允许同源。
  std::chrono::seconds session_idle_timeout{24 * 60 * 60};
};

enum class Access { Public, Authenticated, Administrator };

// 账号与聊天 API 共用来源/CSRF 检查、有界队列和异常转换。
// Authenticated handler 仅在 Auth 验证会话成功后执行。
HttpHandler ApiHandler(auth::Auth &auth, SubmitTask submit, const AuthHttpConfig &config,
                       Access access, AuthenticatedHandler handler);
} // namespace chat
