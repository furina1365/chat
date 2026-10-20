#include "ServerConfig.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

ServerConfig LoadServerConfig(const char *path)
{
  try
  {
    std::ifstream input(path);
    if (!input)
      throw std::runtime_error("无法打开配置文件");
    using Json = nlohmann::json;
    std::set<std::string> keys;
    auto root = Json::parse(input, [&keys](int depth, Json::parse_event_t event, Json &value)
    {
      if (depth == 1 && event == Json::parse_event_t::key &&
          !keys.insert(value.get<std::string>()).second)
        throw std::runtime_error("重复配置项：" + value.get<std::string>());
      return true;
    });
    if (!root.is_object())
      throw std::runtime_error("必须是 JSON 对象");

    ServerConfig config;
    for (const auto &entry : root.items())
    {
      const auto &key = entry.key();
      if (key != "port" && key != "io_threads" && key != "worker_threads")
        throw std::runtime_error("未知配置项：" + key);
      const auto &number = entry.value();
      if (!number.is_number_integer() || number < 0 || number > std::numeric_limits<int>::max())
        throw std::runtime_error(key + " 必须是有效范围内的非负整数");
      int value = number.get<int>();
      if (key == "port")
      {
        if (value < 1 || value > 65535)
          throw std::runtime_error("port 必须在 1 到 65535 之间");
        config.port = static_cast<std::uint16_t>(value);
      }
      else if (key == "io_threads")
      {
        config.io_threads = value;
      }
      else
      {
        if (value < 1)
          throw std::runtime_error("worker_threads 必须是正整数");
        config.worker_threads = value;
      }
    }
    return config;
  }
  catch (const std::exception &error)
  {
    throw std::runtime_error(std::string(path) + "：" + error.what());
  }
}
