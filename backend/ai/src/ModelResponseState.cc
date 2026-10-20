#include "ModelResponseState.hpp"

#include <stdexcept>
#include <utility>

namespace ai
{
  Error Failure(ErrorCode code, std::string text)
  {
    return {code, std::move(text), {}, {}};
  }
  const char *RoleName(Role role)
  {
    switch (role)
    {
    case Role::System:
      return "system";
    case Role::Developer:
      return "developer";
    case Role::User:
      return "user";
    case Role::Assistant:
      return "assistant";
    }
    throw std::invalid_argument("无效的消息角色");
  }
  FinishReason Reason(const std::string &value)
  {
    if (value == "stop" || value == "end_turn" || value == "stop_sequence" || value == "completed")
      return FinishReason::Stop;
    if (value == "length" || value == "max_tokens" || value == "max_output_tokens")
      return FinishReason::Length;
    if (value == "refusal" || value == "content_filter")
      return FinishReason::Refusal;
    return FinishReason::Other;
  }

  bool ModelResponseState::Append(const std::string &text, bool reasoning)
  {
    if (text.size() > max_response_bytes - response.reply.size() - response.reasoning.size())
    {
      failure = Failure(ErrorCode::ResourceLimit, "模型回复超过大小限制。");
      return false;
    }
    (reasoning ? response.reasoning : response.reply) += text;
    const auto *callback = reasoning ? reasoning_sink : sink;
    if (!text.empty() && callback && !(*callback)(text))
    {
      failure = Failure(ErrorCode::Cancelled, "生成已停止。");
      return false;
    }
    return true;
  }

} // namespace ai
