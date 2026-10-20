#pragma once

// ===== 日志开关（二选一，编译参数传入）=====
// 编译时 -DLOGGING_DEBUG 打印、-DLOGGING_RELEASE 编译掉（零开销）；
// 都不传默认 LOGGING_DEBUG（交互测试方便，不用记参数）。
// 不要用 -Ddebug / -Drelease：-D 在所有头文件之前生效，-Drelease 会把
// 标准库里的 release() 成员函数（如 std::unique_ptr::release()）展开成
// 1()，编译直接失败；-Ddebug 名字太通用，容易和变量/其它库撞。
#if !defined(LOGGING_DEBUG) && !defined(LOGGING_RELEASE)
#define LOGGING_DEBUG // 默认 debug 档
#endif
#if defined(LOGGING_DEBUG) && defined(LOGGING_RELEASE)
#error "LOGGING_DEBUG and LOGGING_RELEASE cannot be defined at the same time"
#endif
// ============================================

#include <cstdio>
#include <ctime>

namespace logging
{
// 日志级别（值越大越严重），只打印 >= kLogLevel 的日志
enum Level
{
  kDebug,
  kInfo,
  kWarn,
  kError
};
// 改这里控制日志量：kDebug 全打（含背压等高频细节），kInfo 常规全打，
// kWarn 只留警告和错误
constexpr Level kLogLevel = kInfo;

// 返回当前时间 "HH:MM:SS"，日志宏里用；thread_local 保证多线程各写各的
inline const char *Now()
{
  static thread_local char buf[16];
  time_t t = ::time(nullptr);
  struct tm tm;
  ::localtime_r(&t, &tm);
  ::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  return buf;
}
} // namespace logging

// 日志宏：LOGGING_DEBUG 下按 kLogLevel 过滤打印（带时间、级别、文件:行号）；
// LOGGING_RELEASE 下整条日志在编译期去掉，参数不求值、零开销。
// 注意：fmt 必须是字符串字面量，数据一律作为变参传入——不能把运行时
// 数据当 fmt 用（printf 会把其中的 % 当格式符）。
#ifdef LOGGING_DEBUG
#define LOG_DEBUG(fmt, ...)                                                         \
  do {                                                                              \
    if (::logging::kLogLevel <= ::logging::kDebug)                                 \
    {                                                                               \
      ::printf("%s [DEBUG] %s:%d " fmt "\n", ::logging::Now(), __FILE__, __LINE__, ##__VA_ARGS__); \
    }                                                                               \
  } while (0)
#define LOG_INFO(fmt, ...)                                                          \
  do {                                                                              \
    if (::logging::kLogLevel <= ::logging::kInfo)                                  \
    {                                                                               \
      ::printf("%s [INFO ] %s:%d " fmt "\n", ::logging::Now(), __FILE__, __LINE__, ##__VA_ARGS__); \
    }                                                                               \
  } while (0)
#define LOG_WARN(fmt, ...)                                                          \
  do {                                                                              \
    if (::logging::kLogLevel <= ::logging::kWarn)                                  \
    {                                                                               \
      ::printf("%s [WARN ] %s:%d " fmt "\n", ::logging::Now(), __FILE__, __LINE__, ##__VA_ARGS__); \
    }                                                                               \
  } while (0)
#define LOG_ERROR(fmt, ...)                                                         \
  do {                                                                              \
    if (::logging::kLogLevel <= ::logging::kError)                                 \
    {                                                                               \
      ::printf("%s [ERROR] %s:%d " fmt "\n", ::logging::Now(), __FILE__, __LINE__, ##__VA_ARGS__); \
    }                                                                               \
  } while (0)
#elif defined(LOGGING_RELEASE)
#define LOG_DEBUG(fmt, ...) ((void)0)
#define LOG_INFO(fmt, ...) ((void)0)
#define LOG_WARN(fmt, ...) ((void)0)
#define LOG_ERROR(fmt, ...) ((void)0)
#else
#error "log switch: define LOGGING_DEBUG or LOGGING_RELEASE"
#endif

