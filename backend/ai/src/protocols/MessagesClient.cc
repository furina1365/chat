#include "MessagesClient.hpp"
#include "../ModelResponseState.hpp"
#include "../SseParser.hpp"

#include <stdexcept>
#include <utility>

namespace ai
{
  using Json = nlohmann::json;

  MessagesClient::MessagesClient(ModelConfig config, std::string api_key)
      : ModelClient(std::move(config), std::move(api_key)) {}

  const char *MessagesClient::Endpoint() const { return "/messages"; }

  std::vector<std::string> MessagesClient::AuthHeaders(const std::string &key) const
  {
    return {"x-api-key: " + key, "anthropic-version: 2023-06-01"};
  }

  Json MessagesClient::RequestBody(const ChatRequest &request, bool stream) const
  {
    Json body{{"model", config_.model}, {"stream", stream}};
    Json messages = Json::array();
    std::string system;
    bool conversation_started = false;
    for (const auto &message : request.messages)
    {
      const char *role = RoleName(message.role);
      if (message.role == Role::Developer)
        throw std::domain_error("Messages 不支持 developer 角色");
      if (message.role == Role::System)
      {
        if (conversation_started)
          throw std::domain_error("Messages 的 system 消息必须放在开头");
        if (!system.empty())
          system += "\n\n";
        system += message.content;
      }
      else
      {
        messages.push_back({{"role", role}, {"content", message.content}});
        conversation_started = true;
      }
    }
    body["messages"] = std::move(messages);
    if (!system.empty())
      body["system"] = system;
    if (config_.max_output_tokens)
      body["max_tokens"] = *config_.max_output_tokens;
    else
      body["max_tokens"] = 4096;
    if (config_.temperature)
      body["temperature"] = *config_.temperature;
    if (config_.top_p)
      body["top_p"] = *config_.top_p;
    return body;
  }

  bool MessagesClient::ParseEvent(const SseEvent &event, ModelResponseState &state) const
  {
    auto value = Json::parse(event.data);
    if (!value.is_object()) throw std::invalid_argument("无效的流事件");
    const auto type = value.value("type", event.event);
    if (value.contains("error") || type == "error" || type == "response.failed")
    {
      state.failure = Failure(ErrorCode::Upstream, "模型服务返回错误，请稍后重试。");
      return false;
    }
    if (type == "content_block_start")
    {
      auto block = value.at("content_block");
      auto kind = block.value("type", "");
      if (kind == "tool_use" || kind == "server_tool_use")
        throw std::domain_error("当前只支持文字回复");
      if (kind == "text")
        return state.Append(block.value("text", ""));
      if (kind == "thinking")
        return state.Append(block.value("thinking", ""), true);
    }
    if (type == "content_block_delta" && value.at("delta").value("type", "") == "text_delta")
      return state.Append(value["delta"].at("text").get<std::string>());
    if (type == "content_block_delta" && value.at("delta").value("type", "") == "thinking_delta")
      return state.Append(value["delta"].at("thinking").get<std::string>(), true);
    if (type == "message_delta" && value.at("delta").contains("stop_reason") && !value["delta"]["stop_reason"].is_null())
    {
      auto reason = value["delta"]["stop_reason"].get<std::string>();
      if (reason == "tool_use")
        throw std::domain_error("当前只支持文字回复");
      state.response.finish_reason = Reason(reason);
      state.finish_seen = true;
    }
    if (type == "message_stop")
    {
      if (!state.finish_seen)
        state.failure = Failure(ErrorCode::InvalidResponse, "模型流缺少结束原因。");
      state.terminal = true;
      return false;
    }
    return true;
  }

  void MessagesClient::ParseBody(const Json &value, ModelResponseState &state) const
  {
    const auto &items = value.at("content");
    if (!items.is_array()) throw std::invalid_argument("回复格式无效");
    auto append = [&state](const Json &part)
    {
      auto kind = part.value("type", "");
      if (kind == "output_text" || kind == "text")
        state.Append(part.at("text").get<std::string>());
      else if (kind == "thinking")
        state.Append(part.at("thinking").get<std::string>(), true);
      else if (kind == "refusal")
      {
        state.Append(part.at("refusal").get<std::string>());
        state.response.finish_reason = FinishReason::Refusal;
      }
      else if (kind == "tool_use" || kind == "function_call")
        throw std::domain_error("当前只支持文字回复");
    };
    for (const auto &item : items) append(item);
    auto reason = value.at("stop_reason").get<std::string>();
    if (reason == "tool_use") throw std::domain_error("当前只支持文字回复");
    state.response.finish_reason = Reason(reason);
  }
} // namespace ai
