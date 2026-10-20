#pragma once

#include "ai/Types.hpp"

namespace ai
{
  // 每次请求独立持有；协议解析器与公共传输层共用的结果和结束状态。
  struct ModelResponseState
  {
    std::size_t max_response_bytes;
    const TextSink *sink;
    const TextSink *reasoning_sink;
    ChatResponse response;
    std::optional<Error> failure;
    bool terminal = false, finish_seen = false;

    ModelResponseState(std::size_t limit, const TextSink *text, const TextSink *reasoning)
        : max_response_bytes(limit), sink(text), reasoning_sink(reasoning) {}

    bool Append(const std::string &text, bool reasoning = false);
  };

  Error Failure(ErrorCode code, std::string text);
  const char *RoleName(Role role);
  FinishReason Reason(const std::string &value);
} // namespace ai
