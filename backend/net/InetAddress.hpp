#pragma once

#include <cstdint>
#include <iosfwd>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>

// IPv4/IPv6 地址；解析失败时 IsValid() 返回 false。
class InetAddress
{
public:
  explicit InetAddress(std::uint16_t port = 0, const std::string &host = "0.0.0.0");
  explicit InetAddress(const std::string &endpoint);
  InetAddress(const sockaddr_in &address);
  InetAddress(const sockaddr_in6 &address);

  const sockaddr *GetSockAddrPtr() const;
  sockaddr *GetSockAddrPtr();
  socklen_t GetSockLen() const;
  socklen_t *GetSockLenPtr();
  sa_family_t GetFamily() const;
  bool IsIpv4() const;
  bool IsIpv6() const;
  bool IsValid() const;
  std::uint16_t GetPort() const;
  std::string GetIp() const;
  std::string GetIpAndPort() const;
  void SetPort(std::uint16_t port);
  bool operator==(const InetAddress &other) const;
  bool operator!=(const InetAddress &other) const;
  friend std::ostream &operator<<(std::ostream &stream, const InetAddress &address);

private:
  bool Resolve(const std::string &host, const std::string &port);
  static bool ParseEndpoint(const std::string &endpoint, std::string &host, std::string &port);
  sockaddr_storage _addr{};
  socklen_t _sockLen{0};
  bool _valid{false};
};
