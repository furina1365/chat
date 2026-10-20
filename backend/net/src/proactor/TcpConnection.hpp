#pragma once
#include "net/TcpConnection.hpp"
#include "UringLoop.hpp"
#include "net/Buffer.hpp"

namespace net::proactor
{
class TcpConnection : private NonCopyable
{
  friend class ::TcpConnection;
public:
  TcpConnection(UringLoop *loop, int fd, ::TcpConnection &owner)
      : in_(owner.in_), out_(owner.out_), closing_(owner.closing_),
        reading_paused_(owner.reading_paused_), owner_(owner), loop_(loop), fd_(fd)
  {
    last_active_ = NowMs();
    ArmIdle();
    Receive();
  }
  virtual ~TcpConnection()
  {
    loop_->timerQueue().Cancel(timer_);
    // 正常回收由 CanDestroy 保证所有原操作已经完成。
    if (fd_ >= 0) close(fd_);
  }
  int fd() const { return closed_ ? -1 : fd_; }
  bool IsClosed() const { return closed_; }
  bool CanDestroy() const { return closed_ && !recv_ && !send_; }
  void Stop() { Close(); }
  void HandleIdleTimeout()
  {
    if (closed_) return;
    if (NowMs() - last_active_ >= kIdleTimeoutMs)
    {
      LOG_INFO("idle timeout, close fd=%d", fd_);
      Close();
    }
    else ArmIdle();
  }

protected:
  void HandleBusiness() { owner_.HandleBusiness(); }
  bool HasPendingBusiness() const { return owner_.HasPendingBusiness(); }
  void PauseBusiness() { business_paused_ = true; }
  void ContinueHandling()
  {
    if (closed_) return;
    business_paused_ = false;
    HandleBusiness();
    Progress();
  }
  void Close()
  {
    if (closed_) return;
    LOG_INFO("closing fd=%d", fd_);
    closed_ = true;
    loop_->timerQueue().Cancel(timer_);
    // fd 保留到所有原 CQE 到达，避免未提交 SQE 引用已复用的 fd。
    shutdown(fd_, SHUT_RDWR);
    loop_->ring().Cancel(recv_);
    loop_->ring().Cancel(send_);
  }
  Buffer &in_, &out_;
  bool &closing_, &reading_paused_;

private:
  ::TcpConnection &owner_;
  void ArmIdle()
  { timer_ = loop_->timerQueue().AddTimer(NowMs() + kIdleTimeoutMs, [this] { HandleIdleTimeout(); }); }
  void Receive()
  {
    if (closed_ || recv_ || closing_ || reading_paused_ || business_paused_) return;
    auto bytes = std::min<size_t>(16384, in_.maxBytes() - in_.readableBytes());
    if (!bytes)
    {
      LOG_DEBUG("read backpressure: input at high water mark, pause reading (fd=%d)", fd_);
      reading_paused_ = true;
      return;
    }
    recv_ = loop_->ring().Submit([this, bytes](io_uring_sqe *sqe, Operation &op)
        { io_uring_prep_recv(sqe, fd_, op.storage.get(), bytes, 0); },
        [this](int result, Operation &op)
        {
          recv_ = 0;
          if (closed_ || loop_->stopping()) return;
          if (result > 0)
          {
            last_active_ = NowMs();
            if (!in_.append(op.storage.get(), result))
            {
              LOG_ERROR("recv exceeds input buffer limit, fd=%d", fd_);
              Close();
              return;
            }
            if (in_.overLimit())
            {
              LOG_DEBUG("read backpressure: input at high water mark, pause reading (fd=%d)", fd_);
              reading_paused_ = true;
            }
          }
          else if (!result) closing_ = true;
          else if (result != -EINTR)
          {
            LOG_ERROR("recv error (errno=%d): %s, fd=%d", -result, strerror(-result), fd_);
            Close();
            return;
          }
          HandleBusiness();
          Progress();
        }, bytes);
    if (!recv_)
    {
      LOG_ERROR("recv submission failed, fd=%d", fd_);
      Close();
    }
  }
  void Send()
  {
    if (closed_ || send_ || !out_.readableBytes()) return;
    auto bytes = out_.readableBytes();
    send_ = loop_->ring().Submit([this, bytes](io_uring_sqe *sqe, Operation &op)
        {
          // CQE 到来前业务可能继续 append 并移动 out_，必须用独立快照。
          std::copy(out_.peek(), out_.peek() + bytes, op.storage.get());
          io_uring_prep_send(sqe, fd_, op.storage.get(), bytes, MSG_NOSIGNAL);
        }, [this](int result, Operation &)
        {
          send_ = 0;
          if (closed_ || loop_->stopping()) return;
          if (result > 0) out_.retrieve(result);
          else if (result != -EINTR)
          {
            if (result == -EPIPE)
              LOG_WARN("send EPIPE: peer closed, fd=%d", fd_);
            else if (result < 0)
              LOG_ERROR("send error (errno=%d): %s, fd=%d", -result, strerror(-result), fd_);
            else
              LOG_WARN("send completed with zero bytes, close fd=%d", fd_);
            Close();
            return;
          }
          Progress();
        }, bytes);
    if (!send_)
    {
      LOG_ERROR("send submission failed, fd=%d", fd_);
      Close();
    }
  }
  void Progress()
  {
    if (closed_) return;
    // 低水位时恢复业务，提交下一段发送；异步完成后才消费输出缓冲。
    if (reading_paused_ && !business_paused_ &&
        out_.readableBytes() <= out_.maxBytes() / 2)
    {
      LOG_DEBUG("backpressure released, resume handling (fd=%d)", fd_);
      reading_paused_ = false;
      HandleBusiness();
      if (closed_) return;
    }
    if (!out_.readableBytes() && !send_ && closing_ && !HasPendingBusiness())
    { Close(); return; }
    Send();
    Receive();
  }
  UringLoop *loop_;
  int fd_;
  uint64_t recv_ = 0, send_ = 0, last_active_ = 0;
  TimerId timer_ = 0;
  bool closed_ = false, business_paused_ = false;
};
}
