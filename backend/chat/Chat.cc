#include "Chat.hpp"
#include "ai/ModelManager.hpp"
#include "http/HttpRouter.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
  using Json = nlohmann::json;

  const char *FinishName(ai::FinishReason reason)
  {
    switch (reason)
    {
    case ai::FinishReason::Stop: return "stop";
    case ai::FinishReason::Length: return "length";
    case ai::FinishReason::Refusal: return "refusal";
    case ai::FinishReason::Other: return "other";
    }
    return "other";
  }

  void Reject(const std::shared_ptr<HttpResponse> &response,
              const std::string &status, const std::string &message)
  {
    response->Reply(status, Json{{"error", message}}.dump());
  }

  // 模型调用阻塞 worker；HTTP IO 线程只提交任务和发送结果。
  class ChatWorkers
  {
  public:
    explicit ChatWorkers(int count)
    {
      try
      {
        for (int i = 0; i < count; ++i)
          threads_.emplace_back([this] { Work(); });
      }
      catch (...)
      {
        Stop();
        throw;
      }
    }
    ~ChatWorkers() { Stop(); }

    bool Submit(std::shared_ptr<std::atomic<bool>> cancel, std::function<void()> run)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || jobs_.size() >= 8)
        return false;
      tokens_.erase(std::remove_if(tokens_.begin(), tokens_.end(), [](const auto &token)
                                  { return token.expired(); }), tokens_.end());
      tokens_.push_back(cancel);
      jobs_.push_back({std::move(cancel), std::move(run)});
      ready_.notify_one();
      return true;
    }

    void Stop()
    {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        for (const auto &weak : tokens_)
          if (auto token = weak.lock())
            token->store(true);
        jobs_.clear();
      }
      ready_.notify_all();
      for (auto &thread : threads_)
        if (thread.joinable())
          thread.join();
    }

  private:
    struct Job
    {
      std::shared_ptr<std::atomic<bool>> cancel;
      std::function<void()> run;
    };

    void Work()
    {
      for (;;)
      {
        Job job;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
          if (stopping_)
            return;
          job = std::move(jobs_.front());
          jobs_.pop_front();
        }
        if (!job.cancel->load())
          job.run();
      }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    std::vector<std::weak_ptr<std::atomic<bool>>> tokens_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
  };

  void GenerateReply(const ai::ModelClient *client, const ai::ChatRequest &request,
                     bool stream, const std::shared_ptr<HttpResponse> &response)
  {
    try
    {
      ai::Result<ai::ChatResponse> result;
      if (stream)
      {
        auto send_text = [response](const std::string &text)
        {
          return response->SendEvent("delta", Json{{"text", text}}.dump());
        };
        auto send_reasoning = [response](const std::string &text)
        {
          return response->SendEvent("reasoning_delta", Json{{"text", text}}.dump());
        };
        result = client->Stream(request, send_text, response->cancelToken(), send_reasoning);
      }
      else
        result = client->Generate(request, response->cancelToken());

      if (const auto *reply = std::get_if<ai::ChatResponse>(&result))
      {
        Json body{{"finish_reason", FinishName(reply->finish_reason)},
                  {"truncated", reply->finish_reason == ai::FinishReason::Length}};
        if (stream)
          response->SendEvent("done", body.dump(), true);
        else
        {
          body["reply"] = reply->reply;
          if (!reply->reasoning.empty())
            body["reasoning"] = reply->reasoning;
          response->Reply("200 OK", body.dump());
        }
      }
      else
      {
        const auto &error = std::get<ai::Error>(result);
        auto message = response->overloaded()
                           ? "浏览器接收过慢，生成已停止，请重试。" : error.message;
        if (stream)
          response->SendEvent("error", Json{{"error", message}}.dump(), true);
        else
          Reject(response, error.code == ai::ErrorCode::Timeout
                               ? "504 Gateway Timeout" : "502 Bad Gateway", message);
      }
    }
    catch (...)
    {
      if (stream)
        response->SendEvent("error", Json{{"error", "处理回复失败，请重试。"}}.dump(), true);
      else
        Reject(response, "500 Internal Server Error", "处理回复失败。");
    }
  }

  void Chat(const HttpRequest &http_request, const std::shared_ptr<HttpResponse> &response,
            const ai::ModelManager &models, ChatWorkers &workers)
  {
    ai::ChatRequest request;
    const ai::ModelClient *client = nullptr;
    bool stream = true;
    try
    {
      auto body = Json::parse(http_request.body);
      if (!body.is_object())
        throw std::invalid_argument("body");
      for (auto it = body.begin(); it != body.end(); ++it)
        if (it.key() != "messages" && it.key() != "provider" && it.key() != "model" && it.key() != "stream")
          throw std::invalid_argument("field");
      client = models.Find(body.at("provider").get<std::string>(), body.at("model").get<std::string>());
      if (!client)
      {
        Reject(response, "400 Bad Request", "选择的模型未配置。");
        return;
      }
      stream = body.value("stream", true);
      const auto &messages = body.at("messages");
      if (!messages.is_array() || messages.empty() || messages.size() > 200)
        throw std::invalid_argument("messages");
      for (std::size_t i = 0; i < messages.size(); ++i)
      {
        const auto &message = messages[i];
        if (message.at("role").get<std::string>() != (i % 2 == 0 ? "user" : "assistant"))
          throw std::invalid_argument("role");
        auto content = message.at("content").get<std::string>();
        if (content.empty() || content.find_first_not_of(" \r\n\t") == content.npos)
          throw std::invalid_argument("content");
        request.messages.push_back({i % 2 == 0 ? ai::Role::User : ai::Role::Assistant, std::move(content)});
      }
      if (request.messages.back().role != ai::Role::User)
        throw std::invalid_argument("last role");
    }
    catch (...)
    {
      Reject(response, "400 Bad Request", "请求格式无效；仅接受模型选择、消息和 stream。");
      return;
    }

    // IO 线程仍在 handler 内，尚未发送字节；队列满时可替换成普通 503。
    if (stream)
      response->StartStream();
    auto generate = [response, client, stream, request = std::move(request)]
    {
      GenerateReply(client, request, stream, response);
    };
    if (!workers.Submit(response->cancelToken(), std::move(generate)))
      Reject(response, "503 Service Unavailable", "当前请求较多，请稍后重试。");
  }

  void StaticFile(const HttpRequest &request, const std::shared_ptr<HttpResponse> &response)
  {
    struct Asset
    {
      std::string filename;
      std::string content_type;
    };
    static const std::map<std::string, Asset> files{
        {"/", {"index.html", "text/html; charset=utf-8"}},
        {"/index.html", {"index.html", "text/html; charset=utf-8"}},
        {"/src/app.js", {"src/app.js", "text/javascript; charset=utf-8"}},
        {"/src/styles.css", {"src/styles.css", "text/css; charset=utf-8"}},
        {"/vendor/markdown.js", {"vendor/markdown.js", "text/javascript; charset=utf-8"}},
        {"/vendor/katex.css", {"vendor/katex.css", "text/css; charset=utf-8"}},
        {"/favicon.svg", {"favicon.svg", "image/svg+xml"}}};
    auto item = files.find(request.path);
    Asset asset;
    if (item != files.end())
      asset = item->second;
    else if (request.path.rfind("/vendor/fonts/", 0) == 0)
    {
      const auto name = request.path.substr(14);
      if (name.rfind("KaTeX_", 0) != 0 || name.size() < 12 ||
          name.substr(name.size() - 6) != ".woff2" || name.find("..") != name.npos ||
          name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != name.npos)
      {
        Reject(response, "404 Not Found", "页面不存在。");
        return;
      }
      asset = {"vendor/fonts/" + name, "font/woff2"};
    }
    else
    {
      Reject(response, "404 Not Found", "页面不存在。");
      return;
    }
    std::ifstream file("frontend/" + asset.filename, std::ios::binary);
    if (!file)
    {
      Reject(response, "404 Not Found", "前端文件不存在，请从项目根目录启动。");
      return;
    }
    std::string body((std::istreambuf_iterator<char>(file)), {});
    response->Reply("200 OK", body, asset.content_type);
  }
}

