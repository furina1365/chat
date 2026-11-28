#include "ai/ModelClient.hpp"
#include "SseParser.hpp"
#include "ModelResponseState.hpp"
#include "common/AssistantText.hpp"
#include "common/Curl.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace ai
{
  namespace
  {
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    void InitCurl() { transport::InitCurl(); }
    struct Transfer : ModelResponseState
    {
      const ModelConfig &config;
      const CancelToken &cancel;
      long http_status = 0;
      std::string content_type, body;
      std::size_t received = 0;
      std::unique_ptr<SseParser> parser;
      std::optional<Clock::time_point> idle_deadline;
      bool idle_timed_out = false;

      Transfer(const ModelConfig &settings, const CancelToken &token, const TextSink *callback,
               const TextSink *reasoning_callback)
          : ModelResponseState(chat::kMaxAssistantTextBytes, callback, reasoning_callback),
            config(settings), cancel(token) {}
    };

    size_t Header(char *data, size_t size, size_t count, void *context) noexcept
    {
      auto &transfer = *static_cast<Transfer *>(context);
      size_t length = size * count;
      try
      {
        std::string line(data, length);
        if (line.rfind("HTTP/", 0) == 0)
        {
          auto space = line.find(' ');
          transfer.http_status = std::stol(line.substr(space + 1, 3));
          transfer.content_type.clear();
        }
        else
        {
          std::transform(line.begin(), line.end(), line.begin(), [](unsigned char c)
                         { return std::tolower(c); });
          if (line.rfind("content-type:", 0) == 0)
            transfer.content_type = line.substr(13);
        }
        return length;
      }
      catch (...)
      {
        return 0;
      }
    }
    size_t Write(char *data, size_t size, size_t count, void *context) noexcept
    {
      auto &t = *static_cast<Transfer *>(context);
      size_t length = size * count;
      try
      {
        if (t.cancel && t.cancel->load())
        {
          t.failure = Failure(ErrorCode::Cancelled, "生成已停止。");
          return 0;
        }
        if (t.sink && t.config.stream_idle_timeout && length)
          t.idle_deadline = Clock::now() + *t.config.stream_idle_timeout;
        const bool success = t.http_status >= 200 && t.http_status < 300;
        const bool stream = success && t.sink;
        const auto limit = stream ? t.config.max_stream_bytes : t.config.max_response_bytes;
        const auto accepted = std::min(length, limit - t.received);
        t.received += accepted;
        if (!stream)
        {
          if (accepted != length)
          {
            t.failure = Failure(ErrorCode::ResourceLimit, "模型响应超过大小限制。");
            return 0;
          }
          if (success) t.body.append(data, length);
          return length;
        }
        if (t.content_type.find("text/event-stream") == std::string::npos)
        {
          t.failure = Failure(ErrorCode::InvalidResponse, "模型未返回 SSE 流。");
          return 0;
        }
        // 同一次写入回调可能同时包含合规的完整事件和超限数据。
        auto status = t.parser->Feed(std::string_view(data, accepted));
        if (status == SseParser::Status::LimitExceeded)
          t.failure = Failure(ErrorCode::ResourceLimit, "模型 SSE 事件超过大小限制。");
        if (status == SseParser::Status::CallbackError && !t.failure)
          t.failure = Failure(ErrorCode::Internal, "处理模型事件失败。");
        if (status == SseParser::Status::Ok && accepted != length)
        {
          t.failure = Failure(ErrorCode::ResourceLimit, "模型 SSE 流超过大小限制。");
          return 0;
        }
        return status == SseParser::Status::Ok ? length : 0;
      }
      catch (...)
      {
        t.failure = Failure(ErrorCode::Internal, "接收模型响应失败。");
        return 0;
      }
    }
    int Progress(void *context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept
    {
      auto &t = *static_cast<Transfer *>(context);
      return t.cancel && t.cancel->load() ? 1 : 0;
    }

    bool TransferStarted(CURL *curl)
    {
      curl_off_t pretransfer = 0;
      curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME_T, &pretransfer);
      return pretransfer > 0;
    }

    CURLcode Perform(CURL *curl, Transfer &transfer)
    {
      auto multi = std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)>(curl_multi_init(), curl_multi_cleanup);
      if (!multi || curl_multi_add_handle(multi.get(), curl) != CURLM_OK)
        return CURLE_FAILED_INIT;

      constexpr std::chrono::milliseconds cancel_poll{50};
      CURLcode result = CURLE_RECV_ERROR;
      for (;;)
      {
        if (transfer.cancel && transfer.cancel->load())
        {
          result = CURLE_ABORTED_BY_CALLBACK;
          break;
        }
        int running = 0;
        if (curl_multi_perform(multi.get(), &running) != CURLM_OK)
          break;
        if (!running)
        {
          int pending = 0;
          auto *message = curl_multi_info_read(multi.get(), &pending);
          if (message && message->msg == CURLMSG_DONE)
            result = message->data.result;
          break;
        }
        auto poll = cancel_poll;
        if (transfer.sink && transfer.config.stream_idle_timeout)
        {
          auto now = Clock::now();
          // DNS、连接与协议握手完成后开始等待响应体。
          if (!transfer.idle_deadline && TransferStarted(curl))
            transfer.idle_deadline = now + *transfer.config.stream_idle_timeout;
          if (transfer.idle_deadline)
          {
            auto remaining = *transfer.idle_deadline - now;
            if (remaining <= Clock::duration::zero())
            {
              transfer.idle_timed_out = true;
              result = CURLE_OPERATION_TIMEDOUT;
              break;
            }
            poll = std::min(poll, std::chrono::ceil<std::chrono::milliseconds>(remaining));
          }
        }
        if (curl_multi_poll(multi.get(), nullptr, 0, static_cast<int>(poll.count()), nullptr) != CURLM_OK)
          break;
      }
      curl_multi_remove_handle(multi.get(), curl);
      return result;
    }

  } // namespace

  bool ModelClient::SupportsReasoningEffort() const
  {
    return config_.api == Api::OpenAICompletions && config_.provider == "deepseek" &&
        (config_.model == "deepseek-flash" || config_.model == "deepseek-v4-pro" ||
         config_.model == "deepseek-v4-flash" || config_.model == "deepseek-v4-flash-vision-exp");
  }

  Result<ChatResponse> ModelClient::Call(const ChatRequest &request, const TextSink *sink,
                                         const CancelToken &cancel, const TextSink *reasoning_sink) const
  {
    const auto &config = config_;
    const auto &key = api_key_;
    try
    {
      if (cancel && cancel->load())
        return Failure(ErrorCode::Cancelled, "生成已停止。");
      if (request.reasoning_effort && !SupportsReasoningEffort())
        return Failure(ErrorCode::UnsupportedFeature, "该模型不支持设置思考强度。");
      if (sink && !*sink)
        return Failure(ErrorCode::InvalidRequest, "流式回调为空。");
      if (request.messages.empty() || request.messages.size() > 200)
        throw std::invalid_argument("消息数量无效");
      for (const auto &message : request.messages)
      {
        if (message.content.empty()) throw std::invalid_argument("消息内容为空");
        RoleName(message.role);
      }
      const auto payload = RequestBody(request, sink != nullptr).dump();
      auto curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(curl_easy_init(), curl_easy_cleanup);
      if (!curl)
        return Failure(ErrorCode::Internal, "无法创建模型请求。");
      auto headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>(nullptr, curl_slist_free_all);
      auto add_header = [&headers](const std::string &line)
      {
        auto *next = curl_slist_append(headers.get(), line.c_str());
        if (!next)
          throw std::bad_alloc();
        headers.release();
        headers.reset(next);
      };
      add_header("Content-Type: application/json");
      add_header(sink ? "Accept: text/event-stream" : "Accept: application/json");
      for (const auto &header : AuthHeaders(key)) add_header(header);
      auto url = config.base_url + Endpoint();
      Transfer transfer(config, cancel, sink, reasoning_sink);
      if (sink)
      {
        auto parse_event = [this, &transfer](const SseEvent &event)
        {
          try { return ParseEvent(event, transfer); }
          catch (const std::domain_error &) { transfer.failure = Failure(ErrorCode::UnsupportedFeature, "当前只支持文字回复。"); }
          catch (const Json::exception &) { transfer.failure = Failure(ErrorCode::InvalidResponse, "模型流数据格式异常。"); }
          catch (const std::invalid_argument &) { transfer.failure = Failure(ErrorCode::InvalidResponse, "模型流数据格式异常。"); }
          catch (...) { transfer.failure = Failure(ErrorCode::Internal, "流式回调执行失败。"); }
          return false;
        };
        transfer.parser = std::make_unique<SseParser>(parse_event, config.max_sse_event_bytes);
      }
      curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
      curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.c_str());
      curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
      curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(config.timeout.count()));
      curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(config.connect_timeout.count()));
      curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, Write);
      curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
      curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, Header);
      curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &transfer);
      curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, Progress);
      curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
      curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
      auto code = Perform(curl.get(), transfer);
      long status = 0;
      curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
      auto with_finish_reason = [&transfer](Error error)
      {
        if (transfer.finish_seen) error.finish_reason = transfer.response.finish_reason;
        return error;
      };
      if (cancel && cancel->load())
        return with_finish_reason(Failure(ErrorCode::Cancelled, "生成已停止。"));
      if (sink && code == CURLE_OPERATION_TIMEDOUT)
      {
        if (transfer.idle_timed_out)
          return with_finish_reason(Failure(ErrorCode::StreamIdleTimeout, "模型流式响应空闲超时，请重试。"));
        if (config.connect_timeout < config.timeout && !TransferStarted(curl.get()))
          return Failure(ErrorCode::ConnectTimeout, "连接模型服务超时，请重试。");
        return with_finish_reason(Failure(ErrorCode::Timeout, "模型请求超时：已达到单次调用总时限，请重试。"));
      }
      if (status >= 300)
      {
        auto error = Failure(ErrorCode::Upstream, "模型服务暂时不可用，请稍后重试。");
        if (status == 401 || status == 403)
          error = Failure(ErrorCode::Authentication, "模型认证失败，请检查后端密钥配置。");
        else if (status == 429)
          error = Failure(ErrorCode::RateLimited, "模型请求过于频繁，请稍后重试。");
        error.http_status = static_cast<int>(status);
        return error;
      }
      if (transfer.failure)
        return with_finish_reason(std::move(*transfer.failure));
      if (code != CURLE_OK && !(sink && transfer.terminal && code == CURLE_WRITE_ERROR))
      {
        if (code == CURLE_OPERATION_TIMEDOUT)
          return with_finish_reason(Failure(ErrorCode::Timeout, "模型请求超时，请重试。"));
        if (sink && status >= 200 && status < 300 && transfer.content_type.find("text/event-stream") != std::string::npos)
          return with_finish_reason(Failure(ErrorCode::Transport, "模型流意外中断，请重新生成。"));
        return with_finish_reason(Failure(ErrorCode::Transport, "无法连接模型服务或传输中断，请重试。"));
      }
      if (sink)
      {
        transfer.parser->Finish();
        if (!transfer.terminal)
          return with_finish_reason(Failure(ErrorCode::InvalidResponse, "模型流意外中断，请重新生成。"));
      }
      else
        ParseBody(Json::parse(transfer.body), transfer);
      if (transfer.failure)
        return with_finish_reason(std::move(*transfer.failure));
      if (transfer.response.reply.empty() && transfer.response.finish_reason != FinishReason::Refusal
          && transfer.response.finish_reason != FinishReason::Length)
        return with_finish_reason(Failure(ErrorCode::InvalidResponse, "模型没有返回文字回复。"));
      return std::move(transfer.response);
    }
    catch (const std::domain_error &)
    {
      return Failure(ErrorCode::UnsupportedFeature, "当前模型不支持此消息格式。");
    }
    catch (const Json::exception &)
    {
      return Failure(ErrorCode::InvalidResponse, "模型回复格式异常。");
    }
    catch (const std::invalid_argument &)
    {
      return Failure(ErrorCode::InvalidRequest, "消息格式无效。");
    }
    catch (...)
    {
      return Failure(ErrorCode::Internal, "模型调用失败，请重试。");
    }
  }

  ModelClient::ModelClient(ModelConfig config, std::string api_key)
      : config_(std::move(config)), api_key_(std::move(api_key))
  {
    InitCurl();
  }

  std::vector<std::string> ModelClient::AuthHeaders(const std::string &key) const
  {
    return {"Authorization: Bearer " + key};
  }

  Result<ChatResponse> ModelClient::Generate(const ChatRequest &request, const CancelToken &cancel) const
  {
    return Call(request, nullptr, cancel);
  }

  Result<ChatResponse> ModelClient::Stream(const ChatRequest &request, const TextSink &sink,
                                           const CancelToken &cancel, const TextSink &reasoning_sink) const
  {
    return Call(request, &sink, cancel, reasoning_sink ? &reasoning_sink : nullptr);
  }

} // namespace ai
