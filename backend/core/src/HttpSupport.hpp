#pragma once
#include "Api.hpp"
#include "auth/Types.hpp"
#include "history/Types.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>

namespace chat::detail
{
inline void Cookie(const std::shared_ptr<HttpResponse> &response, const AuthHttpConfig &config,
                   const std::string &token, std::int64_t seconds)
{
  response->SetHeader("Set-Cookie", config.cookie_name + "=" + token + "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" +
                      std::to_string(std::max<std::int64_t>(0, seconds)) + (config.secure_cookie ? "; Secure" : ""));
}
using Json = nlohmann::json;
inline void ReplyError(const std::shared_ptr<HttpResponse> &response, const std::string &status,
                       const std::string &code, const std::string &message)
{ response->Reply(status, Json{{"code", code}, {"error", message}}.dump()); }
inline void AuthError(const std::shared_ptr<HttpResponse> &response, const auth::Error &error)
{
  using C = auth::ErrorCode;
  const char *status = "503 Service Unavailable", *code = "unavailable";
  switch (error.code)
  {
    case C::InvalidRequest: status = "400 Bad Request"; code = "invalid_request"; break;
    case C::Unauthenticated: status = "401 Unauthorized"; code = "unauthenticated"; break;
    case C::Forbidden: status = "403 Forbidden"; code = "forbidden"; break;
    case C::InvalidCredentials: status = "401 Unauthorized"; code = "invalid_credentials"; break;
    case C::ReauthenticationRequired: status = "403 Forbidden"; code = "reauthentication_required"; break;
    case C::InvalidCode: status = "400 Bad Request"; code = "invalid_code"; break;
    case C::NotFound: status = "404 Not Found"; code = "not_found"; break;
    case C::Conflict: status = "409 Conflict"; code = "conflict"; break;
    case C::RegistrationClosed: status = "403 Forbidden"; code = "registration_closed"; break;
    case C::RateLimited: status = "429 Too Many Requests"; code = "rate_limited"; response->SetHeader("Retry-After", "60"); break;
    case C::Timeout: status = "504 Gateway Timeout"; code = "timeout"; break;
    case C::OutcomeUnknown: code = "outcome_unknown"; break;
    case C::Internal: status = "500 Internal Server Error"; code = "internal"; break;
    case C::Unavailable: break;
  }
  ReplyError(response, status, code, error.message);
}
inline void HistoryError(const std::shared_ptr<HttpResponse> &response, const history::Error &error)
{
  using C = history::ErrorCode;
  const char *status = "503 Service Unavailable", *code = "unavailable";
  switch (error.code)
  {
    case C::InvalidRequest: status = "400 Bad Request"; code = "invalid_request"; break;
    case C::NotFound: status = "404 Not Found"; code = "not_found"; break;
    case C::Conflict: status = "409 Conflict"; code = "conflict"; break;
    case C::Timeout: status = "504 Gateway Timeout"; code = "timeout"; break;
    case C::OutcomeUnknown: code = "outcome_unknown"; break;
    case C::Internal: status = "500 Internal Server Error"; code = "internal"; break;
    case C::Unavailable: break;
  }
  ReplyError(response, status, code, error.message);
}
inline std::string Header(const HttpRequest &request, const std::string &name)
{
  std::string result;
  for (const auto &entry : request.headers)
    if (entry.first == name) { if (!result.empty()) throw std::invalid_argument("重复请求头"); result = entry.second; }
  return result;
}
inline Json Body(const HttpRequest &request, std::initializer_list<const char *> fields)
{
  auto body = Json::parse(request.body);
  if (!body.is_object()) throw std::invalid_argument("body");
  for (const auto &entry : body.items())
    if (std::find(fields.begin(), fields.end(), entry.key()) == fields.end()) throw std::invalid_argument("field");
  return body;
}
inline std::string String(const Json &body, const char *key)
{
  auto value = body.at(key).get<std::string>();
  if (value.size() > 80000) throw std::invalid_argument("length");
  return value;
}
inline std::uint64_t Number(const std::string &value)
{
  if (value.empty() || value.size() > 20 || value.find_first_not_of("0123456789") != value.npos) throw std::invalid_argument("number");
  auto result = std::stoull(value);
  if (!result) throw std::invalid_argument("number");
  return result;
}
inline std::uint64_t Id(const Json &body, const char *key)
{ return Number(String(body, key)); }
inline Json Query(const HttpRequest &request, std::initializer_list<const char *> fields)
{
  Json query = Json::object();
  auto mark = request.path.find('?');
  if (mark == std::string::npos) return query;
  auto text = request.path.substr(mark + 1);
  std::size_t start = 0;
  while (start < text.size())
  {
    auto end = text.find('&', start);
    auto part = text.substr(start, end == text.npos ? end : end - start);
    auto equal = part.find('=');
    auto key = part.substr(0, equal);
    if (equal == part.npos || query.contains(key) || std::find(fields.begin(), fields.end(), key) == fields.end()) throw std::invalid_argument("query");
    query[key] = part.substr(equal + 1);
    if (end == text.npos) break;
    start = end + 1;
  }
  return query;
}
inline std::int64_t Millis(auth::TimePoint value)
{ return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count(); }
inline Json User(const auth::User &user)
{ return {{"id", std::to_string(user.id)}, {"username", user.username}, {"email", user.email ? Json(*user.email) : Json(nullptr)}}; }
inline Json Conversation(const history::Conversation &value)
{
  return {{"id", std::to_string(value.id)}, {"title", value.title},
          {"current_message_id", value.current_message_id ? Json(std::to_string(*value.current_message_id)) : Json(nullptr)},
          {"active_message_id", value.active_message_id ? Json(std::to_string(*value.active_message_id)) : Json(nullptr)},
          {"created_at", Millis(value.created_at)}, {"updated_at", Millis(value.updated_at)}};
}
inline Json Message(const history::Message &value)
{
  auto state = value.state == history::MessageState::Completed ? "completed" : value.state == history::MessageState::Interrupted ? "interrupted" : value.state == history::MessageState::Failed ? "failed" : "generating";
  Json result = {{"id", std::to_string(value.id)}, {"role", value.role == history::Role::User ? "user" : "assistant"},
          {"parent_id", value.parent_id ? Json(std::to_string(*value.parent_id)) : Json(nullptr)},
          {"state", state}, {"content", value.content}, {"reasoning", value.reasoning}, {"finish_reason", value.finish_reason},
          {"error", value.error}, {"interrupted", value.state != history::MessageState::Completed},
          {"truncated", value.finish_reason == "length"}};
  if (value.versions)
    result["versions"] = {{"index", value.versions->index}, {"total", value.versions->total},
        {"previous_id", value.versions->previous_id ? Json(std::to_string(*value.versions->previous_id)) : Json(nullptr)},
        {"next_id", value.versions->next_id ? Json(std::to_string(*value.versions->next_id)) : Json(nullptr)}};
  return result;
}
inline Json ConversationView(const history::ConversationView &view)
{
  Json messages = Json::array();
  for (const auto &message : view.messages) messages.push_back(Message(message));
  return {{"conversation", Conversation(view.conversation)}, {"messages", messages}};
}
inline bool Prepare(const HttpRequest &request, const std::shared_ptr<HttpResponse> &response, const AuthHttpConfig &config)
{
  response->SetHeader("Cache-Control", "no-store");
  try
  {
    auto origin = Header(request, "origin");
    auto host = Header(request, "host");
    auto expected = std::string(config.secure_cookie ? "https://" : "http://") + host;
    if (!origin.empty())
    {
      // 浏览器的同源判断基于外部 URL，不受反向代理改写 Host 或终止 TLS 影响。
      // Sec- 请求头由浏览器控制；其他来源及旧客户端继续校验 Origin。
      auto same_origin = Header(request, "sec-fetch-site") == "same-origin";
      if (!same_origin && origin != expected && std::find(config.allowed_origins.begin(), config.allowed_origins.end(), origin) == config.allowed_origins.end())
      { ReplyError(response, "403 Forbidden", "origin_rejected", "请求来源未获允许。"); return false; }
      response->SetHeader("Access-Control-Allow-Origin", origin);
      response->SetHeader("Access-Control-Allow-Credentials", "true");
      response->SetHeader("Vary", "Origin");
    }
    if (request.method == "OPTIONS")
    {
      response->SetHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
      response->SetHeader("Access-Control-Allow-Headers", "Content-Type, X-Chat-Request");
      response->Reply("204 No Content", "");
      return false;
    }
    if (request.method != "GET" && (Header(request, "x-chat-request") != "1" || Header(request, "content-type").rfind("application/json", 0) != 0))
    { ReplyError(response, "403 Forbidden", "csrf_rejected", "请求缺少安全校验信息。"); return false; }
    return true;
  }
  catch (const std::invalid_argument &) { ReplyError(response, "400 Bad Request", "invalid_request", "请求头无效。"); return false; }
}
template<class F> void Safely(const std::shared_ptr<HttpResponse> &response, F action)
{
  try { action(); }
  catch (const auth::Error &error) { AuthError(response, error); }
  catch (const history::Error &error) { HistoryError(response, error); }
  catch (const Json::exception &) { ReplyError(response, "400 Bad Request", "invalid_request", "请求参数无效。"); }
  catch (const std::invalid_argument &) { ReplyError(response, "400 Bad Request", "invalid_request", "请求参数无效。"); }
  catch (const std::out_of_range &) { ReplyError(response, "400 Bad Request", "invalid_request", "请求参数无效。"); }
  catch (...) { ReplyError(response, "500 Internal Server Error", "internal", "处理请求失败，请稍后重试。"); }
}
template<class T> T Take(auth::Result<T> result)
{
  if (auto *error = std::get_if<auth::Error>(&result)) throw *error;
  return std::get<T>(std::move(result));
}
template<class T> T HistoryTake(history::Result<T> result)
{
  if (auto *error = std::get_if<history::Error>(&result)) throw *error;
  return std::get<T>(std::move(result));
}
} // namespace chat::detail
