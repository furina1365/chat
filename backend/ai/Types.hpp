#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ai
{
// 沿用 Pi 的 API 类型命名，对应 config.json 的 models 数组中的 api 字段。
enum class Api
{
  OpenAICompletions, // openai-completions
  OpenAIResponses,   // openai-responses
  AnthropicMessages // anthropic-messages
};

struct ModelConfig
{
  std::string provider;
  Api api;
  std::string base_url;    // API 根路径，例如 https://example.com/v1。
  std::string model;

  // 仅从 config.json 的 models 数组加载；缺省或 null 表示使用上游默认值。
  std::optional<std::uint64_t> max_output_tokens;
  std::optional<double> temperature;
  std::optional<double> top_p;

  // timeout 是调用总时长；客户端开始调用时据此计算 deadline。
  std::chrono::milliseconds timeout{60000};
  std::chrono::milliseconds connect_timeout{10000};
  // 普通响应体、流式响应体和单条 SSE 事件缓冲的字节上限。
  std::size_t max_response_bytes = 4 * 1024 * 1024;
  std::size_t max_stream_bytes = 64 * 1024 * 1024;
  std::size_t max_sse_event_bytes = 256 * 1024;
};

struct ModelInfo
{
  std::string provider;
  std::string model;
  bool supports_reasoning_effort;
};

enum class Role { System, Developer, User, Assistant };
enum class ReasoningEffort { High, Max };

struct Message
{
  Role role;
  std::string content;
};

struct AssistantPrefix
{
  std::string content;
  std::string reasoning;
};

struct ChatRequest
{
  std::vector<Message> messages;
  std::optional<ReasoningEffort> reasoning_effort;
  std::optional<AssistantPrefix> prefix;
};

enum class FinishReason { Stop, Length, Refusal, Other };

struct ChatResponse
{
  std::string reply;
  FinishReason finish_reason = FinishReason::Other;
  std::string reasoning; // 上游明确提供的思考文字或摘要，与正文分开。
};

// 在调用线程内依次交付新增文字；返回 false 请求取消。
// 文字引用仅在回调期间有效。
using TextSink = std::function<bool(const std::string &)>;

enum class ErrorCode
{
  InvalidRequest, UnsupportedFeature, Authentication, RateLimited,
  Timeout, Cancelled, Transport, Upstream, InvalidResponse, ResourceLimit, Internal
};

struct Error
{
  ErrorCode code;
  std::string message; // 脱敏说明，不直接暴露密钥或上游原始错误体。
  std::optional<int> http_status;
  std::string request_id;
  std::optional<FinishReason> finish_reason = std::nullopt;
};

template <typename T>
using Result = std::variant<T, Error>;

// 每次调用使用独立的取消标记；调用方持有可写指针，客户端只读。
// 空指针表示不主动取消。网络等待期间也需检查，不仅在收到文字时检查。
using CancelToken = std::shared_ptr<const std::atomic<bool>>;
} // namespace ai
