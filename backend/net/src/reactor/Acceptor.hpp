#pragma once
#include "EventLoop.hpp"
#include "../common/Socket.hpp"

namespace net::reactor
{
class Acceptor : private NonCopyable
{
public:
  bool Init(EventLoop *loop, int fd, std::function<void(int, const InetAddress &)> callback)
  {
    loop_ = loop;
    callback_ = std::move(callback);
    channel_.Init(&loop->poller(), fd);
    channel_.SetEventCallback([this](uint32_t) { Accept(); });
    channel_.SetEvents(EPOLLIN);
    return true;
  }
  void Stop()
  {
    if (!loop_ || stopped_) return;
    stopped_ = true;
    loop_->timerQueue().Cancel(retry_);
    channel_.Disable();
  }
private:
  void Accept()
  {
    while (!stopped_)
    {
      InetAddress peer;
      int fd = accept4(channel_.fd(), peer.GetSockAddrPtr(), peer.GetSockLenPtr(),
                       SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd >= 0) { callback_(fd, peer); continue; }
      if (errno == EINTR || errno == ECONNABORTED) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)
      {
        channel_.Disable();
        retry_ = loop_->timerQueue().AddTimer(NowMs() + 1000, [this]
        { if (!stopped_) channel_.SetEvents(EPOLLIN); });
      }
      break;
    }
  }
  EventLoop *loop_ = nullptr;
  Channel channel_;
  std::function<void(int, const InetAddress &)> callback_;
  TimerId retry_ = 0;
  bool stopped_ = false;
};
}
