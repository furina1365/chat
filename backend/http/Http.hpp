#pragma once

#include "ServerConfig.hpp"
#include "HttpRequest.hpp"
#include "HttpResponse.hpp"

using HttpHandler = std::function<void(const HttpRequest &, const std::shared_ptr<HttpResponse> &)>;

// handler 在连接所属 IO 线程调用；异步任务可持有 response，跨线程返回结果。
class HttpServer
{
public:
  HttpServer(const ServerConfig &config, HttpHandler handler);
  ~HttpServer();
  // 返回后 IO 线程已停止；销毁 server 前应取消并等待业务异步任务。
  int Run();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
