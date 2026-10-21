#pragma once
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace crypto
{
inline std::string Hex(const unsigned char *data, std::size_t size)
{
  constexpr char digits[] = "0123456789abcdef";
  std::string value(size * 2, '0');
  for (std::size_t i = 0; i < size; ++i) { value[i * 2] = digits[data[i] >> 4]; value[i * 2 + 1] = digits[data[i] & 15]; }
  return value;
}
inline std::string Random(std::size_t bytes = 32)
{
  std::vector<unsigned char> buffer(bytes);
  if (RAND_bytes(buffer.data(), buffer.size()) != 1) throw std::runtime_error("随机数生成失败");
  return Hex(buffer.data(), buffer.size());
}
inline std::string Hash(const std::string &value)
{
  std::array<unsigned char, 32> buffer{};
  unsigned int size = 0;
  if (EVP_Digest(value.data(), value.size(), buffer.data(), &size, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("摘要计算失败");
  return Hex(buffer.data(), size);
}
}
