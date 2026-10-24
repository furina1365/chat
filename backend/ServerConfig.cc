#include "ServerConfig.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Json = nlohmann::json;

void CheckFields(const Json &object, const std::string &name,
                 std::initializer_list<const char *> fields)
{
  if (!object.is_object())
    throw std::runtime_error(name + " 必须是 JSON 对象");
  for (const auto &entry : object.items())
    if (std::find(fields.begin(), fields.end(), entry.key()) == fields.end())
      throw std::runtime_error("未知配置项：" +
                               (name.empty() ? entry.key() : name + "." + entry.key()));
}

int ReadNumber(const Json &object, const char *key, int fallback, int minimum,
               const std::string &prefix = "", int maximum = std::numeric_limits<int>::max())
{
  if (!object.contains(key))
    return fallback;
  const auto &value = object.at(key);
  if (!value.is_number_integer() || value < minimum || value > maximum)
    throw std::runtime_error(prefix + key + " 必须是 " + std::to_string(minimum) +
                             " 到 " + std::to_string(maximum) + " 之间的整数");
  return value.get<int>();
}

std::string ReadString(const Json &object, const char *key, const std::string &fallback,
                       const std::string &prefix, bool allow_empty = false)
{
  if (!object.contains(key))
    return fallback;
  const auto &value = object.at(key);
  if (!value.is_string() || (!allow_empty && value.get_ref<const std::string &>().empty()))
    throw std::runtime_error(prefix + key + (allow_empty ? " 必须是字符串" : " 必须是非空字符串"));
  return value.get<std::string>();
}

bool ReadBool(const Json &object, const char *key, bool fallback, const std::string &prefix)
{
  if (!object.contains(key)) return fallback;
  if (!object.at(key).is_boolean()) throw std::runtime_error(prefix + key + " 必须是布尔值");
  return object.at(key).get<bool>();
}

void LoadServer(const Json &object, ServerConfig &config)
{
  CheckFields(object, "server", {"host", "port", "io_threads", "worker_threads", "business_threads", "business_queue_size", "model_queue_size"});
  config.host = ReadString(object, "host", config.host, "server.");
  config.port = static_cast<std::uint16_t>(ReadNumber(object, "port", config.port, 1, "server.", 65535));
  config.io_threads = ReadNumber(object, "io_threads", config.io_threads, 0, "server.");
  config.worker_threads = ReadNumber(object, "worker_threads", config.worker_threads, 1, "server.");
  config.business_threads = ReadNumber(object, "business_threads", config.business_threads, 1, "server.", 128);
  config.business_queue_size = ReadNumber(object, "business_queue_size", config.business_queue_size, 1, "server.", 4096);
  config.model_queue_size = ReadNumber(object, "model_queue_size", config.model_queue_size, 1, "server.", 4096);
}

