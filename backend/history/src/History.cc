#include "history/History.hpp"
#include "db/src/Support.hpp"
#include "common/Crypto.hpp"
#include <algorithm>
#include <set>

namespace history
{
namespace
{
using Row = std::vector<std::optional<std::string>>;
const std::string conversation_columns = "id,user_id,title,current_message_id,created_at,updated_at,active_message_id";
const std::string message_columns = "id,conversation_id,parent_id,role,state,content,reasoning,finish_reason,error,created_at,updated_at";
constexpr std::int64_t lease_ms = 300000;
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
          db::Text(row, 5), db::Text(row, 6), db::Text(row, 7), db::Text(row, 8), Time(db::Number(row, 9)), Time(db::Number(row, 10))};
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
void Title(const std::string &title)
{ if (title.empty() || title.size() > 256) throw Error{ErrorCode::InvalidRequest, "对话标题须为 1–256 字节。"}; }
}
struct History::Impl
{
  db::MySql &mysql;
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
  Turn Start(UserId user, ConversationId conversation, const std::string &request_id,
             const std::string &content, std::optional<MessageId> regenerate)
  {
    if (request_id.empty() || request_id.size() > 80 || request_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != request_id.npos)
      throw Error{ErrorCode::InvalidRequest, "请求标识无效。"};
    if (!regenerate && (content.empty() || content.size() > 80000 || content.find_first_not_of(" \r\n\t") == content.npos))
      throw Error{ErrorCode::InvalidRequest, "消息内容为空或过长。"};
    auto hash = crypto::Hash(regenerate ? "regenerate:" + std::to_string(*regenerate) : "message:" + content);
    auto connection = db::Take(mysql.Acquire());
    db::Take(connection.Begin());
    auto row = Owner(connection, user, conversation, true);
    Expire(connection, row);
    auto previous = db::Take(connection.Query("SELECT request_hash,user_message_id,assistant_message_id FROM turn_requests WHERE conversation_id=? AND request_id=?", {conversation, request_id}));
    if (!previous.values.empty())
    {
      auto &request = previous.values.front();
      if (db::Text(request, 0) != hash) throw Error{ErrorCode::Conflict, "同一请求标识不能用于不同的消息。"};
      Turn turn{Read(connection, conversation, db::Number(request, 1)), Read(connection, conversation, db::Number(request, 2)), false};
      db::Take(connection.Commit());
      return turn;
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
    auto inserted = db::Take(connection.Execute("INSERT INTO messages(conversation_id,parent_id,role,state,content,reasoning,created_at,updated_at) VALUES(?,?,'assistant','generating','','',?,?)", {conversation, question.id, now, now}));
    auto answer = Read(connection, conversation, inserted.insert_id);
    db::Take(connection.Execute("INSERT INTO turn_requests(conversation_id,request_id,request_hash,user_message_id,assistant_message_id) VALUES(?,?,?,?,?)", {conversation, request_id, hash, question.id, answer.id}));
    if (regenerate)
      db::Take(connection.Execute("UPDATE conversations SET active_message_id=?,generation_deadline=?,updated_at=? WHERE id=?", {answer.id, now + lease_ms, now, conversation}));
    else
      db::Take(connection.Execute("UPDATE conversations SET current_message_id=?,active_message_id=?,generation_deadline=?,updated_at=? WHERE id=?", {answer.id, answer.id, now + lease_ms, now, conversation}));
    db::Take(connection.Commit());
    return Turn{question, answer, true};
  }
  void Update(UserId user, ConversationId conversation, MessageId message, const Progress &progress, const Completion *completion)
  {
    if (progress.content.size() + progress.reasoning.size() > 4 * 1024 * 1024)
      throw Error{ErrorCode::InvalidRequest, "回复内容过大。"};
    auto connection = db::Take(mysql.Acquire());
    db::Take(connection.Begin());
    auto row = Owner(connection, user, conversation, true);
    auto value = Read(connection, conversation, message);
    auto sequences = db::Take(connection.Query("SELECT sequence FROM messages WHERE id=?", {message}));
    auto sequence = db::Number(sequences.values.front(), 0);
    if (value.role != Role::Assistant) throw Error{ErrorCode::InvalidRequest, "只能保存助手回复。"};
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
    if (!row[6] || db::Number(row, 6) != message || !row[7] || db::Number(row, 7) <= static_cast<std::uint64_t>(db::NowMs()))
      throw Error{ErrorCode::Conflict, "生成已过期，请刷新后重试。"};
    auto now = db::NowMs();
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
};
History::History(db::MySql &mysql) : impl_(std::make_unique<Impl>(Impl{mysql})) {}
History::~History() = default;
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
Result<Turn> History::BeginTurn(UserId user, ConversationId conversation, const std::string &request, const std::string &content)
{ return Guard<Turn>([&] { return impl_->Start(user, conversation, request, content, std::nullopt); }); }
Result<Turn> History::Regenerate(UserId user, ConversationId conversation, MessageId question, const std::string &request)
{ return Guard<Turn>([&] { return impl_->Start(user, conversation, request, "", question); }); }
Status History::SaveProgress(UserId user, ConversationId conversation, MessageId message, const Progress &progress)
{ return Guard<std::monostate>([&] { impl_->Update(user, conversation, message, progress, nullptr); return std::monostate{}; }); }
Status History::Finish(UserId user, ConversationId conversation, MessageId message, const Completion &completion)
{
  return Guard<std::monostate>([&] {
    if (completion.state == MessageState::Generating) throw Error{ErrorCode::InvalidRequest, "结束状态无效。"};
    impl_->Update(user, conversation, message, Progress{completion.sequence, completion.content, completion.reasoning}, &completion);
    return std::monostate{};
  });
}
} // namespace history
