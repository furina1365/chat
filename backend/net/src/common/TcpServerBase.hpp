#pragma once
#include "net/TcpServer.hpp"
#include "LoopThreadPool.hpp"
#include "Runtime.hpp"
#include "Socket.hpp"

// 两套后端共用连接管理流程，各自传入 loop、连接和 acceptor 类型。
template<class Loop, class Connection, class Acceptor>
class TcpServerBase : private NonCopyable
{
public:
  using Factory = ConnectionFactory;

  TcpServerBase(const InetAddress &address, Factory factory)
      : address_(address), factory_(std::move(factory)) {}
  TcpServerBase(uint16_t port, Factory factory)
      : TcpServerBase(InetAddress(port), std::move(factory)) {}
  ~TcpServerBase()
  {
    pool_.Stop();
    // 包括 Init 中途失败：accept 的原 CQE 必须在 acceptor 析构前处理。
    acceptor_.Stop();
    main_loop_.Shutdown();
  }

  bool Init(int io_threads = 0)
  {
    if (io_threads < 0)
      return false;
    listen_socket_ = Socket::CreateListen(address_);
    if (listen_socket_.fd() < 0 || !main_loop_.Init())
      return false;
    int count = io_threads ? io_threads : std::max(1u, std::thread::hardware_concurrency());
    if (!pool_.Prepare(count))
      return false;
    for (auto *loop : pool_.loops())
    {
      auto connections = std::make_unique<LoopConnections>();
      auto *owned = connections.get();
      loop->SetEveryLoopCallback([loop, owned]
      {
        owned->RemoveClosed();
        if (g_quit)
          loop->Quit();
      });
      loop->SetShutdownCallback([owned] { owned->Stop(); });
      connections_.push_back(std::move(connections));
    }
    if (!InitAcceptor())
      return false;
    main_loop_.SetEveryLoopCallback([this]
    {
      if (g_quit)
        main_loop_.Quit();
    });
    main_loop_.SetShutdownCallback([this] { acceptor_.Stop(); });
    pool_.StartThreads();
    return true;
  }
  Loop &mainLoop() { return main_loop_; }
  size_t ioThreadCount() const { return pool_.loops().size(); }
  void LoopForever()
  {
    main_loop_.Loop();
    pool_.Stop();
  }

private:
  bool InitAcceptor()
  {
    return acceptor_.Init(&main_loop_, listen_socket_.fd(),
        [this](int fd, const InetAddress &peer) { Dispatch(fd, peer); });
  }

  struct LoopConnections
  {
    std::vector<std::unique_ptr<Connection>> values;
    void RemoveClosed()
    {
      values.erase(std::remove_if(values.begin(), values.end(),
          [](const auto &connection) { return connection->CanDestroy(); }), values.end());
    }
    void Stop()
    {
      for (auto &connection : values)
        connection->Stop();
    }
  };
  void Dispatch(int fd, const InetAddress &peer)
  {
    (void)peer;
    // 回调尚未执行而 loop 已退出时，RAII 仍会关闭 accepted fd。
    auto socket = std::make_shared<Socket>(fd);
    size_t index = pool_.GetNextIndex();
    if (index >= connections_.size())
      return;
    auto *loop = pool_.loops()[index];
    auto *owned = connections_[index].get();
    loop->QueueInLoop([this, loop, owned, socket]
    {
      if (g_quit || owned->values.size() >= kMaxConn)
        return;
      auto connection = factory_(loop, socket->Release());
      if (connection)
        owned->values.push_back(std::move(connection));
    });
  }
  InetAddress address_;
  Factory factory_;
  Socket listen_socket_;
  Loop main_loop_;
  // 析构顺序：连接先析构，随后销毁它们使用的 IO loop。
  LoopThreadPool<Loop> pool_;
  std::vector<std::unique_ptr<LoopConnections>> connections_;
  Acceptor acceptor_;
};
