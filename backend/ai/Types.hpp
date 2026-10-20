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
// 沿用 Pi 的 API 类型命名，对应 model.json 中的 api 字段。
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
  std::string api_key_env;
  std::string model;

  // 仅从 model.json 加载；缺省或 null 表示使用上游默认值。
  std::optional<std::uint64_t> max_output_tokens;
  std::optional<double> temperature;
  std::optional<double> top_p;

  // model.json 中的 timeout_ms、connect_timeout_ms 和 max_response_bytes。
  // timeout 是调用总时长；客户端开始调用时据此计算 deadline。
  std::chrono::milliseconds timeout{60000};
  std::chrono::milliseconds connect_timeout{10000};
  std::size_t max_response_bytes = 4 * 1024 * 1024;
};

// 前端仅获取这两个字段，显示为 provider/model。
struct ModelInfo
{
  std::string provider;
  std::string model;
};

enum class Role { System, Developer, User, Assistant };

struct Message
{
  Role role;
  std::string content;
};

struct ChatRequest
{
  std::vector<Message> messages;
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
};

template <typename T>
using Result = std::variant<T, Error>;

// 每次调用使用独立的取消标记；调用方持有可写指针，客户端只读。
// 空指针表示不主动取消。网络等待期间也需检查，不仅在收到文字时检查。
using CancelToken = std::shared_ptr<const std::atomic<bool>>;
} // namespace ai
