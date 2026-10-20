#include "ChatCompletionsClient.hpp"
#include "../ModelResponseState.hpp"
#include "../SseParser.hpp"

#include <stdexcept>
#include <utility>

namespace ai
{
  using Json = nlohmann::json;

  ChatCompletionsClient::ChatCompletionsClient(ModelConfig config, std::string api_key)
      : ModelClient(std::move(config), std::move(api_key)) {}

  const char *ChatCompletionsClient::Endpoint() const { return "/chat/completions"; }

  Json ChatCompletionsClient::RequestBody(const ChatRequest &request, bool stream) const
  {
    Json body{{"model", config_.model}, {"stream", stream}};
    Json messages = Json::array();
    for (const auto &message : request.messages)
      messages.push_back({{"role", RoleName(message.role)}, {"content", message.content}});
    body["messages"] = std::move(messages);
    if (config_.max_output_tokens) body["max_tokens"] = *config_.max_output_tokens;
    if (config_.temperature) body["temperature"] = *config_.temperature;
    if (config_.top_p) body["top_p"] = *config_.top_p;
    if (config_.provider == "deepseek") body["thinking"] = {{"type", "enabled"}};
    return body;
  }

  bool ChatCompletionsClient::ParseEvent(const SseEvent &event, ModelResponseState &state) const
  {
    if (event.data == "[DONE]")
    {
      if (!state.finish_seen)
        state.failure = Failure(ErrorCode::InvalidResponse, "模型流缺少结束原因。");
      state.terminal = true;
      return false;
    }
    auto value = Json::parse(event.data);
    if (!value.is_object()) throw std::invalid_argument("无效的流事件");
    const auto type = value.value("type", event.event);
    if (value.contains("error") || type == "error" || type == "response.failed")
    {
      state.failure = Failure(ErrorCode::Upstream, "模型服务返回错误，请稍后重试。");
      return false;
    }
    if (!value.contains("choices") || !value["choices"].is_array()) return true;
    for (const auto &choice : value["choices"])
    {
      if (choice.value("index", 0) != 0)
        continue;
      if (choice.contains("finish_reason") && !choice["finish_reason"].is_null())
      {
        const auto reason = choice["finish_reason"].get<std::string>();
        if (reason == "tool_calls" || reason == "function_call")
          throw std::domain_error("当前只支持文字回复");
        if (state.response.finish_reason != FinishReason::Refusal)
          state.response.finish_reason = Reason(reason);
        state.finish_seen = true;
      }
      const auto delta = choice.value("delta", Json::object());
      if (delta.contains("tool_calls") || delta.contains("function_call"))
        throw std::domain_error("当前只支持文字回复");
      if (delta.contains("reasoning_content") && !delta["reasoning_content"].is_null())
        if (!state.Append(delta["reasoning_content"].get<std::string>(), true))
          return false;
      if (delta.contains("refusal") && delta["refusal"].is_string())
      {
        state.response.finish_reason = FinishReason::Refusal;
        if (!state.Append(delta["refusal"].get<std::string>()))
          return false;
      }
      if (delta.contains("content") && !delta["content"].is_null())
        if (!state.Append(delta["content"].get<std::string>()))
          return false;
    }
    return true;
  }

  void ChatCompletionsClient::ParseBody(const Json &value, ModelResponseState &state) const
  {
    const auto &choice = value.at("choices").at(0);
    auto reason = choice.at("finish_reason").get<std::string>();
    if (reason == "tool_calls" || reason == "function_call")
      throw std::domain_error("当前只支持文字回复");
    state.response.finish_reason = Reason(reason);
    const auto &message = choice.at("message");
    if (message.contains("reasoning_content") && !message["reasoning_content"].is_null())
      state.Append(message["reasoning_content"].get<std::string>(), true);
    if (message.contains("refusal") && message["refusal"].is_string())
    {
      state.Append(message["refusal"].get<std::string>());
      state.response.finish_reason = FinishReason::Refusal;
    }
    else if (message.contains("content") && !message["content"].is_null())
      state.Append(message["content"].get<std::string>());
  }
} // namespace ai
