#pragma once

#include <string>
#include <utility>
#include <vector>

struct HttpRequest
{
  std::string method, path, body;
  std::vector<std::pair<std::string, std::string>> headers;
};
