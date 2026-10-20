#include "ai/ModelClient.hpp"
#include "SseParser.hpp"
#include "ModelResponseState.hpp"

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
    struct CurlRuntime
    {
      CurlRuntime()
      {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
          throw std::runtime_error("无法初始化 libcurl");
      }
      ~CurlRuntime() { curl_global_cleanup(); }
    };
    void InitCurl() { static CurlRuntime runtime; }
    struct Transfer : ModelResponseState
    {
      const ModelConfig &config;
      const CancelToken &cancel;
      long http_status = 0;
      std::string content_type, body;
      std::size_t received = 0;
      std::unique_ptr<SseParser> parser;

      Transfer(const ModelConfig &settings, const CancelToken &token, const TextSink *callback,
               const TextSink *reasoning_callback)
          : ModelResponseState(settings.max_response_bytes, callback, reasoning_callback),
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
        if (length > t.config.max_response_bytes - t.received)
        {
          t.failure = Failure(ErrorCode::ResourceLimit, "模型响应超过大小限制。");
          return 0;
        }
        t.received += length;
        if (t.http_status < 200 || t.http_status >= 300)
          return length;
        if (!t.sink)
        {
          t.body.append(data, length);
          return length;
        }
        if (t.content_type.find("text/event-stream") == std::string::npos)
        {
          t.failure = Failure(ErrorCode::InvalidResponse, "模型未返回 SSE 流。");
          return 0;
        }
        auto status = t.parser->Feed(std::string_view(data, length));
        if (status == SseParser::Status::LimitExceeded)
          t.failure = Failure(ErrorCode::ResourceLimit, "模型事件超过大小限制。");
        if (status == SseParser::Status::CallbackError && !t.failure)
          t.failure = Failure(ErrorCode::Internal, "处理模型事件失败。");
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

  } // namespace

  Result<ChatResponse> ModelClient::Call(const ChatRequest &request, const TextSink *sink,
                                         const CancelToken &cancel, const TextSink *reasoning_sink) const
  {
    const auto &config = config_;
    const auto &key = api_key_;
    try
    {
      if (cancel && cancel->load())
        return Failure(ErrorCode::Cancelled, "生成已停止。");
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
        transfer.parser = std::make_unique<SseParser>(parse_event, config.max_response_bytes);
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
      auto code = curl_easy_perform(curl.get());
      long status = 0;
      curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
      if (cancel && cancel->load())
        return Failure(ErrorCode::Cancelled, "生成已停止。");
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
        return *transfer.failure;
      if (code != CURLE_OK && !(sink && transfer.terminal && code == CURLE_WRITE_ERROR))
        return Failure(code == CURLE_OPERATION_TIMEDOUT ? ErrorCode::Timeout : ErrorCode::Transport,
                       code == CURLE_OPERATION_TIMEDOUT ? "模型请求超时，请重试。" : "无法连接模型服务或传输中断，请重试。");
      if (sink)
      {
        transfer.parser->Finish();
        if (!transfer.terminal)
          return Failure(ErrorCode::InvalidResponse, "模型流意外中断，请重新生成。");
      }
      else
        ParseBody(Json::parse(transfer.body), transfer);
      if (transfer.failure)
        return *transfer.failure;
      if (transfer.response.reply.empty() && transfer.response.finish_reason != FinishReason::Refusal)
        return Failure(ErrorCode::InvalidResponse, "模型没有返回文字回复。");
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