void LoadAuth(const Json &object, ServerConfig &config)
{
  CheckFields(object, "auth", {"registration_enabled", "min_password_length", "max_password_length",
      "key_namespace", "login_attempts", "login_window_seconds", "operation_attempts", "operation_window_seconds", "password_verification_seconds",
      "session_namespace", "session_idle_seconds", "session_absolute_seconds", "cookie_name", "secure_cookie", "allowed_origins"});
  auto &account = config.account;
  account.registration_enabled = ReadBool(object, "registration_enabled", account.registration_enabled, "auth.");
  account.min_password_length = ReadNumber(object, "min_password_length", account.min_password_length, 15, "auth.", 128);
  account.max_password_length = ReadNumber(object, "max_password_length", account.max_password_length, account.min_password_length, "auth.", 128);
  account.key_namespace = ReadString(object, "key_namespace", account.key_namespace, "auth.");
  account.login_attempts = ReadNumber(object, "login_attempts", account.login_attempts, 1, "auth.");
  account.login_window = std::chrono::seconds(ReadNumber(object, "login_window_seconds", account.login_window.count(), 1, "auth."));
  account.operation_attempts = ReadNumber(object, "operation_attempts", account.operation_attempts, 1, "auth.");
  account.operation_window = std::chrono::seconds(ReadNumber(object, "operation_window_seconds", account.operation_window.count(), 1, "auth."));
  account.password_verification_timeout = std::chrono::seconds(ReadNumber(object, "password_verification_seconds", account.password_verification_timeout.count(), 1, "auth.", 3600));
  config.session.key_namespace = ReadString(object, "session_namespace", config.session.key_namespace, "auth.");
  config.session.idle_timeout = std::chrono::seconds(ReadNumber(object, "session_idle_seconds", config.session.idle_timeout.count(), 1, "auth."));
  config.session.absolute_timeout = std::chrono::seconds(ReadNumber(object, "session_absolute_seconds", config.session.absolute_timeout.count(), 1, "auth."));
  config.cookie_name = ReadString(object, "cookie_name", config.cookie_name, "auth.");
  if (config.cookie_name.size() > 64 || config.cookie_name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != config.cookie_name.npos)
    throw std::runtime_error("auth.cookie_name 无效");
  config.secure_cookie = ReadBool(object, "secure_cookie", config.secure_cookie, "auth.");
  if (object.contains("allowed_origins"))
  {
    if (!object["allowed_origins"].is_array()) throw std::runtime_error("auth.allowed_origins 必须是数组");
    for (const auto &entry : object["allowed_origins"])
    {
      if (!entry.is_string()) throw std::runtime_error("auth.allowed_origins 必须是来源字符串数组");
      auto value = entry.get<std::string>();
      auto prefix = value.rfind("https://", 0) == 0 ? 8 : value.rfind("http://", 0) == 0 ? 7 : 0;
      if (!prefix || value.size() <= static_cast<std::size_t>(prefix) || value.find_first_of("/\r\n*?#@ ", prefix) != value.npos)
        throw std::runtime_error("auth.allowed_origins 必须填写完整的 HTTP(S) 来源，不含路径");
      config.allowed_origins.push_back(value);
    }
  }
}

void LoadEmail(const Json &object, auth::EmailConfig &config)
{
  CheckFields(object, "email", {"key_namespace", "code_timeout_seconds", "resend_interval_seconds", "max_code_attempts", "smtp"});
  config.key_namespace = ReadString(object, "key_namespace", config.key_namespace, "email.");
  config.code_timeout = std::chrono::seconds(ReadNumber(object, "code_timeout_seconds", config.code_timeout.count(), 1, "email."));
  config.resend_interval = std::chrono::seconds(ReadNumber(object, "resend_interval_seconds", config.resend_interval.count(), 1, "email."));
  config.max_code_attempts = ReadNumber(object, "max_code_attempts", config.max_code_attempts, 1, "email.", 20);
  if (!object.contains("smtp")) return;
  const auto &smtp = object["smtp"];
  CheckFields(smtp, "email.smtp", {"host", "port", "security", "username", "password", "from_address", "from_name", "ca_file", "timeout_ms"});
  config.smtp.host = ReadString(smtp, "host", config.smtp.host, "email.smtp.", true);
  config.smtp.port = ReadNumber(smtp, "port", config.smtp.port, 1, "email.smtp.", 65535);
  auto security = ReadString(smtp, "security", "tls", "email.smtp.");
  if (security != "tls" && security != "starttls") throw std::runtime_error("email.smtp.security 须为 tls 或 starttls");
  config.smtp.security = security == "tls" ? auth::SmtpSecurity::Tls : auth::SmtpSecurity::StartTls;
  config.smtp.username = ReadString(smtp, "username", config.smtp.username, "email.smtp.", true);
  config.smtp.password = ReadString(smtp, "password", config.smtp.password, "email.smtp.", true);
  config.smtp.from_address = ReadString(smtp, "from_address", config.smtp.from_address, "email.smtp.", true);
  config.smtp.from_name = ReadString(smtp, "from_name", config.smtp.from_name, "email.smtp.", true);
  config.smtp.ca_file = ReadString(smtp, "ca_file", config.smtp.ca_file, "email.smtp.", true);
  config.smtp.timeout = std::chrono::milliseconds(ReadNumber(smtp, "timeout_ms", config.smtp.timeout.count(), 1, "email.smtp.", 60000));
}

