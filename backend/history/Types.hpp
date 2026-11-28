#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace history
{
  using UserId = std::uint64_t;
  using ConversationId = std::uint64_t;
  using MessageId = std::uint64_t;
  using TimePoint = std::chrono::system_clock::time_point;

  enum class ErrorCode
  {
    InvalidRequest,
    NotFound,
    Conflict,
    Unavailable,
    Timeout,
    OutcomeUnknown,
    Internal
  };

  struct Error
  {
    ErrorCode code;
    std::string message;
  };

  template <typename T>
  using Result = std::variant<T, Error>;
  using Status = Result<std::monostate>;

  enum class Role
  {
    User,
    Assistant
  };
  enum class MessageState
  {
    Generating,
    Completed,
    Interrupted,
    Failed
  };

  struct Conversation
  {
    ConversationId id = 0;
    UserId user_id = 0;
    std::string title;
    std::optional<MessageId> current_message_id;
    TimePoint created_at{};
    TimePoint updated_at{};
    std::optional<MessageId> active_message_id = std::nullopt;
  };

  struct ReplyVersions
  {
    std::uint64_t index = 1;
    std::uint64_t total = 1;
    std::optional<MessageId> previous_id;
    std::optional<MessageId> next_id;
  };

  struct GenerationModel
  {
    std::string provider;
    std::string model;
    std::string reasoning_effort;
  };

  struct Message
  {
    MessageId id = 0;
    ConversationId conversation_id = 0;
    std::optional<MessageId> parent_id;
    Role role = Role::User;
    MessageState state = MessageState::Completed;
    std::string content;
    std::string reasoning;
    std::string finish_reason;
    std::string error;
    TimePoint created_at{};
    TimePoint updated_at{};
    std::optional<ReplyVersions> versions = std::nullopt;
    std::string generation_id;
    GenerationModel generation_model;

    bool CanContinue() const;
  };

  struct ConversationView
  {
    Conversation conversation;
    std::vector<Message> messages;
  };

  struct ConversationCursor
  {
    TimePoint updated_at{};
    ConversationId id = 0;
  };

  struct ConversationPage
  {
    std::vector<Conversation> items;
    std::optional<ConversationCursor> next;
  };

  struct MessagePage
  {
    std::vector<Message> items;
    std::optional<MessageId> next_before;
  };

  struct Turn
  {
    Message user_message;
    Message assistant_message;
    bool created = false; // 相同 request_id 重试时为 false，不能重复启动模型调用。
  };

  struct Progress
  {
    std::uint64_t sequence = 0;
    std::string content; // 累计快照。
    std::string reasoning;
  };

  struct Completion
  {
    std::uint64_t sequence = 0;
    MessageState state = MessageState::Completed;
    std::string content;
    std::string reasoning;
    std::string finish_reason;
    std::string error;
  };
} // namespace history
