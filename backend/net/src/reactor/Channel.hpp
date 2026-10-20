#pragma once
#include "../common/Runtime.hpp"
#include <functional>
#include <map>
#include <memory>
#include <sys/epoll.h>
#include <unistd.h>
#include <vector>

namespace net::reactor
{
// Channel 通过 EPollPoller 更新 fd 的 epoll 事件订阅。
class EPollPoller;

// 一个 fd 的事件封装：保存关注事件、返回事件和分发回调。
class Channel
{
public:
  void Init(EPollPoller *poller, int fd)
  {
    poller_ = poller;
    fd_ = fd;
    events_ = EPOLLIN;
    revents_ = 0;
    added_ = false;
  }

  void Reset()
  {
    poller_ = nullptr;
    fd_ = -1;
    events_ = 0;
    revents_ = 0;
    added_ = false;
    event_cb_ = nullptr;
  }

  int fd() const { return fd_; }
  void SetRevents(uint32_t revents) { revents_ = revents; }
  void SetEventCallback(std::function<void(uint32_t)> cb)
  {
    event_cb_ = std::move(cb);
  }
  void HandleEvent()
  {
    if (event_cb_)
    {
      event_cb_(revents_);
    }
  }

  void SetEvents(uint32_t events);
  void EnableReading() { SetEvents(events_ | EPOLLIN); }
  void DisableReading() { SetEvents(events_ & ~EPOLLIN); }
  void EnableWriting() { SetEvents(events_ | EPOLLOUT); }
  void DisableWriting() { SetEvents(events_ & ~EPOLLOUT); }
  void Disable();
  void RemoveFromEPollPoller();

private:
  EPollPoller *poller_ = nullptr;
  int fd_ = -1;
  uint32_t events_ = 0;
  uint32_t revents_ = 0;
  bool added_ = false;
  std::function<void(uint32_t)> event_cb_;
};

// epoll 注册和就绪事件查询。注册细节只由 Channel 调用。
class EPollPoller
{
public:
  ~EPollPoller()
  {
    if (epfd_ >= 0)
    {
      close(epfd_);
    }
  }

  bool Init()
  {
    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0)
    {
      perror("epoll_create1");
      return false;
    }
    events_ = std::make_unique<epoll_event[]>(kMaxEvents);
    return true;
  }

  bool Wait(int timeoutMs, std::vector<Channel *> &ready)
  {
    ready.clear();
    int n = epoll_wait(epfd_, events_.get(), kMaxEvents, timeoutMs);
    if (n < 0)
    {
      if (errno == EINTR)
      {
        return true;
      }
      perror("epoll_wait");
      return false;
    }
    for (int i = 0; i < n; ++i)
    {
      int fd = events_[i].data.fd;
      auto it = channels_.find(fd);
      if (it == channels_.end())
      {
        continue;
      }
      it->second->SetRevents(events_[i].events);
      ready.push_back(it->second);
    }
    return true;
  }

private:
  friend class Channel;

  bool Add(int fd, uint32_t events, Channel *channel)
  {
    struct epoll_event event;
    event.events = events;
    event.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &event) < 0)
    {
      perror("epoll_ctl ADD");
      return false;
    }
    channels_[fd] = channel;
    return true;
  }

  void Update(int fd, uint32_t events)
  {
    struct epoll_event event;
    event.events = events;
    event.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event) < 0)
    {
      LOG_WARN("epoll_ctl MOD fd=%d failed (errno=%d)", fd, errno);
    }
  }

  void Remove(int fd)
  {
    channels_.erase(fd);
  }

  void Disable(int fd)
  {
    channels_.erase(fd);
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
  }

  int epfd_ = -1;
  std::unique_ptr<epoll_event[]> events_;
  std::map<int, Channel *> channels_;
};

inline void Channel::SetEvents(uint32_t events)
{
  events_ = events;
  if (!added_)
  {
    poller_->Add(fd_, events_, this);
    added_ = true;
  }
  else
  {
    poller_->Update(fd_, events_);
  }
}

inline void Channel::Disable()
{
  poller_->Disable(fd_);
  added_ = false;
}

inline void Channel::RemoveFromEPollPoller()
{
  poller_->Remove(fd_);
  added_ = false;
}

}
