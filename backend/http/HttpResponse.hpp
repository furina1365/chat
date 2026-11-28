#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

class HttpConnection;

// 每个请求一个响应；封装普通响应、SSE 编码和有界的跨线程发送队列。
class HttpResponse
{
public:
  void SetHeader(const std::string &name, const std::string &value);
  void Reply(const std::string &status, const std::string &body,
             const std::string &type = "application/json; charset=utf-8");
  void StartStream();
  // data 为已序列化的单行 JSON；final 同时结束 SSE 和 HTTP chunked 响应。
  bool SendEvent(const char *name, const std::string &data, bool final = false);
  const std::shared_ptr<std::atomic<bool>> &cancelToken() const { return cancel_; }
  void SetCancelHandler(std::function<void()> handler);
  bool overloaded() const;

private:
  friend class HttpConnection;
  enum class Frame { Data, StreamStart, Final };
  bool Push(std::string text, Frame frame = Frame::Data);
  bool Heartbeat(bool when_empty = false);
  void Cancel();

  mutable std::mutex mutex_;
  std::deque<std::string> output_;
  std::string headers_;
  std::size_t queued_ = 0, offset_ = 0;
  bool finished_ = false, notified_ = false, overloaded_ = false, streaming_ = false;
  std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
  std::function<void()> cancel_handler_;
  std::function<void()> wake_;
  // 仅 IO 线程访问 owner_；工作线程只访问队列和取消标记。
  HttpConnection *owner_ = nullptr;
};
