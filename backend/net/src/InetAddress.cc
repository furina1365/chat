#include "net/InetAddress.hpp"
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <ostream>

InetAddress::InetAddress(uint16_t port, const std::string &host)
{
    _valid = Resolve(host, std::to_string(port));
  }

InetAddress::InetAddress(const std::string &endpoint)
{
    std::string host;
    std::string port;

    if (!ParseEndpoint(endpoint, host, port))
    {
      return;
    }

    _valid = Resolve(host, port);
  }

InetAddress::InetAddress(const sockaddr_in &addr)
{
    memcpy(&_addr, &addr, sizeof(addr));
    _sockLen = sizeof(addr);
    _valid = true;
  }

InetAddress::InetAddress(const sockaddr_in6 &addr)
{
    memcpy(&_addr, &addr, sizeof(addr));
    _sockLen = sizeof(addr);
    _valid = true;
  }

const sockaddr *InetAddress::GetSockAddrPtr() const
{
    return reinterpret_cast<const sockaddr *>(&_addr);
  }

sockaddr *InetAddress::GetSockAddrPtr()
{
    return reinterpret_cast<sockaddr *>(&_addr);
  }

socklen_t InetAddress::GetSockLen() const
{
    return _sockLen;
  }

socklen_t *InetAddress::GetSockLenPtr()
{
    _sockLen = sizeof(_addr);
    return &_sockLen;
  }

sa_family_t InetAddress::GetFamily() const
{
    return _addr.ss_family;
  }

bool InetAddress::IsIpv4() const
{
    return GetFamily() == AF_INET;
  }

bool InetAddress::IsIpv6() const
{
    return GetFamily() == AF_INET6;
  }

bool InetAddress::IsValid() const
{
    return _valid;
  }

uint16_t InetAddress::GetPort() const
{
    if (IsIpv4())
    {
      auto *addr = reinterpret_cast<const sockaddr_in *>(&_addr);
      return ntohs(addr->sin_port);
    }

    if (IsIpv6())
    {
      auto *addr = reinterpret_cast<const sockaddr_in6 *>(&_addr);
      return ntohs(addr->sin6_port);
    }

    return 0;
  }

std::string InetAddress::GetIp() const
{
    char buffer[INET6_ADDRSTRLEN]{};

    if (IsIpv4())
    {
      auto *addr =
          reinterpret_cast<const sockaddr_in *>(&_addr);

      if (::inet_ntop(AF_INET,
                      &addr->sin_addr,
                      buffer,
                      sizeof(buffer)) == nullptr)
      {
        return "";
      }
    }
    else if (IsIpv6())
    {
      auto *addr =
          reinterpret_cast<const sockaddr_in6 *>(&_addr);

      if (::inet_ntop(AF_INET6,
                      &addr->sin6_addr,
                      buffer,
                      sizeof(buffer)) == nullptr)
      {
        return "";
      }
    }
    else
    {
      return "";
    }

    return buffer;
  }

std::string InetAddress::GetIpAndPort() const
{
    if (IsIpv6())
    {
      return "[" + GetIp() + "]:" +
             std::to_string(GetPort());
    }

    return GetIp() + ":" +
           std::to_string(GetPort());
  }

void InetAddress::SetPort(uint16_t port)
{
    if (IsIpv4())
    {
      auto *addr = reinterpret_cast<sockaddr_in *>(&_addr);
      addr->sin_port = htons(port);
    }
    else if (IsIpv6())
    {
      auto *addr = reinterpret_cast<sockaddr_in6 *>(&_addr);
      addr->sin6_port = htons(port);
    }
  }

bool InetAddress::operator==(const InetAddress &rhs) const
{
    if (GetFamily() != rhs.GetFamily())
    {
      return false;
    }

    if (GetPort() != rhs.GetPort())
    {
      return false;
    }

    if (IsIpv4())
    {
      auto *lhsAddr =
          reinterpret_cast<const sockaddr_in *>(&_addr);
      auto *rhsAddr =
          reinterpret_cast<const sockaddr_in *>(&rhs._addr);

      return lhsAddr->sin_addr.s_addr ==
             rhsAddr->sin_addr.s_addr;
    }

    if (IsIpv6())
    {
      auto *lhsAddr =
          reinterpret_cast<const sockaddr_in6 *>(&_addr);
      auto *rhsAddr =
          reinterpret_cast<const sockaddr_in6 *>(&rhs._addr);

      return memcmp(&lhsAddr->sin6_addr,
                    &rhsAddr->sin6_addr,
                    sizeof(in6_addr)) == 0;
    }

    return false;
  }

bool InetAddress::operator!=(const InetAddress &rhs) const
{
    return !(*this == rhs);
  }

std::ostream &operator<<(std::ostream &os, const InetAddress &addr)
{
    return os << addr.GetIpAndPort();
  }

bool InetAddress::Resolve(const std::string &host, const std::string &port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC; // IPv4 / IPv6 都允许
    hints.ai_socktype = 0;        // 不限制 socket 类型
    hints.ai_protocol = 0;        // 不限制协议号
    hints.ai_flags = AI_NUMERICSERV;

    addrinfo *result = nullptr;

    int ret = ::getaddrinfo(
        host.c_str(),
        port.c_str(),
        &hints,
        &result);

    if (ret != 0)
    {
      fprintf(stderr,
              "getaddrinfo failed: %s\n",
              gai_strerror(ret));
      return false;
    }

    bool success = false;

    for (addrinfo *p = result; p != nullptr; p = p->ai_next)
    {
      if (p->ai_family != AF_INET &&
          p->ai_family != AF_INET6)
      {
        continue;
      }

      if (p->ai_addrlen > sizeof(_addr))
      {
        continue;
      }

      memcpy(&_addr, p->ai_addr, p->ai_addrlen);
      _sockLen = p->ai_addrlen;

      success = true;
      break;
    }

    ::freeaddrinfo(result);

    return success;
  }

bool InetAddress::ParseEndpoint(const std::string &endpoint, std::string &host, std::string &port)
{
    if (endpoint.empty())
    {
      return false;
    }

    // IPv6:
    // [::1]:8080
    // [2001:db8::1]:8080
    if (endpoint.front() == '[')
    {
      size_t right = endpoint.find(']');

      if (right == std::string::npos ||
          right + 1 >= endpoint.size() ||
          endpoint[right + 1] != ':')
      {
        return false;
      }

      host = endpoint.substr(1, right - 1);
      port = endpoint.substr(right + 2);
    }
    else
    {
      // IPv4 / domain
      // 127.0.0.1:8080
      // example.com:8080

      size_t colon = endpoint.rfind(':');

      if (colon == std::string::npos ||
          colon == 0 ||
          colon + 1 == endpoint.size())
      {
        return false;
      }

      host = endpoint.substr(0, colon);
      port = endpoint.substr(colon + 1);

      // 不允许裸 IPv6：
      // ::1:8080
      if (host.find(':') != std::string::npos)
      {
        return false;
      }
    }

    uint32_t value = 0;

    for (char ch : port)
    {
      if (ch < '0' || ch > '9')
      {
        return false;
      }

      value = value * 10 + (ch - '0');

      if (value > 65535)
      {
        return false;
      }
    }

    return value != 0;
  }
