#include "ResponsesClient.hpp"
#include "../ModelResponseState.hpp"
#include "../SseParser.hpp"

#include <stdexcept>
#include <utility>

namespace ai
{
  using Json = nlohmann::json;

  ResponsesClient::ResponsesClient(ModelConfig config, std::string api_key)
      : ModelClient(std::move(config), std::move(api_key)) {}

  const char *ResponsesClient::Endpoint() const { return "/responses"; }

  namespace
  {
    void ResponsesFinish(const Json &value, ModelResponseState &state)
    {
      const auto status = value.value("status", "");
      if (status == "failed" || status == "cancelled")
        state.failure = Failure(ErrorCode::Upstream, "模型未能完成回复。");
      else if (status == "incomplete")
      {
        auto details = value.value("incomplete_details", Json::object());
        if (details.is_null())
          details = Json::object();
        state.response.finish_reason = Reason(details.value("reason", ""));
        if (state.response.finish_reason == FinishReason::Other)
          state.failure = Failure(ErrorCode::Upstream, "模型回复未完整生成。");
      }
      else if (status == "completed")
      {
        if (state.response.finish_reason != FinishReason::Refusal)
          state.response.finish_reason = FinishReason::Stop;
      }
      else
        state.failure = Failure(ErrorCode::InvalidResponse, "模型回复缺少完成状态。");
    }

  }

  Json ResponsesClient::RequestBody(const ChatRequest &request, bool stream) const
  {
    Json body{{"model", config_.model}, {"stream", stream}};
    Json messages = Json::array();
    for (const auto &message : request.messages)
      messages.push_back({{"role", RoleName(message.role)}, {"content", message.content}});
    body["input"] = std::move(messages);
    if (config_.max_output_tokens) body["max_output_tokens"] = *config_.max_output_tokens;
    if (config_.temperature) body["temperature"] = *config_.temperature;
    if (config_.top_p) body["top_p"] = *config_.top_p;
    return body;
  }

  bool ResponsesClient::ParseEvent(const SseEvent &event, ModelResponseState &state) const
  {
    auto value = Json::parse(event.data);
    if (!value.is_object()) throw std::invalid_argument("无效的流事件");
    const auto type = value.value("type", event.event);
    if (value.contains("error") || type == "error" || type == "response.failed")
    {
      state.failure = Failure(ErrorCode::Upstream, "模型服务返回错误，请稍后重试。");
      return false;
    }
    if (type == "response.reasoning_summary_text.delta")
      return state.Append(value.at("delta").get<std::string>(), true);
    if (type == "response.output_text.delta" || type == "response.refusal.delta")
    {
      if (type == "response.refusal.delta")
        state.response.finish_reason = FinishReason::Refusal;
      return state.Append(value.at("delta").get<std::string>());
    }
    if (type == "response.output_item.added")
    {
      const auto kind = value.at("item").value("type", "");
      if (kind != "message" && kind != "reasoning")
        throw std::domain_error("当前只支持文字回复");
    }
    if (type == "response.completed" || type == "response.incomplete")
    {
      ResponsesFinish(value.at("response"), state);
      state.terminal = true;
      return false;
    }
    return true;
  }

  void ResponsesClient::ParseBody(const Json &value, ModelResponseState &state) const
  {
    const auto &items = value.at("output");
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
    for (const auto &item : items)
    {
      if (item.value("type", "") == "message")
        for (const auto &part : item.at("content")) append(part);
      else if (item.value("type", "") == "reasoning")
      {
        for (const auto &part : item.value("summary", Json::array()))
          if (part.value("type", "") == "summary_text")
            state.Append(part.at("text").get<std::string>(), true);
      }
      else throw std::domain_error("当前只支持文字回复");
    }
    ResponsesFinish(value, state);
  }
} // namespace ai
