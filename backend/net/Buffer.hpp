#pragma once

#include <cstddef>
#include <string>
#include <vector>

// 有界字节缓冲；只管理内存和读写位置，不执行 socket IO。
class Buffer
{
public:
  static constexpr std::size_t kDefaultMaxBytes = 64 * 1024;
  static constexpr std::size_t kInitialSize = 1024;

  explicit Buffer(std::size_t maxBytes = kDefaultMaxBytes);
  std::size_t readableBytes() const;
  std::size_t writableBytes() const;
  std::size_t maxBytes() const;
  bool overLimit() const;
  const char *peek() const; // 空缓冲返回 nullptr。

  // 超过容量上限返回 false，不消费已有数据。
  bool append(const char *data, std::size_t len);
  bool append(const std::string &text);
  char *prepareWrite(std::size_t len);
  void commitWrite(std::size_t len);
  void retrieve(std::size_t len);
  void retrieveAll();

private:
  void makeSpace(std::size_t len);
  std::vector<char> buf_;
  std::size_t readIndex_ = 0, writeIndex_ = 0, maxBytes_;
};
