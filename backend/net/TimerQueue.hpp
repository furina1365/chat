#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

using TimerId = std::uint64_t;

// 仅在所属 IO 线程操作；到期时间使用 NowMs() 对应的单调时钟毫秒数。
class TimerQueue
{
public:
  TimerQueue() = default;
  TimerQueue(const TimerQueue &) = delete;
  TimerQueue &operator=(const TimerQueue &) = delete;

  TimerId AddTimer(std::uint64_t expiry, std::function<void()> callback);
  void Cancel(TimerId id);
  std::optional<std::uint64_t> NextDeadline() const;
  int NextTimeoutMs() const; // 空队列返回 -1。
  void RunExpired();
  // 最早到期时间变化时通知后端重设触发器。
  void SetChangedCallback(std::function<void()> callback);

private:
  struct Timer { std::uint64_t expiry; TimerId id; std::function<void()> callback; };
  bool Before(std::size_t a, std::size_t b) const;
  void Swap(std::size_t a, std::size_t b);
  void Up(std::size_t i);
  void Down(std::size_t i);
  void Remove(std::size_t i);
  void Changed(std::optional<std::uint64_t> previous);

  TimerId next_ = 1;
  std::vector<Timer> heap_;
  std::unordered_map<TimerId, std::size_t> index_;
  std::unordered_map<TimerId, std::function<void()>> expired_;
  std::function<void()> changed_;
};
