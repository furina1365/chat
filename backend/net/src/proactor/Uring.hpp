#pragma once
#include "../common/Runtime.hpp"
#include <functional>
#include <cstring>
#include <liburing.h>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace net::proactor
{
  // 每项 Op 的存储与回调一直保留到原请求 CQE；cancel 的 CQE 不代表原请求完成。
  struct Operation
  {
    uint64_t id = 0;
    bool cancel_requested = false;
    std::unique_ptr<char[]> storage;
    std::function<void(int, Operation &)> complete;
  };

  class Uring : private NonCopyable
  {
  public:
    using Prepare = std::function<void(io_uring_sqe *, Operation &)>;
    using Complete = std::function<void(int, Operation &)>;
    ~Uring()
    {
      if (initialized_)
        io_uring_queue_exit(&ring_);
    }
    bool Init()
    {
      int result = io_uring_queue_init(256, &ring_, 0);
      if (result < 0)
      {
        LOG_ERROR("io_uring_queue_init error (errno=%d): %s", -result, strerror(-result));
        return false;
      }
      initialized_ = true;
      return true;
    }
    uint64_t Submit(Prepare prepare, Complete complete, size_t bytes = 0, bool control = false)
    {
      // 每 loop 最多 4096 项 IO，另预留同等数量的取消请求。
      if (operations_.size() >= (control ? 8192u : 4096u))
      {
        LOG_WARN("io_uring operation capacity exhausted (pending=%zu, control=%d)",
                 operations_.size(), control);
        return 0;
      }
      auto operation = std::make_unique<Operation>();
      operation->id = next_++;
      // recv 只消费 CQE 返回的字节数，send 提交前会填满；无需预清零。
      if (bytes) operation->storage.reset(new char[bytes]);
      operation->complete = std::move(complete);
      io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
      if (!sqe)
      {
        Flush();
        sqe = io_uring_get_sqe(&ring_);
      }
      if (!sqe)
      {
        LOG_WARN("io_uring submission queue full after flush (pending=%zu)", operations_.size());
        return 0;
      }
      prepare(sqe, *operation);
      auto id = operation->id;
      io_uring_sqe_set_data64(sqe, id);
      operations_.emplace(id, std::move(operation));
      return id;
    }
    void Cancel(uint64_t id)
    {
      auto it = operations_.find(id);
      if (it == operations_.end() || it->second->cancel_requested)
        return;
      it->second->cancel_requested = true;
      auto cancel = Submit([id](io_uring_sqe *sqe, Operation &)
                           { io_uring_prep_cancel64(sqe, id, 0); }, {}, 0, true);
      if (!cancel)
        throw std::runtime_error("io_uring cancellation capacity exhausted");
    }
    void Flush()
    {
      int result;
      do
      {
        result = io_uring_submit(&ring_);
      } while (result == -EINTR);
      if (result < 0)
        throw std::runtime_error(std::string("io_uring_submit: ") + strerror(-result));
    }
    void Wait(int timeout)
    {
      Flush();
      __kernel_timespec ts{timeout / 1000, (timeout % 1000) * 1000000LL};
      io_uring_cqe *cqe = nullptr;
      int result = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
      if (result < 0 && result != -ETIME && result != -EINTR)
        throw std::runtime_error(std::string("io_uring_wait: ") + strerror(-result));
      // 限制每轮完成数，避免持续流量饿死任务队列和定时器。
      for (int i = 0; i < kMaxEvents && io_uring_peek_cqe(&ring_, &cqe) == 0; ++i)
      {
        auto id = io_uring_cqe_get_data64(cqe);
        auto res = cqe->res;
        io_uring_cqe_seen(&ring_, cqe);
        auto node = operations_.extract(id);
        if (!node.empty() && node.mapped()->complete)
          node.mapped()->complete(res, *node.mapped());
      }
    }
    void CancelAll()
    {
      std::vector<uint64_t> ids;
      for (const auto &entry : operations_)
        if (entry.second->complete)
          ids.push_back(entry.first);
      for (auto id : ids)
        Cancel(id);
      while (!operations_.empty())
        Wait(250);
    }
    bool empty() const { return operations_.empty(); }

  private:
    io_uring ring_{};
    bool initialized_ = false;
    uint64_t next_ = 1;
    std::unordered_map<uint64_t, std::unique_ptr<Operation>> operations_;
  };
}
