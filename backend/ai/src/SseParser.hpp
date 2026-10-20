#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace ai
{
  struct SseEvent
  {
    std::string event; // event: 字段；未指定或为空时为 message。
    std::string data;  // 多个 data: 行用换行连接，不解析 JSON。
  };

  // 每个上游响应使用一个解析器，在调用线程内顺序使用。
  // 只解析 SSE 格式；HTTP 收发、模型协议和网页连接由调用方处理。
  class SseParser
  {
  public:
    enum class Status
    {
      Ok,
      Stopped,
      LimitExceeded,
      CallbackError
    };

    // 事件引用仅在回调期间有效；返回 false 停止接收后续事件。
    using EventSink = std::function<bool(const SseEvent &)>;

    // sink 不得为空，max_buffer_bytes 必须大于 0。
    // 限制未处理行及当前事件占用；整个响应的累计大小由客户端限制。
    explicit SseParser(EventSink sink, std::size_t max_buffer_bytes);

    // 接收任意大小的字节片段，支持事件、行和 UTF-8 字符被拆开。
    // 支持 LF、CRLF、CR 换行及开头的 UTF-8 BOM。
    // 空行结束事件，忽略注释和当前无需使用的 id、retry 等字段。
    // Ok 表示片段已处理，不表示模型生成结束；回调异常返回 CallbackError。
    // 非 Ok 状态后停止解析，后续 Feed 保持返回该状态。
    Status Feed(std::string_view bytes);

    // 输入结束，丢弃没有空行结束的不完整事件，不补发事件。
    // 此后不可继续 Feed；上游是否正常完成由协议客户端判断。
    void Finish();

  private:
    Status ProcessLine();
    Status DispatchEvent();

    EventSink sink_;
    std::size_t max_buffer_bytes_;
    std::string line_;
    SseEvent current_;
    bool first_line_ = true;
    bool skip_lf_ = false;
    bool has_data_ = false;
    bool finished_ = false;
    Status status_ = Status::Ok;
  };
} // namespace ai
