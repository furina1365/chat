#pragma once

#include "Buffer.hpp"
#include <memory>

class EventLoop;
namespace net::reactor { class TcpConnection; }
namespace net::proactor { class TcpConnection; }

// 业务继承此类，只在所属 IO 线程处理连接；fd 所有权交给连接。
class TcpConnection
{
public:
  TcpConnection(EventLoop *loop, int fd);
  virtual ~TcpConnection();
  TcpConnection(const TcpConnection &) = delete;
  TcpConnection &operator=(const TcpConnection &) = delete;

  int fd() const;
  bool IsClosed() const;
  // 关闭且原收发操作已完成后，运行时才可以销毁连接。
  bool CanDestroy() const;
  void Stop();
  void HandleIdleTimeout();

protected:
  virtual void HandleBusiness() = 0;
  virtual bool HasPendingBusiness() const;
  void PauseBusiness();
  void ContinueHandling();
  void Close();

  // 业务消费 in_、填充 out_；网络层负责实际收发和背压。
  Buffer in_, out_;
  bool closing_ = false, reading_paused_ = false;

private:
  friend class net::reactor::TcpConnection;
  friend class net::proactor::TcpConnection;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
