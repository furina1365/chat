#pragma once
#include "UringLoop.hpp"
#include "../common/Socket.hpp"

namespace net::proactor
{
class Acceptor : private NonCopyable
{
public:
  bool Init(UringLoop *loop, int fd, std::function<void(int, const InetAddress &)> callback)
  {
    loop_ = loop;
    fd_ = fd;
    callback_ = std::move(callback);
    return Arm();
  }
  void Stop()
  {
    if (!loop_ || stopped_) return;
    stopped_ = true;
    loop_->timerQueue().Cancel(retry_);
    loop_->ring().Cancel(accept_);
  }
private:
  bool Arm()
  {
    if (stopped_ || loop_->stopping()) return false;
    accept_ = loop_->ring().Submit([this](io_uring_sqe *sqe, Operation &)
        { io_uring_prep_accept(sqe, fd_, nullptr, nullptr, SOCK_CLOEXEC); },
        [this](int result, Operation &)
        {
          accept_ = 0;
          if (stopped_ || loop_->stopping())
          { if (result >= 0) close(result); return; }
          if (result >= 0)
          {
            sockaddr_storage address{};
            socklen_t size = sizeof(address);
            InetAddress peer;
            if (getpeername(result, reinterpret_cast<sockaddr *>(&address), &size) == 0)
            {
              if (address.ss_family == AF_INET)
                peer = InetAddress(*reinterpret_cast<sockaddr_in *>(&address));
              else if (address.ss_family == AF_INET6)
                peer = InetAddress(*reinterpret_cast<sockaddr_in6 *>(&address));
            }
            callback_(result, peer);
            if (!Arm()) Retry();
          }
          else
          {
            if (result != -EINTR)
              LOG_WARN("accept error (errno=%d): %s, fd=%d; retry in 1s",
                       -result, strerror(-result), fd_);
            Retry();
          }
        });
    if (!accept_) LOG_WARN("accept submission failed, fd=%d", fd_);
    return accept_ != 0;
  }
  void Retry()
  {
    if (stopped_ || loop_->stopping()) return;
    // 包括 EMFILE 在内的失败都延迟重投，避免错误完成造成忙循环。
    retry_ = loop_->timerQueue().AddTimer(NowMs() + 1000, [this]
        { if (!Arm()) Retry(); });
  }
  UringLoop *loop_ = nullptr;
  int fd_ = -1;
  uint64_t accept_ = 0;
  TimerId retry_ = 0;
  bool stopped_ = false;
  std::function<void(int, const InetAddress &)> callback_;
};
}
