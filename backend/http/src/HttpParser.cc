#include "HttpParser.hpp"
#include <algorithm>

HttpParser::HttpParser()
{
  llhttp_settings_init(&settings_);
  settings_.on_url = OnUrl;
  settings_.on_header_field = OnHeaderField;
  settings_.on_header_value = OnHeaderValue;
  settings_.on_header_value_complete = OnHeaderComplete;
  settings_.on_headers_complete = OnHeadersComplete;
  settings_.on_body = OnBody;
  settings_.on_message_complete = OnMessageComplete;
  llhttp_init(&parser_, HTTP_REQUEST, &settings_);
  parser_.data = this;
}

HttpParser::Status HttpParser::Feed(std::string_view data)
{
  if (status() != Status::NeedMore)
    return status();
  if (data.empty())
    return Status::NeedMore;

  if (!headers_done_)
  {
    auto count = std::min(data.size(), kMaxHeader - header_bytes_);
    auto error = llhttp_execute(&parser_, data.data(), count);
    if (error == HPE_PAUSED && headers_done_)
    {
      // 在头部末尾暂停，精确统计原始字节（含空白），不把 body 算进请求头。
      auto used = static_cast<std::size_t>(llhttp_get_error_pos(&parser_) - data.data());
      header_bytes_ += used;
      data.remove_prefix(used);
      llhttp_resume(&parser_);
    }
    else
    {
      header_bytes_ += count;
      if (error != HPE_OK && !error_status_)
        Fail(400, "HTTP 请求格式无效。");
      else if (!error_status_ && header_bytes_ == kMaxHeader)
        Fail(431, "请求头过大。");
      return status();
    }
  }

  auto error = llhttp_execute(&parser_, data.data(), data.size());
  if (error != HPE_OK && !(error == HPE_PAUSED && complete_) && !error_status_)
    Fail(400, "HTTP 请求格式无效。");
  return status();
}

HttpParser::Status HttpParser::Finish()
{
  if (status() == Status::NeedMore)
  {
    llhttp_finish(&parser_);
    if (!complete_)
      Fail(400, headers_done_ ? "请求体不完整。" : "请求头不完整。");
  }
  return status();
}

int HttpParser::OnUrl(llhttp_t *parser, const char *data, std::size_t size)
{
  static_cast<HttpParser *>(parser->data)->request_.path.append(data, size);
  return 0;
}

int HttpParser::OnHeaderField(llhttp_t *parser, const char *data, std::size_t size)
{
  auto &name = static_cast<HttpParser *>(parser->data)->header_name_;
  for (std::size_t i = 0; i < size; ++i)
  {
    unsigned char c = data[i];
    name.push_back(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
  }
  return 0;
}

int HttpParser::OnHeaderValue(llhttp_t *parser, const char *data, std::size_t size)
{
  static_cast<HttpParser *>(parser->data)->header_value_.append(data, size);
  return 0;
}

int HttpParser::OnHeaderComplete(llhttp_t *parser)
{
  auto &self = *static_cast<HttpParser *>(parser->data);
  if (self.header_name_ == "transfer-encoding")
    return self.Fail(400, "不支持 Transfer-Encoding 请求体。");
  self.request_.headers.emplace_back(std::move(self.header_name_), std::move(self.header_value_));
  self.header_name_.clear();
  self.header_value_.clear();
  return 0;
}

int HttpParser::OnHeadersComplete(llhttp_t *parser)
{
  auto &self = *static_cast<HttpParser *>(parser->data);
  if (parser->http_major != 1 || parser->http_minor != 1 || parser->upgrade)
    return self.Fail(400, "仅支持 HTTP/1.1 普通请求。");
  self.request_.method = llhttp_method_name(static_cast<llhttp_method_t>(parser->method));
  if (self.request_.method == "POST" && !(parser->flags & F_CONTENT_LENGTH))
    return self.Fail(411, "缺少 Content-Length。");
  if (parser->content_length > kMaxBody)
    return self.Fail(413, "请求内容过大。");
  self.headers_done_ = true;
  return HPE_PAUSED;
}

int HttpParser::OnBody(llhttp_t *parser, const char *data, std::size_t size)
{
  auto &self = *static_cast<HttpParser *>(parser->data);
  if (size > kMaxBody - self.request_.body.size())
    return self.Fail(413, "请求内容过大。");
  self.request_.body.append(data, size);
  return 0;
}

int HttpParser::OnMessageComplete(llhttp_t *parser)
{
  static_cast<HttpParser *>(parser->data)->complete_ = true;
  // 当前服务每个连接只处理一个请求，不继续解析流水线中的下一条。
  return HPE_PAUSED;
}

int HttpParser::Fail(int status, const char *message)
{
  error_status_ = status;
  error_message_ = message;
  return HPE_USER;
}

HttpParser::Status HttpParser::status() const
{
  if (error_status_)
    return Status::Error;
  return complete_ ? Status::Complete : Status::NeedMore;
}
