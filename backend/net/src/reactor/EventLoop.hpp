#pragma once
#include "Channel.hpp"
#include "../common/LoopBase.hpp"
#include <sys/eventfd.h>
#include <sys/timerfd.h>

namespace net::reactor
{
class EventLoop : public LoopBase<EventLoop>
{
public:
  ~EventLoop()
  {
    timerQueue().SetChangedCallback({});
    if (wakeup_fd_ >= 0) close(wakeup_fd_);
    if (timer_fd_ >= 0) close(timer_fd_);
  }
  bool Init()
  {
    if (!poller_.Init()) return false;
    wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (wakeup_fd_ < 0 || timer_fd_ < 0) { perror("loop fd"); return false; }
    wakeup_channel_.Init(&poller_, wakeup_fd_);
    wakeup_channel_.SetEventCallback([this](uint32_t) { Drain(wakeup_fd_); });
    wakeup_channel_.SetEvents(EPOLLIN);
    timer_channel_.Init(&poller_, timer_fd_);
    timer_channel_.SetEventCallback([this](uint32_t) { Drain(timer_fd_); });
    timer_channel_.SetEvents(EPOLLIN);
    timerQueue().SetChangedCallback([this] { ResetTimer(); });
    return true;
  }
  EPollPoller &poller() { return poller_; }
  void Shutdown() {}
  void Wakeup()
  {
    uint64_t one = 1;
    if (wakeup_fd_ >= 0) while (write(wakeup_fd_, &one, sizeof(one)) < 0 && errno == EINTR) {}
  }
  void Loop()
  {
    EnterLoop();
    while (!Quitting())
    {
      std::vector<Channel *> ready;
      // 信号处理函数只设置标志；有限等待保证没有业务流量时也能退出。
      if (!poller_.Wait(250, ready)) break;
      for (auto *channel : ready) channel->HandleEvent();
      FinishIteration();
    }
    FinishLoop();
  }
private:
  static void Drain(int fd)
  { uint64_t value; while (read(fd, &value, sizeof(value)) > 0) {} }
  void ResetTimer()
  {
    itimerspec value{};
    auto deadline = timerQueue().NextDeadline();
    if (deadline)
    {
      auto now = NowMs();
      // timerfd 支持长时长，不使用 poll 超时的 INT_MAX 毫秒上限。
      MsToTimespec(*deadline > now ? *deadline - now : 1, value.it_value);
    }
    if (timerfd_settime(timer_fd_, 0, &value, nullptr) < 0) perror("timerfd_settime");
  }
  EPollPoller poller_;
  Channel wakeup_channel_, timer_channel_;
  int wakeup_fd_ = -1, timer_fd_ = -1;
};
}
