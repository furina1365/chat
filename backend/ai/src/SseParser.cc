#include "SseParser.hpp"

#include <stdexcept>
#include <utility>

namespace ai
{
SseParser::SseParser(EventSink sink, std::size_t max_buffer_bytes)
    : sink_(std::move(sink)), max_buffer_bytes_(max_buffer_bytes)
{
  if (!sink_ || !max_buffer_bytes_) throw std::invalid_argument("无效的 SSE 解析器参数");
}

SseParser::Status SseParser::Feed(std::string_view bytes)
{
  if (status_ != Status::Ok) return status_;
  if (finished_) return status_ = Status::Stopped;
  for (char c : bytes)
  {
    if (skip_lf_)
    {
      skip_lf_ = false;
      if (c == '\n') continue;
    }
    if (c == '\r' || c == '\n')
    {
      skip_lf_ = c == '\r';
      status_ = ProcessLine();
      line_.clear();
      if (status_ != Status::Ok) return status_;
    }
    else
    {
      if (line_.size() + current_.data.size() + current_.event.size() >= max_buffer_bytes_)
        return status_ = Status::LimitExceeded;
      line_ += c;
    }
  }
  return status_;
}

SseParser::Status SseParser::ProcessLine()
{
  std::string_view line = line_;
  if (first_line_)
  {
    first_line_ = false;
    if (line.substr(0, 3) == "\xef\xbb\xbf") line.remove_prefix(3);
  }
  if (line.empty()) return DispatchEvent();
  if (line.front() == ':') return Status::Ok;
  const auto colon = line.find(':');
  auto name = line.substr(0, colon);
  auto value = colon == line.npos ? std::string_view{} : line.substr(colon + 1);
  if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
  if (name == "event") current_.event.assign(value);
  else if (name == "data")
  {
    if (current_.data.size() + value.size() + current_.event.size() + 1 > max_buffer_bytes_)
      return Status::LimitExceeded;
    current_.data.append(value);
    current_.data += '\n';
    has_data_ = true;
  }
  return Status::Ok;
}

SseParser::Status SseParser::DispatchEvent()
{
  Status status = Status::Ok;
  if (has_data_)
  {
    current_.data.pop_back();
    if (current_.event.empty()) current_.event = "message";
    try { if (!sink_(current_)) status = Status::Stopped; }
    catch (...) { status = Status::CallbackError; }
  }
  current_ = {};
  has_data_ = false;
  return status;
}

void SseParser::Finish()
{
  finished_ = true;
  line_.clear();
  current_ = {};
  has_data_ = false;
}
} // namespace ai
