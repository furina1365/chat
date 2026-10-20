#pragma once

#include "Http.hpp"
#include <map>
#include <utility>

// 启动前注册路由，启动后只读；具体处理函数由业务层提供。
class HttpRouter
{
public:
  void Add(std::string method, std::string path, HttpHandler handler);
  void SetFallback(HttpHandler handler);
  void operator()(const HttpRequest &request, const std::shared_ptr<HttpResponse> &response) const;

private:
  std::map<std::pair<std::string, std::string>, HttpHandler> routes_;
  HttpHandler fallback_;
};
