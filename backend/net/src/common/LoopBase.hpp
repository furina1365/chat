#pragma once
#include "net/TimerQueue.hpp"
#include "Runtime.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

// CRTP 只统一任务与线程归属；等待 IO 和 Wakeup 均由后端实现。
template<class Derived>
class LoopBase : private NonCopyable
{
public:
  using Functor = std::function<void()>;
  bool IsInLoopThread() const { return current_ == this; }
  bool RunInLoop(Functor callback)
  { if (IsInLoopThread()) { callback(); return true; } return QueueInLoop(std::move(callback)); }
  bool QueueInLoop(Functor callback)
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (quit_ || pending_.size() >= 65535) return false;
      pending_.push_back(std::move(callback));
    }
    self().Wakeup();
    return true;
  }
  void Quit() { quit_ = true; self().Wakeup(); }
  TimerQueue &timerQueue() { return timers_; }
  void SetEveryLoopCallback(Functor callback) { every_loop_ = std::move(callback); }
  void SetShutdownCallback(Functor callback) { shutdown_ = std::move(callback); }

protected:
  void EnterLoop() { current_ = this; }
  bool Quitting() const { return quit_; }
  void FinishIteration()
  {
    DoPendingFunctors();
    timers_.RunExpired();
    if (every_loop_) every_loop_();
  }
  void FinishLoop()
  {
    DoPendingFunctors();
    if (shutdown_) shutdown_();
    current_ = nullptr;
  }

private:
  Derived &self() { return static_cast<Derived &>(*this); }
  void DoPendingFunctors()
  {
    std::deque<Functor> ready;
    { std::lock_guard<std::mutex> lock(mutex_); ready.swap(pending_); }
    for (auto &callback : ready) callback();
  }
  std::atomic<bool> quit_{false};
  static inline thread_local LoopBase *current_ = nullptr;
  std::mutex mutex_;
  std::deque<Functor> pending_;
  TimerQueue timers_;
  Functor every_loop_, shutdown_;
};
