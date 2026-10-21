#include "http/HttpResponse.hpp"
#include <sstream>
#include <stdexcept>

namespace
{
  constexpr std::size_t kMaxQueuedOutput = 1024 * 1024;

  std::string Chunk(const std::string &data)
  {
    std::ostringstream header;
    header << std::hex << data.size() << "\r\n";
    return header.str() + data + "\r\n";
  }
}

void HttpResponse::SetHeader(const std::string &name, const std::string &value)
{
  if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != name.npos ||
      value.find_first_of("\r\n") != value.npos) throw std::invalid_argument("无效响应头");
  std::lock_guard<std::mutex> lock(mutex_);
  headers_ += name + ": " + value + "\r\n";
}

bool HttpResponse::Push(std::string text, bool final)
{
  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancel_->load() || finished_)
      return false;
    if (!final && text.size() > kMaxQueuedOutput - queued_)
    {
      overloaded_ = true;
      return false;
    }
    queued_ += text.size();
    output_.push_back(std::move(text));
    finished_ = finished_ || final;
    if (!notified_)
    {
      notified_ = true;
      notify = true;
    }
  }
  if (notify)
    wake_();
  return true;
}

void HttpResponse::Reply(const std::string &status, const std::string &body, const std::string &type)
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // 流尚未开始发送时，可用普通错误响应替换它（例如任务队列已满）。
    output_.clear();
    queued_ = offset_ = 0;
    streaming_ = false;
  }
  Push("HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\n" + headers_ +
       "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body, true);
}

void HttpResponse::StartStream()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    streaming_ = true;
  }
  Push("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
       "Cache-Control: no-cache\r\nX-Accel-Buffering: no\r\nTransfer-Encoding: chunked\r\n" + headers_ +
       "Connection: close\r\n\r\n" + Chunk(": connected\n\n"));
}

bool HttpResponse::SendEvent(const char *name, const std::string &data, bool final)
{
  auto text = Chunk(std::string("event: ") + name + "\ndata: " + data + "\n\n");
  if (final)
    text += "0\r\n\r\n";
  return Push(std::move(text), final);
}

void HttpResponse::Heartbeat()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!streaming_ || finished_)
      return;
  }
  Push(Chunk(": ping\n\n"));
}

bool HttpResponse::overloaded() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return overloaded_;
}
