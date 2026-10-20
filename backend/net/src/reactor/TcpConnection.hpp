#pragma once
#include "net/TcpConnection.hpp"
#include "EventLoop.hpp"
#include "BufferRead.hpp"
#include <sys/socket.h>

namespace net::reactor
{
class TcpConnection : private NonCopyable
{
  friend class ::TcpConnection;
public:
  TcpConnection(EventLoop *loop, int fd, ::TcpConnection &owner)
      : in_(owner.in_), out_(owner.out_), closing_(owner.closing_),
        reading_paused_(owner.reading_paused_), owner_(owner), timer_queue_(&loop->timerQueue())
  {
    channel_.Init(&loop->poller(), fd);
    last_active_ = NowMs();
    channel_.SetEventCallback([this](uint32_t events)
    {
      HandleEvent(events);
    });
    channel_.SetEvents(EPOLLIN);
    timer_id_ = timer_queue_->AddTimer(NowMs() + kIdleTimeoutMs, [this]
    {
      HandleIdleTimeout();
    });
  }

  virtual ~TcpConnection()
  {
    timer_queue_->Cancel(timer_id_);
    if (channel_.fd() >= 0)
    {
      close(channel_.fd());
    }
  }

  int fd() const { return channel_.fd(); }
  bool IsClosed() const { return channel_.fd() < 0; }
  bool CanDestroy() const { return IsClosed(); }
  void Stop() { Close(); }

  void HandleIdleTimeout()
  {
    if (channel_.fd() < 0)
    {
      return;
    }
    // 到点时差值恰好可能等于阈值，必须用 >=，否则会推迟一个周期。
    if (NowMs() - last_active_ >= kIdleTimeoutMs)
    {
      LOG_INFO("idle timeout, close fd=%d", channel_.fd());
      Close();
    }
    else
    {
      timer_id_ = timer_queue_->AddTimer(NowMs() + kIdleTimeoutMs, [this]
      {
        HandleIdleTimeout();
      });
    }
  }

protected:
  void HandleBusiness() { owner_.HandleBusiness(); }
  bool HasPendingBusiness() const { return owner_.HasPendingBusiness(); }

  // 异步业务完成时，前端 fd 未必再次产生事件。主动走一遍连接控制流，
  // 才能提交响应、继续处理输入缓冲里的下一条命令并恢复 EPOLLIN。
  void PauseBusiness()
  {
    business_paused_ = true;
  }

  void ContinueHandling()
  {
    business_paused_ = false;
    HandleEvent(0);
  }

  void Close()
  {
    if (IsClosed()) return;
    int connectionFd = channel_.fd();
    LOG_INFO("closed fd=%d", connectionFd);
    timer_queue_->Cancel(timer_id_);
    channel_.RemoveFromEPollPoller();
    close(connectionFd);
    channel_.Reset();
  }

