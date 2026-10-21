#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace db::detail
{
template<class T> class Pool
{
public:
  struct Lease
  {
    Pool *pool = nullptr;
    T *handle = nullptr;
    bool healthy = true;
    Lease(Pool *pool, T *handle) : pool(pool), handle(handle) {}
    Lease(Lease &&other) noexcept : pool(other.pool), handle(other.handle), healthy(other.healthy)
    { other.handle = nullptr; }
    Lease &operator=(Lease &&) = delete;
    ~Lease() { if (handle) pool->Release(handle, healthy); }
  };

  Pool(std::size_t limit, std::chrono::milliseconds timeout,
       std::function<T *()> create, std::function<void(T *)> destroy)
      : limit_(limit), timeout_(timeout), create_(std::move(create)), destroy_(std::move(destroy))
  {
    if (!limit_ || timeout_.count() <= 0) throw std::invalid_argument("无效的连接池配置");
    idle_.push_back(create_());
    total_ = 1;
  }
  ~Pool() { for (auto *handle : idle_) destroy_(handle); }

  std::optional<Lease> Acquire()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_.wait_for(lock, timeout_, [this] { return !idle_.empty() || total_ < limit_; }))
      return std::nullopt;
    if (!idle_.empty())
    {
      auto *handle = idle_.back();
      idle_.pop_back();
      return Lease(this, handle);
    }
    ++total_;
    lock.unlock();
    try { return Lease(this, create_()); }
    catch (...)
    {
      lock.lock();
      --total_;
      ready_.notify_one();
      throw;
    }
  }

private:
  void Release(T *handle, bool healthy)
  {
    if (!healthy) destroy_(handle);
    std::lock_guard<std::mutex> lock(mutex_);
    if (healthy) idle_.push_back(handle);
    else --total_;
    ready_.notify_one();
  }
  std::size_t limit_, total_ = 0;
  std::chrono::milliseconds timeout_;
  std::function<T *()> create_;
  std::function<void(T *)> destroy_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::vector<T *> idle_;
};
} // namespace db::detail
