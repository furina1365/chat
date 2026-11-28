#include "Chat.hpp"
#include "ai/ModelManager.hpp"
#include "http/HttpRouter.hpp"
#include "src/AuthRoutes.hpp"
#include "src/AdminRoutes.hpp"
#include "src/ChatRoutes.hpp"
#include "src/HttpSupport.hpp"
#include "auth/Auth.hpp"
#include "db/src/Support.hpp"
#include "history/History.hpp"

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

  void Reject(const std::shared_ptr<HttpResponse> &response,
              const std::string &status, const std::string &message)
  {
    response->Reply(status, Json{{"error", message}}.dump());
  }

  // 模型调用阻塞 worker；HTTP IO 线程只提交任务和发送结果。
  class ChatWorkers
  {
  public:
    explicit ChatWorkers(int count, std::size_t capacity, bool finish_cancelled = false) : capacity_(capacity)
    {
      try
      {
        for (int i = 0; i < count; ++i)
          threads_.emplace_back([this] { Work(); });
        if (finish_cancelled)
          threads_.emplace_back([this] { FinishCancelled(); });
      }
      catch (...)
      {
        Stop();
        throw;
      }
    }
    ~ChatWorkers() { Stop(); }

    bool Submit(std::function<void()> run, const std::function<void()> &on_admitted = {},
                const std::shared_ptr<HttpResponse> &response = {})
    {
      auto cancel = std::make_shared<std::atomic<bool>>(false);
      auto response_cancel = response ? response->cancelToken() : nullptr;
      if (response)
        response->SetCancelHandler([this] {
          std::lock_guard<std::mutex> lock(mutex_);
          cancelled_.notify_one();
        });
      std::unique_lock<std::mutex> lock(mutex_);
      if (stopping_ || jobs_.size() + reserved_ >= capacity_)
        return false;
      if (on_admitted)
      {
        // 保留队列名额，解锁建立 SSE，准备好之后再让 worker 看到任务。
        ++reserved_;
        lock.unlock();
        try { on_admitted(); }
        catch (...)
        {
          lock.lock();
          --reserved_;
          throw;
        }
        lock.lock();
        --reserved_;
        if (stopping_) return false;
      }
      tokens_.erase(std::remove_if(tokens_.begin(), tokens_.end(), [](const auto &token)
                                  { return token.expired(); }), tokens_.end());
      tokens_.push_back(cancel);
      jobs_.push_back({std::move(cancel), std::move(run), response_cancel});
      ready_.notify_one();
      if (response_cancel && response_cancel->load())
        cancelled_.notify_one();
      return true;
    }

    void Track(const std::shared_ptr<std::atomic<bool>> &cancel)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tokens_.erase(std::remove_if(tokens_.begin(), tokens_.end(), [](const auto &token) { return token.expired(); }), tokens_.end());
      tokens_.push_back(cancel);
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
      cancelled_.notify_one();
      for (auto &thread : threads_)
        if (thread.joinable())
          thread.join();
    }

  private:
    struct Job
    {
      std::shared_ptr<std::atomic<bool>> cancel;
      std::function<void()> run;
      std::shared_ptr<std::atomic<bool>> response_cancel;
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

    void FinishCancelled()
    {
      for (;;)
      {
        Job job;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          auto found = jobs_.end();
          cancelled_.wait(lock, [&] {
            if (stopping_)
              return true;
            found = std::find_if(jobs_.begin(), jobs_.end(), [](const Job &pending) {
              return pending.response_cancel && pending.response_cancel->load();
            });
            return found != jobs_.end();
          });
          if (stopping_)
            return;
          job = std::move(*found);
          jobs_.erase(found);
        }
        // Generate 看到取消标记后只写入 interrupted，不请求上游。
        if (!job.cancel->load())
          job.run();
      }
    }

    std::mutex mutex_;
    std::condition_variable ready_, cancelled_;
    std::deque<Job> jobs_;
    std::vector<std::weak_ptr<std::atomic<bool>>> tokens_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
    std::size_t capacity_, reserved_ = 0;
  };

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
        {"/admin", {"admin.html", "text/html; charset=utf-8"}},
        {"/admin/", {"admin.html", "text/html; charset=utf-8"}},
        {"/src/admin.js", {"src/admin.js", "text/javascript; charset=utf-8"}},
        {"/src/admin.css", {"src/admin.css", "text/css; charset=utf-8"}},
        {"/src/app.js", {"src/app.js", "text/javascript; charset=utf-8"}},
        {"/src/styles.css", {"src/styles.css", "text/css; charset=utf-8"}},
        {"/vendor/markdown.js", {"vendor/markdown.js", "text/javascript; charset=utf-8"}},
        {"/vendor/katex.css", {"vendor/katex.css", "text/css; charset=utf-8"}},
        {"/favicon.svg", {"favicon.svg", "image/svg+xml"}}};
    const auto path = request.path.substr(0, request.path.find('?'));
    auto item = files.find(path);
    Asset asset;
    if (item != files.end())
      asset = item->second;
    else if (path.rfind("/c/", 0) == 0 && path.size() > 3 &&
             path.find_first_not_of("0123456789", 3) == path.npos)
      asset = {"index.html", "text/html; charset=utf-8"};
    else if (path.rfind("/vendor/fonts/", 0) == 0)
    {
      const auto name = path.substr(14);
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
  ai::ModelManager models("config.json");
  db::MySql mysql(config.mysql);
  db::Redis redis(config.redis);
  {
    auto connection = db::Take(mysql.Acquire());
    for (const auto *table : {"users", "conversations", "messages", "turn_requests"})
      if (std::holds_alternative<db::Error>(connection.Query(std::string("SELECT 1 FROM ") + table + " LIMIT 0")))
        throw std::runtime_error("Database schema is not ready; import backend/db/schema.sql first");
  }
  {
    auto connection = db::Take(mysql.Acquire());
    if (std::holds_alternative<db::Error>(connection.Query("SELECT disabled FROM users LIMIT 0")))
      throw std::runtime_error("Import backend/db/migrations/20261114-admin.sql before starting");
  }
  auth::Auth auth(mysql, redis, config.auth);
  history::History history(mysql);
  ChatWorkers business(config.business_threads, config.business_queue_size);
  ChatWorkers workers(config.worker_threads, config.model_queue_size, true);
  chat::SubmitTask business_tasks = [&business](auto task) { return business.Submit(std::move(task)); };
  chat::SubmitModelTask model_tasks = [&workers](auto task, auto on_admitted, const auto &response)
  { return workers.Submit(std::move(task), on_admitted, response); };
  chat::AuthHttpConfig http{config.cookie_name, config.secure_cookie, config.allowed_origins, config.auth.session_idle_timeout};
  HttpRouter router;
  chat::RegisterAuthRoutes(router, auth, business_tasks, http);
  chat::RegisterAdminRoutes(router, auth, business_tasks, http);
  chat::RegisterChatRoutes(router, auth, history, models, business_tasks, model_tasks, http);
  router.Add("GET", "/api/hello", [](const HttpRequest &, const auto &response)
  {
    response->Reply("200 OK", "Hello from C++ HTTP server\n", "text/plain; charset=utf-8");
  });
  router.Add("POST", "/api/echo", [](const HttpRequest &request, const auto &response)
  {
    response->Reply("200 OK", request.body, "text/plain; charset=utf-8");
  });
  router.SetFallback([http](const HttpRequest &request, const auto &response)
  {
    if (request.method == "OPTIONS")
      chat::detail::Prepare(request, response, http);
    else if (request.method == "GET")
      StaticFile(request, response);
    else
      Reject(response, "405 Method Not Allowed", "不支持此请求方法或路径。");
  });

  // Run 返回后 IO 线程已经停止；在 HTTP server 析构前取消并回收所有模型任务。
  HttpServer server(config, [&router, &workers](const HttpRequest &request, const auto &response) {
    chat::detail::Safely(response, [&] {
      workers.Track(response->cancelToken());
      router(request, response);
    });
  });
  const auto host = config.host.find(':') == std::string::npos ? config.host : "[" + config.host + "]";
  std::cout << "AI chat: http://" << host << ":" << config.port << " (" << models.List().size()
            << " models, worker threads: " << config.worker_threads << ")" << std::endl;
  int result = server.Run();
  workers.Stop();
  business.Stop();
  return result;
}
