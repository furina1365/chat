#pragma once

#include "http/HttpRequest.hpp"
#include <llhttp.h>
#include <string_view>

// 每个连接一个解析器；Feed 只接收新到的字节，半包状态由 llhttp 保存。
class HttpParser
{
public:
  enum class Status { NeedMore, Complete, Error };
  static constexpr std::size_t kMaxBody = 256 * 1024;
  static constexpr std::size_t kMaxHeader = 16384 + 4;

  HttpParser();
  HttpParser(const HttpParser &) = delete;
  HttpParser &operator=(const HttpParser &) = delete;

  Status Feed(std::string_view data);
  Status Finish();
  const HttpRequest &request() const { return request_; }
  int errorStatus() const { return error_status_; }
  const char *errorMessage() const { return error_message_; }

private:
  static int OnUrl(llhttp_t *, const char *, std::size_t);
  static int OnHeaderField(llhttp_t *, const char *, std::size_t);
  static int OnHeaderValue(llhttp_t *, const char *, std::size_t);
  static int OnHeaderComplete(llhttp_t *);
  static int OnHeadersComplete(llhttp_t *);
  static int OnBody(llhttp_t *, const char *, std::size_t);
  static int OnMessageComplete(llhttp_t *);

  int Fail(int status, const char *message);
  Status status() const;

  llhttp_settings_t settings_{};
  llhttp_t parser_{};
  HttpRequest request_;
  std::string header_name_, header_value_;
  std::size_t header_bytes_ = 0;
  bool headers_done_ = false, complete_ = false;
  int error_status_ = 0;
  const char *error_message_ = "";
};
