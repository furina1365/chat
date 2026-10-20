#include "common/Runtime.hpp"

uint64_t NowMs()
{
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void PrepareServerProcess()
{
  ::signal(SIGPIPE, SIG_IGN);
  ::signal(SIGINT, handle_signal);
  ::signal(SIGTERM, handle_signal);
  struct rlimit limit;
  limit.rlim_cur = kMaxConn + 16;
  limit.rlim_max = kMaxConn + 16;
  if (setrlimit(RLIMIT_NOFILE, &limit) == -1)
  {
    perror("setrlimit");
  }
}
