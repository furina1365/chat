#pragma once
#include "Uring.hpp"
#include "../common/LoopBase.hpp"
#include <sys/eventfd.h>

namespace net::proactor
{
class UringLoop : public LoopBase<UringLoop>
{
public:
  ~UringLoop()
  {
    // 未启动的 loop 也可能有预投递的唤醒/accept，先退完再释放 fd。
    Shutdown();
    if (wakeup_fd_ >= 0) close(wakeup_fd_);
  }
  bool Init()
  {
    if (!ring_.Init()) return false;
    initialized_ = true;
    wakeup_fd_ = eventfd(0, EFD_CLOEXEC);
    if (wakeup_fd_ < 0)
    {
      [[maybe_unused]] int error = errno;
      LOG_ERROR("eventfd error (errno=%d): %s", error, strerror(error));
      return false;
    }
    return ArmWakeup();
  }
  Uring &ring() { return ring_; }
  bool stopping() const { return stopping_; }
  void Shutdown()
  {
    if (!initialized_ || drained_) return;
    stopping_ = true;
    FinishLoop();
    Drain();
  }
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
      int timeout = timerQueue().NextTimeoutMs();
      ring_.Wait(timeout < 0 ? 250 : std::min(timeout, 250));
      FinishIteration();
    }
    Shutdown();
  }
private:
  bool ArmWakeup()
  {
    auto wakeup = ring_.Submit([this](io_uring_sqe *sqe, Operation &op)
        { io_uring_prep_read(sqe, wakeup_fd_, op.storage.get(), sizeof(uint64_t), 0); },
        [this](int result, Operation &)
        {
          if (stopping_) return;
          if (result < 0)
            LOG_ERROR("wakeup read error (errno=%d): %s, fd=%d",
                      -result, strerror(-result), wakeup_fd_);
          else if (!ArmWakeup()) Quit();
        }, sizeof(uint64_t));
    if (!wakeup) LOG_ERROR("wakeup read submission failed, fd=%d", wakeup_fd_);
    return wakeup != 0;
  }
  void Drain()
  {
    stopping_ = true;
    ring_.CancelAll();
    drained_ = true;
  }
  Uring ring_;
  int wakeup_fd_ = -1;
  bool initialized_ = false, stopping_ = false, drained_ = false;
};
}
