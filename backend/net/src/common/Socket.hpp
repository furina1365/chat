#pragma once

// 服务端、客户端共用的网络地址与套接字封装：InetAddress 和 Socket。
// Socket 通过工厂方法创建，以 RAII 管理 fd；两套网络后端使用这些封装。
// NonCopyable 在 NonCopyable.hpp，这里只依赖它。

#include "NonCopyable.hpp"
#include "net/InetAddress.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <netdb.h>

#include <cstdio>
#include <cstring>
#include <ostream>
#include <string>
// 套接字 fd 的 RAII 封装：创建统一走工厂方法（CreateListen/CreateConnect），
// 错误路径由 Close 或析构保证不泄漏 fd；fd 所有权唯一，不允许拷贝，
// 移动转移所有权（工厂返回对象时用）。
class Socket : private NonCopyable
{
public:
  Socket() = default;
  explicit Socket(int fd) : fd_(fd) {}

  ~Socket()
  {
    if (fd_ >= 0)
    {
      close(fd_);
    }
  }

  Socket(Socket &&other) noexcept : fd_(other.fd_)
  {
    other.fd_ = -1;
  }

  Socket &operator=(Socket &&other) noexcept
  {
    if (this != &other)
    {
      if (fd_ >= 0)
      {
        close(fd_);
      }
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;

  int fd() const { return fd_; }

  // 工厂：创建监听套接字（socket + 选项 + 非阻塞 + bind + listen）。
  // 失败时返回无效 Socket（fd() == -1），perror 已打印原因。
  static Socket CreateListen(const InetAddress &listenAddr)
  {
    if (!listenAddr.IsValid())
    {
      return Socket();
    }

    Socket s;
    s.fd_ = socket(listenAddr.GetFamily(), SOCK_STREAM, 0);
    if (s.fd_ < 0)
    {
      perror("socket");
      return Socket();
    }

    int opt = 1;
    // TIME_WAIT 期间可立即重用地址；失败不直接退出，bind 会给最终结果。
    if (setsockopt(s.fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
      perror("setsockopt SO_REUSEADDR");
    }
    // 多进程可共享端口，必须在 bind 前设置；失败同样交给 bind 兜底。
    if (setsockopt(s.fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
    {
      perror("setsockopt SO_REUSEPORT");
    }
    if (s.SetNonBlocking() < 0)
    {
      s.Close();
      return Socket();
    }

    if (bind(s.fd_, listenAddr.GetSockAddrPtr(), listenAddr.GetSockLen()) < 0)
    {
      perror("bind");
      s.Close();
      return Socket();
    }
    // 实际 backlog = min(4096, net.core.somaxconn)，内核上限只能通过
    // sysctl 调整，代码里没有提高它的接口。
    if (listen(s.fd_, 4096) < 0)
    {
      perror("listen");
      s.Close();
      return Socket();
    }
    return s;
  }

  // 工厂：创建连接套接字（socket + connect）。客户端直连用，最简阻塞版；
  // 失败时返回无效 Socket（fd() == -1），perror 已打印原因。
  static Socket CreateConnect(const InetAddress &addr)
  {
    if (!addr.IsValid())
    {
      return Socket();
    }

    Socket s;
    s.fd_ = socket(addr.GetFamily(), SOCK_STREAM, 0);
    if (s.fd_ < 0)
    {
      perror("socket");
      return Socket();
    }
    if (connect(s.fd_, addr.GetSockAddrPtr(), addr.GetSockLen()) < 0)
    {
      perror("connect");
      s.Close();
      return Socket();
    }
    return s;
  }

  // 事件循环里的客户端连接必须先设非阻塞再 connect。立即成功时
  // connected=true；EINPROGRESS 表示连接正在建立，之后由 EPOLLOUT 和
  // SO_ERROR 判断最终结果。其它错误直接返回无效 Socket。
  static Socket CreateNonBlockingConnect(const InetAddress &addr,
                                         bool &connected)
  {
    connected = false;
    if (!addr.IsValid())
    {
      return Socket();
    }

    Socket s;
    s.fd_ = socket(addr.GetFamily(), SOCK_STREAM, 0);
    if (s.fd_ < 0)
    {
      perror("socket");
      return Socket();
    }
    if (s.SetNonBlocking() < 0)
    {
      s.Close();
      return Socket();
    }
    if (connect(s.fd_, addr.GetSockAddrPtr(), addr.GetSockLen()) == 0)
    {
      connected = true;
      return s;
    }
    if (errno == EINPROGRESS)
    {
      return s;
    }
    perror("connect");
    s.Close();
    return Socket();
  }

  Socket Accept(InetAddress &peer)
  {
    return Socket(accept(fd_, peer.GetSockAddrPtr(), peer.GetSockLenPtr()));
  }

  // 监听和连接套接字都设为非阻塞，事件循环中的 accept/recv/send
  // 才能一直处理到 EAGAIN，而不会被某一个 fd 卡住。
  int SetNonBlocking()
  {
    int flags = fcntl(fd_, F_GETFL);
    if (flags < 0)
    {
      perror("fcntl F_GETFL");
      return -1;
    }
    if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0)
    {
      perror("fcntl F_SETFL O_NONBLOCK");
      return -1;
    }
    return 0;
  }

  int Release()
  {
    int fd = fd_;
    fd_ = -1;
    return fd;
  }

  void Close()
  {
    if (fd_ >= 0)
    {
      close(fd_);
      fd_ = -1;
    }
  }

private:
  int fd_ = -1;
};