  Channel channel_;
  Buffer &in_, &out_;
  bool &closing_, &reading_paused_;

private:
  ::TcpConnection &owner_;
  void HandleEvent(uint32_t events)
  {
    if (events & (EPOLLIN | EPOLLERR | EPOLLHUP))
    {
      if (HandleRead(events))
      {
        return;
      }
    }

    // 异步业务完成由下游 Channel 或 deadline 唤醒，前端 fd 此时可能没有
    // 新事件；ContinueHandling 通过 events=0 明确要求继续业务处理。
    if (events == 0 && !business_paused_)
    {
      HandleBusiness();
      if (channel_.fd() < 0)
      {
        return;
      }
    }

    if (!FlushOut())
    {
      Close();
      return;
    }

    // 输出缓冲降到低水位后，继续处理已经读进输入缓冲的请求：
    // 1. 每次处理前先清掉旧的 reading_paused_。
    //    原因：旧值只表示上一次输出缓冲已满，不能表示这次处理后仍要暂停。
    //    如果不清，输入缓冲只剩半包时 HandleBusiness 无法消费数据，也不会
    //    修改旧值，循环便会一直围着同一个半包空转。
    //    具体案例：64KB 输入中有 5041 条 13 字节完整命令和下一条命令的
    //    前 3 字节。循环会反复解析这 3 字节，IO 线程占满 CPU，套接字中
    //    剩余的 10 字节得不到读取，客户端永远少收到最后一条响应。
    // 2. 处理后根据 HandleBusiness 是否重新置 reading_paused_ 决定退出。
    //    原因：重新置 true 表示输出缓冲再次放不下响应，输入缓冲中可能还有
    //    完整请求；保持暂停可以避免继续读取新数据，并在输出下降后接着处理。
    //    没有重新置 true 则表示当前完整请求已经处理完，应该恢复 EPOLLIN，
    //    等待半包的剩余数据到达。
    //    具体案例：输入缓冲中还有 100 条完整请求，但输出缓冲只能再放 20 条
    //    响应，HandleBusiness 处理 20 条后会重新置 true；此时必须保持暂停，
    //    等这 20 条响应发送出去后再处理剩余 80 条。反过来，如果输入缓冲
    //    只剩下一条请求的前 3 字节，HandleBusiness 不会重新置 true；此时
    //    必须退出循环并恢复 EPOLLIN，才能收齐这条请求的剩余字节。
    // 输入仍满时也必须先让业务消费，否则已停读且输出为空会永久失去唤醒。
    if (reading_paused_ && !business_paused_ &&
        out_.readableBytes() <= out_.maxBytes() / 2)
    {
      LOG_DEBUG("backpressure released, resume reading (fd=%d)", channel_.fd());
      while (true)
      {
        reading_paused_ = false;
        HandleBusiness();
        if (channel_.fd() < 0)
        {
          return;
        }
        if (!FlushOut())
        {
          Close();
          return;
        }
        // 没有再次触发写背压：退出循环，下面重新监听 EPOLLIN。
        if (!reading_paused_ || business_paused_)
        {
          break;
        }
        if (out_.readableBytes() > out_.maxBytes() / 2)
        {
          break;
        }
      }
    }

    if (out_.readableBytes() == 0 && closing_ && !HasPendingBusiness())
    {
      Close();
      return;
    }

    uint32_t interested = 0;
    if (!closing_ && !reading_paused_ && !business_paused_)
    {
      interested |= EPOLLIN;
    }
    if (out_.readableBytes() > 0)
    {
      interested |= EPOLLOUT;
    }
    channel_.SetEvents(interested);
  }

  bool HandleRead(uint32_t events)
  {
    int connectionFd = channel_.fd();
    if (!reading_paused_ || (events & (EPOLLERR | EPOLLHUP)))
    {
      while (true)
      {
        int savedErrno = 0;
        ssize_t n = ReadFd(in_, connectionFd, &savedErrno);
        if (n > 0)
        {
          last_active_ = NowMs();
          continue;
        }
        if (n < 0 && savedErrno == EINTR)
        {
          continue;
        }
        if (n == 0)
        {
          closing_ = true;
          break;
        }
        if (n < 0 && savedErrno != EAGAIN)
        {
          Close();
          return true;
        }
        break;
      }
      if (in_.overLimit())
      {
        reading_paused_ = true;
        LOG_DEBUG("read backpressure: input at high water mark, pause reading (fd=%d)",
                  connectionFd);
      }
    }

    HandleBusiness();
    return channel_.fd() < 0;
  }

  bool FlushOut()
  {
    int connectionFd = channel_.fd();
    while (out_.readableBytes() > 0)
    {
      ssize_t n = send(connectionFd, out_.peek(), out_.readableBytes(), 0);
      if (n > 0)
      {
        out_.retrieve(static_cast<size_t>(n));
      }
      else if (n < 0 && errno == EAGAIN)
      {
        return true;
      }
      else if (n < 0 && errno == EINTR)
      {
        continue;
      }
      else if (n < 0 && errno == EPIPE)
      {
        LOG_WARN("send EPIPE: peer closed, fd=%d", connectionFd);
        return false;
      }
      else
      {
        LOG_ERROR("send error (errno=%d), fd=%d", errno, connectionFd);
        return false;
      }
    }
    return true;
  }

  uint64_t last_active_ = 0;
  TimerQueue *timer_queue_ = nullptr;
  TimerId timer_id_ = 0;
  bool business_paused_ = false;
};
}
