#pragma once

#include "Types.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace db
{
  class MySql;
}

namespace history
{
  // 所有读写都校验用户归属；不存在和不属于该用户的记录统一返回 NotFound。
  // MySQL 须比本对象活得更久；同步接口可并发调用，在业务 worker 执行。
  // 不跨模型生成持有数据库事务。
  class History
  {
  public:
    explicit History(db::MySql &mysql);
    ~History();
    History(const History &) = delete;
    History &operator=(const History &) = delete;

    Result<Conversation> Create(UserId user_id, const std::string &title);
    Result<Conversation> Get(UserId user_id, ConversationId conversation_id);
    Result<ConversationPage> List(UserId user_id, std::size_t limit = 50,
                                  const std::optional<ConversationCursor> &before = std::nullopt);
    Status Rename(UserId user_id, ConversationId conversation_id, const std::string &title);
    Status Remove(UserId user_id, ConversationId conversation_id);
    // 分页接口的 limit 必须为正，并受服务端上限约束。
    Result<MessagePage> ListMessages(UserId user_id, ConversationId conversation_id,
                                     std::size_t limit = 200,
                                     std::optional<MessageId> before = std::nullopt);
    // 沿 parent_id 读取指定消息的上下文链，按发送顺序返回可用于模型的消息。
    Result<std::vector<Message>> Context(UserId user_id, ConversationId conversation_id,
                                         MessageId last_message_id, std::size_t limit = 200);

    // 一个短事务保存用户消息和生成中的助手占位；request_id 用于幂等重试。
    // 同一 request_id 携带不同内容返回 Conflict。
    Result<Turn> BeginTurn(UserId user_id, ConversationId conversation_id,
                           const std::string &request_id, const std::string &content);
    // 为原问题创建新的助手回答，生成完成前保留原回答。
    Result<Turn> Regenerate(UserId user_id, ConversationId conversation_id,
                            MessageId user_message_id, const std::string &request_id);
    // sequence 防止乱序快照覆盖新内容，终态消息不能被旧进度重新置为生成中。
    // 对终态写入进度返回 Conflict，调用方应停止旧生成任务。
    Status SaveProgress(UserId user_id, ConversationId conversation_id,
                        MessageId assistant_message_id, const Progress &progress);
    // completion.state 只允许 Completed、Interrupted 或 Failed。
    Status Finish(UserId user_id, ConversationId conversation_id,
                  MessageId assistant_message_id, const Completion &completion);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
} // namespace history
