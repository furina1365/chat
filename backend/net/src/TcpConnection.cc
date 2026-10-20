#include "net/TcpConnection.hpp"
#include "Backend.hpp"
#if CHAT_NET_PROACTOR
#include "proactor/TcpConnection.hpp"
using BackendConnection = net::proactor::TcpConnection;
#else
#include "reactor/TcpConnection.hpp"
using BackendConnection = net::reactor::TcpConnection;
#endif

struct TcpConnection::Impl
{
  BackendConnection connection;
  Impl(TcpConnection &owner, EventLoop *loop, int fd)
      : connection(&net::detail::Access::Loop(*loop), fd, owner) {}
};

TcpConnection::TcpConnection(EventLoop *loop, int fd)
    : impl_(std::make_unique<Impl>(*this, loop, fd)) {}
TcpConnection::~TcpConnection() = default;
int TcpConnection::fd() const { return impl_->connection.fd(); }
bool TcpConnection::IsClosed() const { return impl_->connection.IsClosed(); }
bool TcpConnection::CanDestroy() const { return impl_->connection.CanDestroy(); }
void TcpConnection::Stop() { impl_->connection.Stop(); }
void TcpConnection::HandleIdleTimeout() { impl_->connection.HandleIdleTimeout(); }
bool TcpConnection::HasPendingBusiness() const { return false; }
void TcpConnection::PauseBusiness() { impl_->connection.PauseBusiness(); }
void TcpConnection::ContinueHandling() { impl_->connection.ContinueHandling(); }
void TcpConnection::Close() { impl_->connection.Close(); }