void LoadMySql(const Json &object, db::MySqlConfig &config)
{
  CheckFields(object, "mysql", {"host", "port", "database", "username", "password",
                               "pool_size", "acquire_timeout_ms", "connect_timeout_seconds",
                               "io_timeout_seconds"});
  config.host = ReadString(object, "host", config.host, "mysql.");
  config.port = static_cast<std::uint16_t>(ReadNumber(object, "port", config.port, 1, "mysql.", 65535));
  config.database = ReadString(object, "database", config.database, "mysql.");
  config.username = ReadString(object, "username", config.username, "mysql.");
  config.password = ReadString(object, "password", config.password, "mysql.", true);
  config.pool_size = ReadNumber(object, "pool_size", static_cast<int>(config.pool_size), 1, "mysql.");
  config.acquire_timeout = std::chrono::milliseconds(ReadNumber(
      object, "acquire_timeout_ms", static_cast<int>(config.acquire_timeout.count()), 1, "mysql."));
  config.connect_timeout = std::chrono::seconds(ReadNumber(
      object, "connect_timeout_seconds", static_cast<int>(config.connect_timeout.count()), 1, "mysql."));
  config.io_timeout = std::chrono::seconds(ReadNumber(
      object, "io_timeout_seconds", static_cast<int>(config.io_timeout.count()), 1, "mysql."));
}

void LoadRedis(const Json &object, db::RedisConfig &config)
{
  CheckFields(object, "redis", {"host", "port", "username", "password", "database",
                               "pool_size", "acquire_timeout_ms", "connect_timeout_ms", "io_timeout_ms"});
  config.host = ReadString(object, "host", config.host, "redis.");
  config.port = static_cast<std::uint16_t>(ReadNumber(object, "port", config.port, 1, "redis.", 65535));
  config.username = ReadString(object, "username", config.username, "redis.", true);
  config.password = ReadString(object, "password", config.password, "redis.", true);
  config.database = ReadNumber(object, "database", config.database, 0, "redis.");
  config.pool_size = ReadNumber(object, "pool_size", static_cast<int>(config.pool_size), 1, "redis.");
  config.acquire_timeout = std::chrono::milliseconds(ReadNumber(
      object, "acquire_timeout_ms", static_cast<int>(config.acquire_timeout.count()), 1, "redis."));
  config.connect_timeout = std::chrono::milliseconds(ReadNumber(
      object, "connect_timeout_ms", static_cast<int>(config.connect_timeout.count()), 1, "redis."));
  config.io_timeout = std::chrono::milliseconds(ReadNumber(
      object, "io_timeout_ms", static_cast<int>(config.io_timeout.count()), 1, "redis."));
}
} // namespace

ServerConfig LoadServerConfig(const char *path)
{
  try
  {
    std::ifstream input(path);
    if (!input)
      throw std::runtime_error("无法打开配置文件");
    std::vector<std::set<std::string>> keys;
    auto root = Json::parse(input, [&keys](int depth, Json::parse_event_t event, Json &value)
    {
      (void)depth;
      if (event == Json::parse_event_t::object_start)
        keys.emplace_back();
      else if (event == Json::parse_event_t::object_end)
        keys.pop_back();
      else if (event == Json::parse_event_t::key &&
               !keys.back().insert(value.get<std::string>()).second)
        throw std::runtime_error("重复配置项：" + value.get<std::string>());
      return true;
    });
    CheckFields(root, "", {"server", "mysql", "redis", "auth", "email", "models", "docker"});

    if (root.contains("docker"))
    {
      CheckFields(root["docker"], "docker", {"mysql_root_password"});
      ReadString(root["docker"], "mysql_root_password", "", "docker.", true);
    }
    if (root.contains("models") && !root["models"].is_array())
      throw std::runtime_error("models 必须是数组");

    ServerConfig config;
    if (root.contains("server"))
      LoadServer(root.at("server"), config);
    if (root.contains("mysql"))
      LoadMySql(root.at("mysql"), config.mysql);
    if (root.contains("redis"))
      LoadRedis(root.at("redis"), config.redis);
    if (root.contains("auth")) LoadAuth(root.at("auth"), config);
    if (root.contains("email")) LoadEmail(root.at("email"), config.email);
    return config;
  }
  catch (const Json::parse_error &)
  {
    throw std::runtime_error(std::string(path) + "：配置不是有效 JSON");
  }
  catch (const std::exception &error)
  {
    throw std::runtime_error(std::string(path) + "：" + error.what());
  }
}
