#pragma once

#include "Types.hpp"
#include <nlohmann/json.hpp>

namespace ai
{
  struct SseEvent;
  struct ModelResponseState;

  class ModelClient
  {
  public:
    virtual ~ModelClient() = default;
    ModelClient(const ModelClient &) = delete;
    ModelClient &operator=(const ModelClient &) = delete;

    // 调用均阻塞调用线程且不保存会话；可并发使用同一客户端。
    // 生成参数使用 config_ 中的后端配置，不接受对话请求覆盖。
    // 不支持的显式配置返回 UnsupportedFeature，不静默忽略。
    virtual Result<ChatResponse> Generate(
        const ChatRequest &request, const CancelToken &cancel) const;

    // 网页对话优先使用流式调用，按顺序回调文字增量，结束后返回完整回复。
    // 仅确认协议正常终止后返回成功；异常断流返回 Error。
    // sink 返回 false 时返回 Cancelled，回调异常转成 Internal。
    // reasoning_sink 单独交付上游可见的思考文字或摘要，不与正文混合。
    virtual Result<ChatResponse> Stream(
        const ChatRequest &request, const TextSink &sink,
        const CancelToken &cancel, const TextSink &reasoning_sink = {}) const;

  protected:
    ModelClient(ModelConfig config, std::string api_key);

    // 传输流程共用，协议差异由三个子类实现。
    virtual const char *Endpoint() const = 0;
    virtual std::vector<std::string> AuthHeaders(const std::string &key) const;
    virtual nlohmann::json RequestBody(const ChatRequest &request, bool stream) const = 0;
    virtual void ParseBody(const nlohmann::json &body, ModelResponseState &state) const = 0;
    virtual bool ParseEvent(const SseEvent &event, ModelResponseState &state) const = 0;

    const ModelConfig config_;

  private:
    Result<ChatResponse> Call(const ChatRequest &request, const TextSink *sink,
                              const CancelToken &cancel, const TextSink *reasoning_sink = nullptr) const;
    const std::string api_key_;
  };

} // namespace ai
