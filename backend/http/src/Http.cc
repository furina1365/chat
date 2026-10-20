#include "http/Http.hpp"
#include "HttpParser.hpp"
#include "net/Net.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>

class HttpConnection final : public TcpConnection
{
public:
  HttpConnection(EventLoop *loop, int fd, const HttpHandler &handler)
      : TcpConnection(loop, fd), loop_(loop), handler_(handler), response_(std::make_shared<HttpResponse>())
  {
    response_->owner_ = this;
    std::weak_ptr<HttpResponse> weak = response_;
    response_->wake_ = [loop, weak]
    {
      auto resume = [weak]
      {
        auto state = weak.lock();
        if (!state)
          return;
        {
          std::lock_guard<std::mutex> lock(state->mutex_);
          state->notified_ = false;
        }
        if (state->owner_ && !state->owner_->IsClosed())
          state->owner_->ContinueHandling();
      };
      if (!loop->QueueInLoop(resume))
      {
        if (auto state = weak.lock())
          state->cancel_->store(true);
      }
    };
    Tick();
  }
  ~HttpConnection() override
  {
    response_->cancel_->store(true);
    response_->owner_ = nullptr;
    loop_->timerQueue().Cancel(tick_);
  }

protected:
  bool HasPendingBusiness() const override { return active_; }
  void HandleBusiness() override
  {
    if (dispatched_)
    {
      Drain();
      return;
    }
    auto status = parser_.Feed(std::string_view(in_.peek(), in_.readableBytes()));
    in_.retrieveAll();
    reading_paused_ = false;
    if (status == HttpParser::Status::NeedMore && closing_)
      status = parser_.Finish();
    if (status == HttpParser::Status::Error)
    {
      const char *code = "400 Bad Request";
      switch (parser_.errorStatus())
      {
      case 411: code = "411 Length Required"; break;
      case 413: code = "413 Payload Too Large"; break;
      case 431: code = "431 Request Header Fields Too Large"; break;
      }
      Reject(code, parser_.errorMessage());
      return;
    }
    if (status != HttpParser::Status::Complete)
      return;
    dispatched_ = true;
    active_ = true;
    handler_(parser_.request(), response_);
    Drain();
  }

private:
  void Tick()
  {
    tick_ = loop_->timerQueue().AddTimer(NowMs() + 1000, [this]
    {
      if (IsClosed())
        return;
      // SSE 空闲期间定期写心跳，及时发现客户端断开。
      if (active_)
        response_->Heartbeat();
      ContinueHandling();
      if (!IsClosed())
        Tick();
    });
  }
  void Drain()
  {
    std::lock_guard<std::mutex> lock(response_->mutex_);
    while (!response_->output_.empty() && !out_.overLimit())
    {
      auto &text = response_->output_.front();
      auto count = std::min(text.size() - response_->offset_, out_.maxBytes() - out_.readableBytes());
      if (!out_.append(text.data() + response_->offset_, count))
      {
        Close();
        return;
      }
      response_->offset_ += count;
      response_->queued_ -= count;
      if (response_->offset_ == text.size())
      {
        response_->output_.pop_front();
        response_->offset_ = 0;
      }
    }
    if (response_->finished_ && response_->output_.empty())
    {
      active_ = false;
      closing_ = true;
    }
    reading_paused_ = !response_->output_.empty();
  }
  void Reject(const std::string &status, const std::string &message)
  {
    dispatched_ = true;
    active_ = true;
    response_->Reply(status, nlohmann::json{{"error", message}}.dump());
    Drain();
  }
  EventLoop *loop_;
  const HttpHandler &handler_;
  std::shared_ptr<HttpResponse> response_;
  TimerId tick_ = 0;
  HttpParser parser_;
  bool dispatched_ = false, active_ = false;
};

struct HttpServer::Impl
{
  ServerConfig config;
  HttpHandler handler;
  TcpServer server;

  Impl(const ServerConfig &config, HttpHandler handler)
      : config(config), handler(std::move(handler)),
        server(config.port, [this](EventLoop *loop, int fd)
        {
          return std::make_unique<HttpConnection>(loop, fd, this->handler);
        }) {}
};

HttpServer::HttpServer(const ServerConfig &config, HttpHandler handler)
    : impl_(std::make_unique<Impl>(config, std::move(handler))) {}
HttpServer::~HttpServer() = default;

int HttpServer::Run()
{
  PrepareServerProcess();
  if (!impl_->server.Init(impl_->config.io_threads))
    return 1;
  std::cout << "HTTP IO 线程：" << impl_->server.ioThreadCount() << std::endl;
  impl_->server.Loop();
  return 0;
}
