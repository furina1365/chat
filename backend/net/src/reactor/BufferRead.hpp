#pragma once
#include "net/Buffer.hpp"
#include <cerrno>
#include <unistd.h>
#include <sys/uio.h>

namespace net::reactor
{
inline ssize_t ReadFd(Buffer &buffer, int fd, int *savedErrno)
{
  size_t quota = buffer.maxBytes() - buffer.readableBytes();
  if (!quota) { *savedErrno = EAGAIN; return -1; }
  // 保留原 readv 路径：优先直接写 Buffer，溢出部分才从栈上追加。
  if (!buffer.writableBytes()) buffer.prepareWrite(std::min<size_t>(1024, quota));
  char storage[64 * 1024];
  size_t direct = std::min(buffer.writableBytes(), quota);
  iovec vec[2]{{buffer.prepareWrite(direct), direct},
               {storage, std::min(quota - direct, sizeof(storage))}};
  ssize_t result = ::readv(fd, vec, 2);
  if (result < 0) *savedErrno = errno;
  else
  {
    size_t bytes = static_cast<size_t>(result);
    buffer.commitWrite(std::min(bytes, direct));
    if (bytes > direct) buffer.append(storage, bytes - direct);
  }
  return result;
}
}
