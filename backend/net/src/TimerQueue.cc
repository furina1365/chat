#include "net/TimerQueue.hpp"
#include "net/Runtime.hpp"
#include <algorithm>
#include <climits>
#include <utility>

void TimerQueue::SetChangedCallback(std::function<void()> callback)
{ changed_ = std::move(callback); }

TimerId TimerQueue::AddTimer(uint64_t expiry, std::function<void()> callback)
{
    auto previous = NextDeadline();
    TimerId id = next_++;
    index_[id] = heap_.size();
    heap_.push_back({expiry, id, std::move(callback)});
    Up(heap_.size() - 1);
    Changed(previous);
    return id;
  }

void TimerQueue::Cancel(TimerId id)
{
    expired_.erase(id);
    auto it = index_.find(id);
    if (it == index_.end()) return;
    auto previous = NextDeadline();
    Remove(it->second);
    Changed(previous);
  }

std::optional<uint64_t> TimerQueue::NextDeadline() const
{
    if (heap_.empty()) return std::nullopt;
    return heap_[0].expiry;
  }

int TimerQueue::NextTimeoutMs() const
{
    if (heap_.empty()) return -1;
    auto now = NowMs();
    return heap_[0].expiry <= now ? 0 :
        static_cast<int>(std::min<uint64_t>(heap_[0].expiry - now, INT_MAX));
  }

void TimerQueue::RunExpired()
{
    auto previous = NextDeadline();
    // 先取出本轮到期回调，Cancel 仍能取消尚未执行的回调。
    std::vector<TimerId> ready;
    auto now = NowMs();
    while (!heap_.empty() && heap_[0].expiry <= now)
    {
      ready.push_back(heap_[0].id);
      expired_.emplace(heap_[0].id, std::move(heap_[0].callback));
      Remove(0);
    }
    Changed(previous);
    for (auto id : ready)
    {
      auto node = expired_.extract(id);
      if (!node.empty()) node.mapped()();
    }
  }

bool TimerQueue::Before(size_t a, size_t b) const
{ return std::make_pair(heap_[a].expiry, heap_[a].id) < std::make_pair(heap_[b].expiry, heap_[b].id); }

void TimerQueue::Swap(size_t a, size_t b)
{
    std::swap(heap_[a], heap_[b]);
    index_[heap_[a].id] = a;
    index_[heap_[b].id] = b;
  }

void TimerQueue::Up(size_t i)
{ while (i && Before(i, (i - 1) / 2)) { auto p = (i - 1) / 2; Swap(i, p); i = p; } }

void TimerQueue::Down(size_t i)
{
    for (;;)
    {
      size_t child = i * 2 + 1;
      if (child >= heap_.size()) return;
      if (child + 1 < heap_.size() && Before(child + 1, child)) ++child;
      if (!Before(child, i)) return;
      Swap(i, child);
      i = child;
    }
  }

void TimerQueue::Remove(size_t i)
{
    auto id = heap_[i].id;
    Swap(i, heap_.size() - 1);
    heap_.pop_back();
    index_.erase(id);
    if (i < heap_.size())
    {
      if (i && Before(i, (i - 1) / 2)) Up(i);
      else Down(i);
    }
  }

void TimerQueue::Changed(std::optional<uint64_t> previous)
{
    // 后面的定时器增删、同到期时间的堆顶替换，都无需重设触发器。
    if (changed_ && previous != NextDeadline()) changed_();
  }
