#pragma once

#include "EventLoop.hpp"
#include "InetAddress.hpp"
#include "TcpConnection.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

// 工厂在接收连接的 IO 线程调用，返回业务连接并接管 fd。
using ConnectionFactory = std::function<std::unique_ptr<TcpConnection>(EventLoop *, int)>;

class TcpServer
{
public:
  using Factory = ::ConnectionFactory;
  using ConnectionFactory = Factory;

  TcpServer(const InetAddress &address, Factory factory);
  TcpServer(std::uint16_t port, Factory factory);
  ~TcpServer();
  TcpServer(const TcpServer &) = delete;
  TcpServer &operator=(const TcpServer &) = delete;

  // 0 按 CPU 逻辑核数创建 IO 线程；初始化失败返回 false。
  bool Init(int io_threads = 0);
  void Loop();
  EventLoop &mainLoop();
  std::size_t ioThreadCount() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
