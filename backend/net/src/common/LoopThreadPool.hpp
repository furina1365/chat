#pragma once
#include "NonCopyable.hpp"
#include <memory>
#include <thread>
#include <vector>

template<class Loop>
class LoopThreadPool : private NonCopyable
{
public:
  ~LoopThreadPool() { Stop(); }
  bool Prepare(int count)
  {
    for (int i = 0; i < count; ++i)
    {
      auto loop = std::make_unique<Loop>();
      if (!loop->Init()) { Stop(); return false; }
      loops_.push_back(loop.get());
      owned_.push_back(std::move(loop));
    }
    return !loops_.empty();
  }
  void StartThreads()
  { for (auto *loop : loops_) threads_.emplace_back([loop] { loop->Loop(); }); }
  const std::vector<Loop *> &loops() const { return loops_; }
  size_t GetNextIndex()
  {
    if (loops_.empty()) return SIZE_MAX;
    size_t index = next_;
    next_ = (next_ + 1) % loops_.size();
    return index;
  }
  void Stop()
  {
    for (auto *loop : loops_) loop->Quit();
    for (auto &thread : threads_) if (thread.joinable()) thread.join();
    threads_.clear();
    // loop 生命周期覆盖业务线程停止和连接析构，不能在 join 后立即销毁。
  }
private:
  std::vector<std::unique_ptr<Loop>> owned_;
  std::vector<Loop *> loops_;
  std::vector<std::thread> threads_;
  size_t next_ = 0;
};
