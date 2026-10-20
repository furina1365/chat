#pragma once
#include "NonCopyable.hpp"
#include "Logging.hpp"
#include "net/Runtime.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <sys/resource.h>

inline volatile sig_atomic_t g_quit = 0;
inline void handle_signal(int) { g_quit = 1; }
inline constexpr int kMaxConn = 65535;
inline constexpr int kMaxEvents = 4096;
#ifdef LOGGING_DEBUG
inline constexpr time_t kIdleTimeout = 30;
#else
inline constexpr time_t kIdleTimeout = 60;
#endif
inline constexpr uint64_t kIdleTimeoutMs = kIdleTimeout * 1000;

inline void MsToTimespec(uint64_t milliseconds, timespec &ts)
{
  ts.tv_sec = milliseconds / 1000;
  ts.tv_nsec = (milliseconds % 1000) * 1000000;
}

