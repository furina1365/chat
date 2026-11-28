#include "history/History.hpp"
#include "db/src/Support.hpp"
#include "common/AssistantText.hpp"
#include "common/Crypto.hpp"
#include <algorithm>
#include <set>

namespace history
{
namespace
{
using Row = std::vector<std::optional<std::string>>;
const std::string conversation_columns = "id,user_id,title,current_message_id,created_at,updated_at,active_message_id";
const std::string message_columns = "id,conversation_id,parent_id,role,state,content,reasoning,finish_reason,error,created_at,updated_at,generation_id,model_provider,model_name,reasoning_effort";
TimePoint Time(std::uint64_t value) { return TimePoint(std::chrono::milliseconds(value)); }
std::optional<std::uint64_t> OptionalId(const Row &row, std::size_t i)
{ return row.at(i) ? std::optional<std::uint64_t>(db::Number(row, i)) : std::nullopt; }
db::SqlValue Id(std::optional<std::uint64_t> id)
{ return id ? db::SqlValue(*id) : db::SqlValue(nullptr); }
Conversation ReadConversation(const Row &row)
{ return {db::Number(row, 0), db::Number(row, 1), db::Text(row, 2), OptionalId(row, 3), Time(db::Number(row, 4)), Time(db::Number(row, 5)), OptionalId(row, 6)}; }
Message ReadMessage(const Row &row)
{
  auto state = db::Text(row, 4);
  return {db::Number(row, 0), db::Number(row, 1), OptionalId(row, 2),
          db::Text(row, 3) == "user" ? Role::User : Role::Assistant,
          state == "completed" ? MessageState::Completed : state == "interrupted" ? MessageState::Interrupted : state == "failed" ? MessageState::Failed : MessageState::Generating,
          db::Text(row, 5), db::Text(row, 6), db::Text(row, 7), db::Text(row, 8), Time(db::Number(row, 9)), Time(db::Number(row, 10)), {},
          row[11].value_or(""), {row[12].value_or(""), row[13].value_or(""), row[14].value_or("")}};
}
template<class T, class F> Result<T> Guard(F action)
{
  try { return action(); }
  catch (const Error &error) { return error; }
  catch (const db::Error &error)
  {
    auto code = error.code == db::ErrorCode::Conflict ? ErrorCode::Conflict :
                error.code == db::ErrorCode::OutcomeUnknown ? ErrorCode::OutcomeUnknown :
                error.code == db::ErrorCode::Timeout ? ErrorCode::Timeout : ErrorCode::Unavailable;
    return Error{code, code == ErrorCode::OutcomeUnknown ? "保存结果尚未确认，请刷新历史确认。" : "历史服务暂不可用，请稍后重试。"};
  }
  catch (...) { return Error{ErrorCode::Internal, "处理聊天历史失败。"}; }
}
void Limit(std::size_t value, std::size_t maximum)
{ if (!value || value > maximum) throw Error{ErrorCode::InvalidRequest, "分页数量超出范围。"}; }
void ValidateRequestId(const std::string &request_id)
{
  if (request_id.empty() || request_id.size() > 80 ||
      request_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != request_id.npos)
    throw Error{ErrorCode::InvalidRequest, "请求标识无效。"};
}
void Title(const std::string &title)
{ if (title.empty() || title.size() > 256) throw Error{ErrorCode::InvalidRequest, "对话标题须为 1–256 字节。"}; }
}
bool Message::CanContinue() const
{
  return role == Role::Assistant && !generation_id.empty() && !content.empty() &&
      content.size() + reasoning.size() < chat::kMaxAssistantTextBytes &&
      (state == MessageState::Interrupted || state == MessageState::Failed ||
       (state == MessageState::Completed && finish_reason == "length"));
}
struct History::Impl
{
  db::MySql &mysql;
  std::int64_t lease_ms;
  Row Owner(db::MySql::Connection &connection, UserId user, ConversationId conversation, bool lock = false)
  {
    auto rows = db::Take(connection.Query("SELECT " + conversation_columns + ",generation_deadline FROM conversations WHERE id=? AND user_id=?" + (lock ? " FOR UPDATE" : ""), {conversation, user}));
    if (rows.values.empty()) throw Error{ErrorCode::NotFound, "对话不存在。"};
    return rows.values.front();
  }
  Message Read(db::MySql::Connection &connection, ConversationId conversation, MessageId id)
  {
    auto rows = db::Take(connection.Query("SELECT " + message_columns + " FROM messages WHERE id=? AND conversation_id=?", {id, conversation}));
    if (rows.values.empty()) throw Error{ErrorCode::NotFound, "消息不存在。"};
    return ReadMessage(rows.values.front());
  }
  std::vector<Message> Context(db::MySql::Connection &connection, ConversationId conversation, MessageId last, std::size_t limit)
  {
    std::vector<Message> messages;
    std::set<MessageId> seen;
    for (std::optional<MessageId> id = last; id;)
    {
      if (messages.size() >= limit || !seen.insert(*id).second)
        throw Error{ErrorCode::InvalidRequest, "对话过长，请新建对话继续。"};
      auto message = Read(connection, conversation, *id);
      id = message.parent_id;
      messages.push_back(std::move(message));
    }
    std::reverse(messages.begin(), messages.end());
    return messages;
  }
  ConversationView View(db::MySql::Connection &connection, const Row &row)
  {
    ConversationView view{ReadConversation(row), {}};
    if (!view.conversation.current_message_id) return view;
    view.messages = Context(connection, view.conversation.id, *view.conversation.current_message_id, 500);
    std::string placeholders;
    db::SqlParams params{view.conversation.id};
    for (const auto &message : view.messages)
      if (message.role == Role::Assistant && message.state != MessageState::Generating)
      {
        placeholders += placeholders.empty() ? "?" : ",?";
        params.emplace_back(message.id);
      }
    if (placeholders.empty()) return view;
    auto versions = db::Take(connection.Query(
        "SELECT selected.id,COUNT(sibling.id),SUM(sibling.id<=selected.id),"
        "MAX(CASE WHEN sibling.id<selected.id THEN sibling.id END),"
        "MIN(CASE WHEN sibling.id>selected.id THEN sibling.id END) "
        "FROM messages selected JOIN messages sibling ON sibling.conversation_id=selected.conversation_id "
        "AND sibling.parent_id=selected.parent_id AND sibling.role='assistant' AND sibling.state<>'generating' "
        "WHERE selected.conversation_id=? AND selected.id IN (" + placeholders + ") GROUP BY selected.id", params));
    for (const auto &version : versions.values)
      for (auto &message : view.messages)
        if (message.id == db::Number(version, 0))
        {
          message.versions = ReplyVersions{db::Number(version, 2), db::Number(version, 1), OptionalId(version, 3), OptionalId(version, 4)};
          break;
        }
    return view;
  }
  void Expire(db::MySql::Connection &connection, Row &row)
  {
    if (row[6] && db::Number(row, 7) <= static_cast<std::uint64_t>(db::NowMs()))
    {
      db::Take(connection.Execute("UPDATE messages SET state='interrupted',error='生成连接已中断，请重试。',updated_at=? WHERE id=? AND state='generating'", {db::NowMs(), db::Number(row, 6)}));
      db::Take(connection.Execute("UPDATE conversations SET active_message_id=NULL,generation_deadline=NULL WHERE id=?", {db::Number(row, 0)}));
      row[6] = std::nullopt;
      row[7] = std::nullopt;
    }
  }
  std::optional<Turn> Replay(db::MySql::Connection &connection, ConversationId conversation,
                             const std::string &request_id, const std::string &hash)
  {
    auto previous = db::Take(connection.Query(
        "SELECT request_hash,user_message_id,assistant_message_id,replay_content_bytes,"
        "replay_reasoning_bytes,replay_finish_reason FROM turn_requests WHERE conversation_id=? AND request_id=?",
        {conversation, request_id}));
    if (previous.values.empty()) return std::nullopt;
    const auto &request = previous.values.front();
    if (db::Text(request, 0) != hash)
      throw Error{ErrorCode::Conflict, "同一请求标识不能用于不同的消息。"};
    auto answer = Read(connection, conversation, db::Number(request, 2));
    if (!answer.generation_id.empty() && answer.generation_id != request_id)
    {
      // The same message was reopened. Reconstruct the earlier result from the
      // append-only text and the boundary saved when it was reopened.
      if (request[3])
      {
        answer.content.erase(db::Number(request, 3));
        answer.reasoning.erase(db::Number(request, 4));
        answer.finish_reason = db::Text(request, 5);
        answer.state = MessageState::Completed;
      }
      else
        answer.state = MessageState::Interrupted;
    }
    return Turn{Read(connection, conversation, db::Number(request, 1)), std::move(answer), false};
  }
  void Remember(db::MySql::Connection &connection, ConversationId conversation,
                const std::string &request_id, const std::string &hash, MessageId question, MessageId answer)
  {
    db::Take(connection.Execute(
        "INSERT INTO turn_requests(conversation_id,request_id,request_hash,user_message_id,assistant_message_id) VALUES(?,?,?,?,?)",
        {conversation, request_id, hash, question, answer}));
  }
  Turn Start(UserId user, ConversationId conversation, const std::string &request_id,
             const std::string &content, std::optional<MessageId> regenerate, const GenerationModel &model)
  {
    ValidateRequestId(request_id);
    if (!regenerate && (content.empty() || content.size() > 80000 || content.find_first_not_of(" \r\n\t") == content.npos))
      throw Error{ErrorCode::InvalidRequest, "消息内容为空或过长。"};
    auto hash = crypto::Hash(regenerate ? "regenerate:" + std::to_string(*regenerate) : "message:" + content);
    auto connection = db::Take(mysql.Acquire());
    db::Take(connection.Begin());
    auto row = Owner(connection, user, conversation, true);
    Expire(connection, row);
    if (auto previous = Replay(connection, conversation, request_id, hash))
    {
      db::Take(connection.Commit());
      return std::move(*previous);
    }
    if (row[6] && db::Number(row, 7) > static_cast<std::uint64_t>(db::NowMs()))
      throw Error{ErrorCode::Conflict, "当前对话正在生成回复，请稍后重试。"};
    auto now = db::NowMs();
    Message question;
    if (regenerate)
    {
      question = Read(connection, conversation, *regenerate);
      if (question.role != Role::User) throw Error{ErrorCode::InvalidRequest, "只能为用户问题重新生成回复。"};
    }
    else
    {
      auto parent = OptionalId(row, 3);
      if (parent && Read(connection, conversation, *parent).role != Role::Assistant)
        throw Error{ErrorCode::Conflict, "上一条消息还没有回复，请先重试。"};
      auto inserted = db::Take(connection.Execute("INSERT INTO messages(conversation_id,parent_id,role,state,content,reasoning,created_at,updated_at) VALUES(?,?,'user','completed',?,'',?,?)", {conversation, Id(parent), content, now, now}));
      question = Read(connection, conversation, inserted.insert_id);
    }
    auto inserted = db::Take(connection.Execute(
        "INSERT INTO messages(conversation_id,parent_id,role,state,content,reasoning,created_at,updated_at,"
        "generation_id,model_provider,model_name,reasoning_effort) VALUES(?,?,'assistant','generating','','',?,?,?,?,?,?)",
        {conversation, question.id, now, now, request_id, model.provider, model.model, model.reasoning_effort}));
    auto answer = Read(connection, conversation, inserted.insert_id);
    Remember(connection, conversation, request_id, hash, question.id, answer.id);
    if (regenerate)
      db::Take(connection.Execute("UPDATE conversations SET active_message_id=?,generation_deadline=?,updated_at=? WHERE id=?", {answer.id, now + lease_ms, now, conversation}));
    else
      db::Take(connection.Execute("UPDATE conversations SET current_message_id=?,active_message_id=?,generation_deadline=?,updated_at=? WHERE id=?", {answer.id, answer.id, now + lease_ms, now, conversation}));
    db::Take(connection.Commit());
    return Turn{question, answer, true};
  }
  Turn Continue(UserId user, ConversationId conversation, MessageId message, const std::string &revision,
                const std::string &request_id, const std::function<bool(const GenerationModel &)> &supports_prefix)
  {
    ValidateRequestId(request_id);
    ValidateRequestId(revision);
    const auto hash = crypto::Hash("continue:" + std::to_string(message) + ":" + revision);
    auto connection = db::Take(mysql.Acquire());
    db::Take(connection.Begin());
    auto row = Owner(connection, user, conversation, true);
    Expire(connection, row);
    if (auto previous = Replay(connection, conversation, request_id, hash))
    {
      db::Take(connection.Commit());
      return std::move(*previous);
    }
    auto answer = Read(connection, conversation, message);
    if (row[6] || OptionalId(row, 3) != message || answer.generation_id != revision)
      throw Error{ErrorCode::Conflict, "当前回复已变化，请刷新历史后继续生成。"};
    if (!answer.CanContinue() || !answer.parent_id || !supports_prefix(answer.generation_model))
      throw Error{ErrorCode::InvalidRequest, "此回复无法继续生成。"};
    auto question = Read(connection, conversation, *answer.parent_id);
    if (question.role != Role::User)
      throw Error{ErrorCode::InvalidRequest, "此回复缺少原问题。"};

    if (answer.state == MessageState::Completed)
      db::Take(connection.Execute(
          "UPDATE turn_requests SET replay_content_bytes=?,replay_reasoning_bytes=?,replay_finish_reason=? "
          "WHERE conversation_id=? AND request_id=?",
          {static_cast<std::uint64_t>(answer.content.size()), static_cast<std::uint64_t>(answer.reasoning.size()),
           answer.finish_reason, conversation, answer.generation_id}));
    auto now = db::NowMs();
    db::Take(connection.Execute(
        "UPDATE messages SET generation_id=?,state='generating',sequence=0,finish_reason='',error='',updated_at=? WHERE id=?",
        {request_id, now, message}));
    db::Take(connection.Execute(
        "UPDATE conversations SET active_message_id=?,generation_deadline=?,updated_at=? WHERE id=?",
        {message, now + lease_ms, now, conversation}));
    Remember(connection, conversation, request_id, hash, question.id, message);
    db::Take(connection.Commit());
    answer.generation_id = request_id;
    return Turn{std::move(question), std::move(answer), true};
  }
  void Update(UserId user, ConversationId conversation, MessageId message, const std::string &generation_id,
              const Progress &progress, const Completion *completion)
  {
    if (progress.content.size() + progress.reasoning.size() > chat::kMaxAssistantTextBytes)
      throw Error{ErrorCode::InvalidRequest, "回复内容过大。"};
    auto connection = db::Take(mysql.Acquire());
    db::Take(connection.Begin());
    auto row = Owner(connection, user, conversation, true);
    auto value = Read(connection, conversation, message);
    if (value.generation_id != generation_id)
      throw Error{ErrorCode::Conflict, "回复已继续生成，请停止旧的生成任务。"};
    auto sequences = db::Take(connection.Query("SELECT sequence FROM messages WHERE id=?", {message}));
    auto sequence = db::Number(sequences.values.front(), 0);
    if (value.role != Role::Assistant) throw Error{ErrorCode::InvalidRequest, "只能保存助手回复。"};
    auto now = db::NowMs();
    if (value.state == MessageState::Generating &&
        (!row[6] || db::Number(row, 6) != message || !row[7] || db::Number(row, 7) <= static_cast<std::uint64_t>(now)))
      throw Error{ErrorCode::Conflict, "生成已过期，请刷新后重试。"};
    if (value.state != MessageState::Generating || sequence >= progress.sequence)
    {
      if (!completion && value.state != MessageState::Generating)
        throw Error{ErrorCode::Conflict, "回复已结束，请停止旧的生成任务。"};
      if (completion && value.state != completion->state) throw Error{ErrorCode::Conflict, "回复已结束，不能覆盖。"};
      if (completion && (value.content != progress.content || value.reasoning != progress.reasoning || value.finish_reason != completion->finish_reason || value.error != completion->error))
        throw Error{ErrorCode::Conflict, "回复已保存，请刷新确认。"};
      db::Take(connection.Commit());
      return;
    }
    if (progress.content.compare(0, value.content.size(), value.content) != 0 ||
        progress.reasoning.compare(0, value.reasoning.size(), value.reasoning) != 0)
      throw Error{ErrorCode::Conflict, "已保存的回复不能覆盖。"};
    if (completion)
    {
      auto state = completion->state == MessageState::Completed ? "completed" : completion->state == MessageState::Interrupted ? "interrupted" : "failed";
      db::Take(connection.Execute("UPDATE messages SET content=?,reasoning=?,sequence=?,state=?,finish_reason=?,error=?,updated_at=? WHERE id=?", {progress.content, progress.reasoning, progress.sequence, std::string(state), completion->finish_reason, completion->error, now, message}));
      if (completion->state == MessageState::Completed)
        db::Take(connection.Execute("UPDATE conversations SET current_message_id=?,active_message_id=NULL,generation_deadline=NULL,updated_at=? WHERE id=?", {message, now, conversation}));
      else
        db::Take(connection.Execute("UPDATE conversations SET active_message_id=NULL,generation_deadline=NULL,updated_at=? WHERE id=?", {now, conversation}));
    }
    else
    {
      db::Take(connection.Execute("UPDATE messages SET content=?,reasoning=?,sequence=?,updated_at=? WHERE id=? AND state='generating'", {progress.content, progress.reasoning, progress.sequence, now, message}));
      db::Take(connection.Execute("UPDATE conversations SET generation_deadline=? WHERE id=?", {now + lease_ms, conversation}));
    }
    db::Take(connection.Commit());
  }
  void Renew(UserId user, ConversationId conversation, MessageId message, const std::string &generation_id)
  {
    auto connection = db::Take(mysql.Acquire());
    db::Take(connection.Begin());
    auto row = Owner(connection, user, conversation, true);
    auto value = Read(connection, conversation, message);
    if (value.role != Role::Assistant) throw Error{ErrorCode::InvalidRequest, "只能续租助手回复。"};
    if (value.state != MessageState::Generating || value.generation_id != generation_id)
      throw Error{ErrorCode::Conflict, "回复已结束，请停止旧的生成任务。"};
    auto now = db::NowMs();
    if (!row[6] || db::Number(row, 6) != message || !row[7] || db::Number(row, 7) <= static_cast<std::uint64_t>(now))
      throw Error{ErrorCode::Conflict, "生成已过期，请刷新后重试。"};
    db::Take(connection.Execute(
        "UPDATE conversations SET generation_deadline=? WHERE id=? AND active_message_id=?",
        {now + lease_ms, conversation, message}));
    db::Take(connection.Commit());
  }
};
History::History(db::MySql &mysql, std::chrono::milliseconds lease)
    : impl_(std::make_unique<Impl>(Impl{mysql, lease.count()}))
{
  if (lease.count() <= 0)
    throw std::invalid_argument("History generation lease must be positive");
}
History::~History() = default;
std::chrono::milliseconds History::lease() const { return std::chrono::milliseconds(impl_->lease_ms); }
Result<Conversation> History::Create(UserId user, const std::string &title)
{
  return Guard<Conversation>([&] {
    Title(title);
    auto connection = db::Take(impl_->mysql.Acquire());
    auto now = db::NowMs();
    auto result = db::Take(connection.Execute("INSERT INTO conversations(user_id,title,created_at,updated_at) VALUES(?,?,?,?)", {user, title, now, now}));
    return Conversation{result.insert_id, user, title, std::nullopt, Time(now), Time(now)};
  });
}
Result<Conversation> History::Get(UserId user, ConversationId conversation)
{
  return Guard<Conversation>([&] {
    auto connection = db::Take(impl_->mysql.Acquire());
    db::Take(connection.Begin());
    auto row = impl_->Owner(connection, user, conversation, true);
    impl_->Expire(connection, row);
    db::Take(connection.Commit());
    return ReadConversation(row);
  });
}
Result<ConversationView> History::GetView(UserId user, ConversationId conversation)
{
  return Guard<ConversationView>([&] {
    auto connection = db::Take(impl_->mysql.Acquire());
    db::Take(connection.Begin());
    auto row = impl_->Owner(connection, user, conversation, true);
    impl_->Expire(connection, row);
    auto view = impl_->View(connection, row);
    db::Take(connection.Commit());
    return view;
  });
}
Result<ConversationView> History::SelectReply(UserId user, ConversationId conversation, MessageId reply)
{
  return Guard<ConversationView>([&] {
    auto connection = db::Take(impl_->mysql.Acquire());
    db::Take(connection.Begin());
    auto row = impl_->Owner(connection, user, conversation, true);
    impl_->Expire(connection, row);
    if (row[6]) throw Error{ErrorCode::Conflict, "请等待当前回复生成结束后再切换版本。"};
    auto message = impl_->Read(connection, conversation, reply);
    if (message.role != Role::Assistant || message.state == MessageState::Generating)
      throw Error{ErrorCode::InvalidRequest, "请选择已结束生成的助手回复。"};
    auto last = reply;
    std::set<MessageId> seen{reply};
    for (;;)
    {
      auto children = db::Take(connection.Query(
          "SELECT id FROM messages WHERE conversation_id=? AND parent_id=? AND state<>'generating' ORDER BY id DESC LIMIT 1",
          {conversation, last}));
      if (children.values.empty()) break;
      last = db::Number(children.values.front(), 0);
      if (seen.size() >= 500 || !seen.insert(last).second)
        throw Error{ErrorCode::InvalidRequest, "对话过长，请新建对话继续。"};
    }
    auto now = db::NowMs();
    db::Take(connection.Execute("UPDATE conversations SET current_message_id=?,updated_at=? WHERE id=? AND user_id=?", {last, now, conversation, user}));
    row[3] = std::to_string(last);
    row[5] = std::to_string(now);
    auto view = impl_->View(connection, row);
    db::Take(connection.Commit());
    return view;
  });
}
Result<ConversationPage> History::List(UserId user, std::size_t limit, const std::optional<ConversationCursor> &before)
{
  return Guard<ConversationPage>([&] {
    Limit(limit, 100);
    auto connection = db::Take(impl_->mysql.Acquire());
    std::string sql = "SELECT " + conversation_columns + " FROM conversations WHERE user_id=?";
    db::SqlParams params{user};
    if (before)
    {
      auto time = std::chrono::duration_cast<std::chrono::milliseconds>(before->updated_at.time_since_epoch()).count();
      sql += " AND (updated_at<? OR (updated_at=? AND id<?))";
      params.insert(params.end(), {time, time, before->id});
    }
    sql += " ORDER BY updated_at DESC,id DESC LIMIT ?";
    params.emplace_back(static_cast<std::uint64_t>(limit + 1));
    auto rows = db::Take(connection.Query(sql, params));
    ConversationPage page;
    for (std::size_t i = 0; i < std::min(limit, rows.values.size()); ++i) page.items.push_back(ReadConversation(rows.values[i]));
    if (rows.values.size() > limit) page.next = ConversationCursor{page.items.back().updated_at, page.items.back().id};
    return page;
  });
}
Status History::Rename(UserId user, ConversationId conversation, const std::string &title)
{
  return Guard<std::monostate>([&] {
    Title(title);
    auto connection = db::Take(impl_->mysql.Acquire());
    db::Take(connection.Begin());
    impl_->Owner(connection, user, conversation, true);
    db::Take(connection.Execute("UPDATE conversations SET title=?,updated_at=? WHERE id=? AND user_id=?", {title, db::NowMs(), conversation, user}));
    db::Take(connection.Commit());
    return std::monostate{};
  });
}
Status History::Remove(UserId user, ConversationId conversation)
{
  return Guard<std::monostate>([&] {
    auto connection = db::Take(impl_->mysql.Acquire());
    db::Take(connection.Begin());
    impl_->Owner(connection, user, conversation, true);
    db::Take(connection.Execute("DELETE FROM conversations WHERE id=? AND user_id=?", {conversation, user}));
    db::Take(connection.Commit());
    return std::monostate{};
  });
}
Result<MessagePage> History::ListMessages(UserId user, ConversationId conversation, std::size_t limit, std::optional<MessageId> before)
{
  return Guard<MessagePage>([&] {
    Limit(limit, 500);
    auto connection = db::Take(impl_->mysql.Acquire());
    impl_->Owner(connection, user, conversation);
    auto sql = "SELECT " + message_columns + " FROM messages WHERE conversation_id=?";
    db::SqlParams params{conversation};
    if (before) { sql += " AND id<?"; params.emplace_back(*before); }
    sql += " ORDER BY id DESC LIMIT ?";
    params.emplace_back(static_cast<std::uint64_t>(limit + 1));
    auto rows = db::Take(connection.Query(sql, params));
    MessagePage page;
    for (std::size_t i = 0; i < std::min(limit, rows.values.size()); ++i) page.items.push_back(ReadMessage(rows.values[i]));
    if (rows.values.size() > limit) page.next_before = page.items.back().id;
    std::reverse(page.items.begin(), page.items.end());
    return page;
  });
}
Result<std::vector<Message>> History::Context(UserId user, ConversationId conversation, MessageId last, std::size_t limit)
{
  return Guard<std::vector<Message>>([&] {
    Limit(limit, 500);
    auto connection = db::Take(impl_->mysql.Acquire());
    impl_->Owner(connection, user, conversation);
    return impl_->Context(connection, conversation, last, limit);
  });
}
Result<Turn> History::BeginTurn(UserId user, ConversationId conversation, const std::string &request,
                                const std::string &content, const GenerationModel &model)
{ return Guard<Turn>([&] { return impl_->Start(user, conversation, request, content, std::nullopt, model); }); }
Result<Turn> History::Regenerate(UserId user, ConversationId conversation, MessageId question,
                                 const std::string &request, const GenerationModel &model)
{ return Guard<Turn>([&] { return impl_->Start(user, conversation, request, "", question, model); }); }
Result<Turn> History::Continue(UserId user, ConversationId conversation, MessageId message,
                               const std::string &revision, const std::string &request,
                               const std::function<bool(const GenerationModel &)> &supports_prefix)
{ return Guard<Turn>([&] { return impl_->Continue(user, conversation, message, revision, request, supports_prefix); }); }
Status History::SaveProgress(UserId user, ConversationId conversation, MessageId message,
                             const std::string &generation_id, const Progress &progress)
{ return Guard<std::monostate>([&] { impl_->Update(user, conversation, message, generation_id, progress, nullptr); return std::monostate{}; }); }
Status History::RenewLease(UserId user, ConversationId conversation, MessageId message, const std::string &generation_id)
{ return Guard<std::monostate>([&] { impl_->Renew(user, conversation, message, generation_id); return std::monostate{}; }); }
Status History::Finish(UserId user, ConversationId conversation, MessageId message,
                       const std::string &generation_id, const Completion &completion)
{
  return Guard<std::monostate>([&] {
    if (completion.state == MessageState::Generating) throw Error{ErrorCode::InvalidRequest, "结束状态无效。"};
    impl_->Update(user, conversation, message, generation_id,
                  Progress{completion.sequence, completion.content, completion.reasoning}, &completion);
    return std::monostate{};
  });
}
} // namespace history
