#include "http/HttpRouter.hpp"

void HttpRouter::Add(std::string method, std::string path, HttpHandler handler)
{
  routes_[{std::move(method), std::move(path)}] = std::move(handler);
}

void HttpRouter::SetFallback(HttpHandler handler)
{
  fallback_ = std::move(handler);
}

void HttpRouter::operator()(const HttpRequest &request, const std::shared_ptr<HttpResponse> &response) const
{
  auto route = routes_.find({request.method, request.path});
  if (route != routes_.end())
    route->second(request, response);
  else if (fallback_)
    fallback_(request, response);
  else
    response->Reply("404 Not Found", "{\"error\":\"Not Found\"}");
}
