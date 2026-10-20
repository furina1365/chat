#pragma once

#include "net/Config.hpp"
#include "net/EventLoop.hpp"
#if CHAT_NET_PROACTOR
#include "proactor/UringLoop.hpp"
#else
#include "reactor/EventLoop.hpp"
#endif

namespace net::detail
{
#if CHAT_NET_PROACTOR
  using BackendLoop = net::proactor::UringLoop;
#else
  using BackendLoop = net::reactor::EventLoop;
#endif
}

struct EventLoop::Impl : net::detail::BackendLoop {};

namespace net::detail
{
  // 后端原生 loop 仅在实现文件中可见。
  struct Access
  {
    static BackendLoop &Loop(EventLoop &loop) { return *loop.impl_; }
  };
}