int RunChatServer(const ServerConfig &config)
{
  ai::ModelManager models("model.json");
  ChatWorkers workers(config.worker_threads);
  HttpRouter router;
  router.Add("GET", "/api/models", [&models](const HttpRequest &, const auto &response)
  {
    Json list = Json::array();
    for (const auto &model : models.List())
      list.push_back({{"provider", model.provider}, {"model", model.model}});
    response->Reply("200 OK", Json{{"models", list}}.dump());
  });
  router.Add("GET", "/api/hello", [](const HttpRequest &, const auto &response)
  {
    response->Reply("200 OK", "Hello from C++ HTTP server\n", "text/plain; charset=utf-8");
  });
  router.Add("POST", "/api/echo", [](const HttpRequest &request, const auto &response)
  {
    response->Reply("200 OK", request.body, "text/plain; charset=utf-8");
  });
  router.Add("POST", "/api/chat", [&models, &workers](const HttpRequest &request, const auto &response)
  {
    Chat(request, response, models, workers);
  });
  router.SetFallback([](const HttpRequest &request, const auto &response)
  {
    if (request.method == "OPTIONS")
      response->Reply("204 No Content", "");
    else if (request.method == "GET")
      StaticFile(request, response);
    else
      Reject(response, "405 Method Not Allowed", "不支持此请求方法或路径。");
  });

  // Run 返回后 IO 线程已经停止；在 HTTP server 析构前取消并回收所有模型任务。
  HttpServer server(config, router);
  std::cout << "网页 AI 对话：http://localhost:" << config.port << "（" << models.List().size()
            << " 个模型，worker 线程：" << config.worker_threads << "）" << std::endl;
  int result = server.Run();
  workers.Stop();
  return result;
}
