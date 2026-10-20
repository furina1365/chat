#pragma once

#include "TimerQueue.hpp"
#include <functional>
#include <memory>

namespace net::detail { struct Access; }

// 一个线程运行一个事件循环；具体 epoll/io_uring 实现由构建配置选择。
class EventLoop
{
public:
  using Functor = std::function<void()>;

  EventLoop();
  ~EventLoop();
  EventLoop(const EventLoop &) = delete;
  EventLoop &operator=(const EventLoop &) = delete;

  bool Init();
  void Loop();
  void Quit();                    // 可跨线程调用，唤醒并请求退出。
  void Shutdown();                // 所属线程停止后，排空剩余 IO 操作。
  bool IsInLoopThread() const;
  bool RunInLoop(Functor callback);   // 所属线程直接执行，否则入队。
  bool QueueInLoop(Functor callback); // 可跨线程；队列满或退出后返回 false。

  // 定时器和生命周期回调在所属 IO 线程操作。
  TimerQueue &timerQueue();
  void SetEveryLoopCallback(Functor callback);
  void SetShutdownCallback(Functor callback);

private:
  friend struct net::detail::Access;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
