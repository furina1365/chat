#include "net/TcpServer.hpp"
#include "Backend.hpp"
#include "common/TcpServerBase.hpp"
#if CHAT_NET_PROACTOR
#include "proactor/Acceptor.hpp"
using BackendAcceptor = net::proactor::Acceptor;
#else
#include "reactor/Acceptor.hpp"
using BackendAcceptor = net::reactor::Acceptor;
#endif

namespace
{
  class ServerAcceptor
  {
  public:
    bool Init(EventLoop *loop, int fd, std::function<void(int, const InetAddress &)> callback)
    {
      return acceptor_.Init(&net::detail::Access::Loop(*loop), fd, std::move(callback));
    }
    void Stop() { acceptor_.Stop(); }
  private:
    BackendAcceptor acceptor_;
  };
}

struct TcpServer::Impl : TcpServerBase<EventLoop, TcpConnection, ServerAcceptor>
{
  using TcpServerBase::TcpServerBase;
};

TcpServer::TcpServer(const InetAddress &address, Factory factory)
    : impl_(std::make_unique<Impl>(address, std::move(factory))) {}
TcpServer::TcpServer(std::uint16_t port, Factory factory)
    : TcpServer(InetAddress(port), std::move(factory)) {}
TcpServer::~TcpServer() = default;
bool TcpServer::Init(int io_threads) { return impl_->Init(io_threads); }
void TcpServer::Loop() { impl_->LoopForever(); }
EventLoop &TcpServer::mainLoop() { return impl_->mainLoop(); }
std::size_t TcpServer::ioThreadCount() const { return impl_->ioThreadCount(); }
