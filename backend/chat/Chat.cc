#include "Chat.hpp"
#include "ai/ModelManager.hpp"
#include "http/HttpRouter.hpp"
#include "src/ChatRoutes.hpp"
#include "src/HttpSupport.hpp"
#include "auth/Account.hpp"
#include "auth/Email.hpp"
#include "auth/Session.hpp"
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
    explicit ChatWorkers(int count, std::size_t capacity) : capacity_(capacity)
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
      if (stopping_ || jobs_.size() >= capacity_)
        return false;
      tokens_.erase(std::remove_if(tokens_.begin(), tokens_.end(), [](const auto &token)
                                  { return token.expired(); }), tokens_.end());
      tokens_.push_back(cancel);
      jobs_.push_back({std::move(cancel), std::move(run)});
      ready_.notify_one();
      return true;
    }

    bool Submit(std::function<void()> run)
    { return Submit(std::make_shared<std::atomic<bool>>(false), std::move(run)); }

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
          try { job.run(); } catch (...) { std::cerr << "业务任务执行失败\n"; }
      }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    std::vector<std::weak_ptr<std::atomic<bool>>> tokens_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
    std::size_t capacity_;
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
  ai::ModelManager models("config.json");
  db::MySql mysql(config.mysql);
  db::Redis redis(config.redis);
  {
    auto connection = db::Take(mysql.Acquire());
    for (const auto *table : {"users", "conversations", "messages", "turn_requests"})
      if (std::holds_alternative<db::Error>(connection.Query(std::string("SELECT 1 FROM ") + table + " LIMIT 0")))
        throw std::runtime_error("数据库结构未就绪，请先导入 backend/db/schema.sql");
  }
  auth::Session sessions(redis, config.session);
  auth::Email email(redis, config.email);
  auth::Account accounts(mysql, redis, sessions, email, config.account);
  history::History history(mysql);
  ChatWorkers business(config.business_threads, config.business_queue_size);
  ChatWorkers workers(config.worker_threads, config.model_queue_size);
  chat::SubmitTask business_tasks = [&business](auto task) { return business.Submit(std::move(task)); };
  chat::SubmitTask model_tasks = [&workers](auto task) { return workers.Submit(std::move(task)); };
  chat::AuthHttpConfig http{config.cookie_name, config.secure_cookie, config.allowed_origins};
  HttpRouter router;
  chat::RegisterAuthRoutes(router, accounts, sessions, business_tasks, http);
  chat::RegisterChatRoutes(router, sessions, history, models, business_tasks, model_tasks, http);
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
    workers.Track(response->cancelToken());
    router(request, response);
  });
  const auto host = config.host.find(':') == std::string::npos ? config.host : "[" + config.host + "]";
  std::cout << "网页 AI 对话：http://" << host << ":" << config.port << "（" << models.List().size()
            << " 个模型，worker 线程：" << config.worker_threads << "）" << std::endl;
  int result = server.Run();
  workers.Stop();
  business.Stop();
  return result;
}
