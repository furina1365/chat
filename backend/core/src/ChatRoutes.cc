#include "ChatRoutes.hpp"
#include "HttpSupport.hpp"
#include "ai/ModelManager.hpp"
#include "common/AssistantText.hpp"
#include "history/History.hpp"
#include "http/HttpRouter.hpp"
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace chat
{
namespace
{
class LeaseRenewer
{
public:
  struct Entry
  {
    history::UserId user = 0;
    history::ConversationId conversation = 0;
    history::MessageId message = 0;
    std::string generation_id;
    std::shared_ptr<const std::atomic<bool>> client_cancel;
    std::shared_ptr<std::atomic<bool>> model_cancel = std::make_shared<std::atomic<bool>>(false);
    std::chrono::steady_clock::time_point next_renew{};
    bool stopped = false;
    bool renewing = false;
    std::optional<history::Error> lost_error;
  };

  class Handle
  {
  public:
    Handle(std::shared_ptr<LeaseRenewer> owner, std::shared_ptr<Entry> entry)
        : owner_(std::move(owner)), entry_(std::move(entry)) {}
    ~Handle() { Stop(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    const std::shared_ptr<std::atomic<bool>> &modelCancel() const { return entry_->model_cancel; }
    void Touch() { owner_->Touch(entry_); }
    void Stop() { owner_->StopEntry(entry_); }
    std::optional<history::Error> LostError() const { return owner_->LostError(entry_); }

  private:
    std::shared_ptr<LeaseRenewer> owner_;
    std::shared_ptr<Entry> entry_;
  };

  explicit LeaseRenewer(history::History &history)
      : history_(history),
        renew_interval_(std::min(std::chrono::milliseconds(60000),
                                 std::max(std::chrono::milliseconds(100), history.lease() / 3))),
        retry_interval_(std::min(std::chrono::milliseconds(5000),
                                 std::max(std::chrono::milliseconds(100), renew_interval_ / 4)))
  {}

  ~LeaseRenewer()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      entries_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable())
      thread_.join();
  }

  static std::shared_ptr<Handle> Track(const std::shared_ptr<LeaseRenewer> &self,
                                       history::UserId user, history::ConversationId conversation,
                                       const history::Message &message,
                                       std::shared_ptr<const std::atomic<bool>> client_cancel)
  {
    auto entry = std::make_shared<Entry>();
    entry->user = user;
    entry->conversation = conversation;
    entry->message = message.id;
    entry->generation_id = message.generation_id;
    entry->client_cancel = std::move(client_cancel);
    entry->next_renew = std::chrono::steady_clock::now() + self->renew_interval_;
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      if (!self->stopping_)
      {
        if (!self->thread_.joinable())
          self->thread_ = std::thread([raw = self.get()] { raw->Run(); });
        self->entries_.push_back(entry);
      }
    }
    self->cv_.notify_one();
    return std::make_shared<Handle>(self, std::move(entry));
  }

  void Touch(const std::shared_ptr<Entry> &entry)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!entry->stopped)
      entry->next_renew = std::chrono::steady_clock::now() + renew_interval_;
  }

  void StopEntry(const std::shared_ptr<Entry> &entry)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    entry->stopped = true;
    entries_.erase(std::remove(entries_.begin(), entries_.end(), entry), entries_.end());
    done_cv_.wait(lock, [&] { return !entry->renewing; });
  }

  std::optional<history::Error> LostError(const std::shared_ptr<Entry> &entry) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return entry->lost_error;
  }

private:
  void Run()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
      if (stopping_)
        return;
      if (entries_.empty())
      {
        cv_.wait(lock, [this] { return stopping_ || !entries_.empty(); });
        continue;
      }
      auto now = std::chrono::steady_clock::now();
      std::shared_ptr<Entry> earliest;
      for (auto it = entries_.begin(); it != entries_.end();)
      {
        auto &entry = *it;
        if (entry->stopped || (entry->client_cancel && entry->client_cancel->load()))
        {
          entry->stopped = true;
          it = entries_.erase(it);
          continue;
        }
        if (!earliest || entry->next_renew < earliest->next_renew)
          earliest = entry;
        ++it;
      }
      if (!earliest)
        continue;
      if (earliest->next_renew > now)
      {
        cv_.wait_until(lock, earliest->next_renew);
        continue;
      }
      auto due = std::move(earliest);
      due->renewing = true;
      auto user = due->user;
      auto conversation = due->conversation;
      auto message = due->message;
      lock.unlock();

      auto status = history_.RenewLease(user, conversation, message, due->generation_id);

      lock.lock();
      due->renewing = false;
      done_cv_.notify_all();
      if (stopping_ || due->stopped || (due->client_cancel && due->client_cancel->load()))
      {
        due->stopped = true;
        entries_.erase(std::remove(entries_.begin(), entries_.end(), due), entries_.end());
        continue;
      }
      if (auto *error = std::get_if<history::Error>(&status))
      {
        if (error->code == history::ErrorCode::Conflict ||
            error->code == history::ErrorCode::NotFound ||
            error->code == history::ErrorCode::InvalidRequest)
        {
          due->lost_error = *error;
          due->model_cancel->store(true);
          due->stopped = true;
          entries_.erase(std::remove(entries_.begin(), entries_.end(), due), entries_.end());
        }
        else
        {
          due->next_renew = std::chrono::steady_clock::now() + retry_interval_;
        }
      }
      else
      {
        due->next_renew = std::chrono::steady_clock::now() + renew_interval_;
      }
    }
  }

  history::History &history_;
  std::chrono::milliseconds renew_interval_;
  std::chrono::milliseconds retry_interval_;
  mutable std::mutex mutex_;
  std::condition_variable cv_, done_cv_;
  std::vector<std::shared_ptr<Entry>> entries_;
  std::thread thread_;
  bool stopping_ = false;
};

const char *FinishName(ai::FinishReason reason)
{
  switch (reason)
  {
    case ai::FinishReason::Stop: return "stop";
    case ai::FinishReason::Length: return "length";
    case ai::FinishReason::Refusal: return "refusal";
    case ai::FinishReason::Other: return "other";
  }
  return "other";
}
using detail::Json;
Json Meta(history::ConversationId conversation, const history::Turn &turn)
{ return {{"conversation_id", std::to_string(conversation)}, {"user_message_id", std::to_string(turn.user_message.id)}, {"assistant_message_id", std::to_string(turn.assistant_message.id)}}; }
history::Status FinishUnstarted(history::History &history, history::UserId user,
                               history::ConversationId conversation, const history::Message &message,
                               history::MessageState state, const std::string &error)
{
  return history.Finish(user, conversation, message.id, message.generation_id,
                        {1, state, message.content, message.reasoning, "", error});
}

bool FinishIfCancelled(history::History &history, history::UserId user, history::ConversationId conversation,
                       const history::Message &message, const HttpResponse &response)
{
  if (!response.cancelToken()->load())
    return false;
  const auto saved = FinishUnstarted(history, user, conversation, message, history::MessageState::Interrupted, "");
  if (std::holds_alternative<history::Error>(saved))
    std::cerr << "Failed to save an interrupted turn\n";
  return true;
}

void SendHistoryError(const std::shared_ptr<HttpResponse> &response, bool stream, const history::Error &error)
{
  if (stream)
    response->SendEvent("error", Json{{"code", "history_unavailable"}, {"error", error.message}}.dump(), true);
  else
    detail::HistoryError(response, error);
}

void Generate(history::History &history, history::UserId user, history::ConversationId conversation,
              history::Turn turn, const ai::ModelClient *client, ai::ChatRequest request,
              bool stream, const std::shared_ptr<HttpResponse> &response,
              const std::shared_ptr<LeaseRenewer::Handle> &lease)
{
  if (response->cancelToken()->load())
  {
    lease->Stop();
    FinishIfCancelled(history, user, conversation, turn.assistant_message, *response);
    return;
  }
  auto model_cancel = lease->modelCancel();
  response->SetCancelHandler([model_cancel] { model_cancel->store(true); });
  history::Completion completion;
  const auto &assistant = turn.assistant_message;
  completion.state = history::MessageState::Failed;
  completion.content = assistant.content;
  completion.reasoning = assistant.reasoning;
  bool save_failed = false;
  const char *error_status = "502 Bad Gateway";
  auto last_save = std::chrono::steady_clock::now();
  std::size_t saved_size = 0;
  try
  {
    auto started = history.SaveProgress(user, conversation, assistant.id, assistant.generation_id,
                                        {++completion.sequence, completion.content, completion.reasoning});
    if (auto *error = std::get_if<history::Error>(&started))
    {
      lease->Stop();
      SendHistoryError(response, stream, *error);
      return;
    }
    lease->Touch();
    if (stream) response->SendEvent("meta", Meta(conversation, turn).dump());
    auto sink = [&](const std::string &text, bool reasoning) {
      (reasoning ? completion.reasoning : completion.content) += text;
      if (response->cancelToken()->load() || model_cancel->load()) return false;
      auto now = std::chrono::steady_clock::now();
      auto size = completion.content.size() + completion.reasoning.size();
      if (!saved_size || now - last_save >= std::chrono::milliseconds(250) || size - saved_size >= 32768)
      {
        auto saved = history.SaveProgress(user, conversation, assistant.id, assistant.generation_id,
            {++completion.sequence, completion.content, completion.reasoning});
        if (std::holds_alternative<history::Error>(saved)) { save_failed = true; return false; }
        lease->Touch();
        saved_size = size;
        last_save = now;
      }
      return !response->cancelToken()->load() && !model_cancel->load() && (!stream || response->SendEvent(reasoning ? "reasoning_delta" : "delta", Json{{"text", text}}.dump()));
    };
    ai::Result<ai::ChatResponse> result;
    if (stream)
      result = client->Stream(request, [&](const std::string &text) { return sink(text, false); }, model_cancel, [&](const std::string &text) { return sink(text, true); });
    else result = client->Generate(request, model_cancel);
    if (auto *reply = std::get_if<ai::ChatResponse>(&result))
    {
      if (!stream)
      {
        completion.content += reply->reply;
        completion.reasoning += reply->reasoning;
      }
      completion.finish_reason = FinishName(reply->finish_reason);
      completion.state = (response->cancelToken()->load() || model_cancel->load()) ? history::MessageState::Interrupted : history::MessageState::Completed;
      constexpr std::string_view refusal = "模型拒绝回答此问题。";
      if (completion.content.empty() && reply->finish_reason == ai::FinishReason::Refusal &&
          completion.reasoning.size() <= kMaxAssistantTextBytes - refusal.size())
        completion.content = refusal;
      if (completion.state == history::MessageState::Completed && completion.content.empty() && reply->finish_reason == ai::FinishReason::Length)
      {
        completion.state = history::MessageState::Failed;
        completion.error = "模型达到生成长度上限，未生成正文。";
      }
    }
    else
    {
      const auto &error = std::get<ai::Error>(result);
      if (error.finish_reason) completion.finish_reason = FinishName(*error.finish_reason);
      if (error.code == ai::ErrorCode::Timeout) error_status = "504 Gateway Timeout";
      if (save_failed) error_status = "503 Service Unavailable";
      completion.error = save_failed ? "保存回复失败，请刷新历史确认。" : error.message;
      completion.state = error.code == ai::ErrorCode::Cancelled ? history::MessageState::Interrupted : history::MessageState::Failed;
    }
  }
  catch (...)
  {
    completion.error = "处理回复失败，请重试。";
    completion.state = history::MessageState::Failed;
  }
  lease->Stop();
  if (auto lost = lease->LostError())
  {
    if (!response->cancelToken()->load())
      SendHistoryError(response, stream, *lost);
    std::cerr << "Generation lease lost before completion\n";
    return;
  }
  ++completion.sequence;
  auto saved = history.Finish(user, conversation, assistant.id, assistant.generation_id, completion);
  if (auto *error = std::get_if<history::Error>(&saved))
  {
    SendHistoryError(response, stream, *error);
    std::cerr << "Failed to save the final reply\n";
    return;
  }
  if (completion.state != history::MessageState::Completed)
  {
    auto message = completion.error.empty() ? "生成已中断，可重新生成。" : completion.error;
    if (stream) response->SendEvent("error", Json{{"code", "generation_failed"}, {"error", message}}.dump(), true);
    else detail::ReplyError(response, error_status, "generation_failed", message);
    return;
  }
  auto body = Meta(conversation, turn);
  body["finish_reason"] = completion.finish_reason;
  body["truncated"] = completion.finish_reason == "length";
  if (stream) response->SendEvent("done", body.dump(), true);
  else
  {
    body["reply"] = completion.content;
    body["reasoning"] = completion.reasoning;
    response->Reply("200 OK", body.dump());
  }
}

const ai::ModelClient *PrefixClient(const ai::ModelManager &models, const history::GenerationModel &model)
{
  const auto *client = models.Find(model.provider, model.model);
  return client && client->SupportsPrefixCompletion() ? client : nullptr;
}

Json ConversationView(const history::ConversationView &view, const ai::ModelManager &models)
{
  auto result = detail::ConversationView(view);
  if (!view.conversation.active_message_id && !view.messages.empty())
  {
    const auto &message = view.messages.back();
    if (message.CanContinue() && PrefixClient(models, message.generation_model))
      result["messages"].back()["continue_from"] = message.generation_id;
  }
  return result;
}
}
void RegisterChatRoutes(HttpRouter &router, auth::Auth &auth, history::History &history,
                        const ai::ModelManager &models, SubmitTask business_tasks, SubmitModelTask model_tasks,
                        const AuthHttpConfig &config)
{
  auto route = [&](const char *method, const char *path, AuthenticatedHandler handler) {
    router.Add(method, path, ApiHandler(auth, business_tasks, config, Access::Authenticated, std::move(handler)));
  };
  route("GET", "/api/models", [&models](const auto &, const auto &, const auto &response) {
    Json items = Json::array();
    for (const auto &model : models.List())
    {
      Json item{{"provider", model.provider}, {"model", model.model}};
      if (model.supports_reasoning_effort) item["reasoning_efforts"] = {"high", "max"};
      items.push_back(std::move(item));
    }
    response->Reply("200 OK", Json{{"models", items}}.dump());
  });
  route("POST", "/api/conversations", [&history](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"title"});
    auto conversation = detail::HistoryTake(history.Create(context.user_id, detail::String(body, "title")));
    response->Reply("201 Created", Json{{"conversation", detail::Conversation(conversation)}}.dump());
  });
  route("GET", "/api/conversations", [&history](const auto &context, const auto &request, const auto &response) {
    auto query = detail::Query(request, {"limit", "before_updated_at", "before_id"});
    std::optional<history::ConversationCursor> before;
    if (query.contains("before_updated_at") || query.contains("before_id"))
      before = history::ConversationCursor{history::TimePoint(std::chrono::milliseconds(detail::Id(query, "before_updated_at"))), detail::Id(query, "before_id")};
    auto page = detail::HistoryTake(history.List(context.user_id, query.contains("limit") ? detail::Id(query, "limit") : 50, before));
    Json items = Json::array();
    for (const auto &item : page.items) items.push_back(detail::Conversation(item));
    Json next = nullptr;
    if (page.next) next = {{"before_updated_at", std::to_string(detail::Millis(page.next->updated_at))}, {"before_id", std::to_string(page.next->id)}};
    response->Reply("200 OK", Json{{"items", items}, {"next", next}}.dump());
  });
  route("GET", "/api/conversation", [&history, &models](const auto &context, const auto &request, const auto &response) {
    auto query = detail::Query(request, {"id"});
    auto view = detail::HistoryTake(history.GetView(context.user_id, detail::Id(query, "id")));
    response->Reply("200 OK", ConversationView(view, models).dump());
  });
  route("POST", "/api/conversation/select", [&history, &models](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"conversation_id", "message_id"});
    auto view = detail::HistoryTake(history.SelectReply(context.user_id, detail::Id(body, "conversation_id"), detail::Id(body, "message_id")));
    response->Reply("200 OK", ConversationView(view, models).dump());
  });
  route("POST", "/api/conversation/rename", [&history](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"conversation_id", "title"});
    detail::HistoryTake(history.Rename(context.user_id, detail::Id(body, "conversation_id"), detail::String(body, "title")));
    response->Reply("200 OK", "{\"ok\":true}");
  });
  route("POST", "/api/conversation/delete", [&history](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"conversation_id"});
    detail::HistoryTake(history.Remove(context.user_id, detail::Id(body, "conversation_id")));
    response->Reply("200 OK", "{\"ok\":true}");
  });
  route("GET", "/api/conversation/messages", [&history](const auto &context, const auto &request, const auto &response) {
    auto query = detail::Query(request, {"conversation_id", "limit", "before"});
    auto page = detail::HistoryTake(history.ListMessages(context.user_id, detail::Id(query, "conversation_id"),
        query.contains("limit") ? detail::Id(query, "limit") : 200, query.contains("before") ? std::optional<history::MessageId>(detail::Id(query, "before")) : std::nullopt));
    Json items = Json::array();
    for (const auto &message : page.items) items.push_back(detail::Message(message));
    response->Reply("200 OK", Json{{"items", items}, {"next_before", page.next_before ? Json(std::to_string(*page.next_before)) : Json(nullptr)}}.dump());
  });
  route("GET", "/api/conversation/context", [&history](const auto &context, const auto &request, const auto &response) {
    auto query = detail::Query(request, {"conversation_id", "last_message_id"});
    auto messages = detail::HistoryTake(history.Context(context.user_id, detail::Id(query, "conversation_id"), detail::Id(query, "last_message_id")));
    Json items = Json::array();
    for (const auto &message : messages) items.push_back(detail::Message(message));
    response->Reply("200 OK", Json{{"items", items}}.dump());
  });
  auto leases = std::make_shared<LeaseRenewer>(history);
  route("POST", "/api/chat", [&history, &models, model_tasks, leases](const auto &context, const auto &http_request, const auto &response) {
    auto body = detail::Body(http_request, {"conversation_id", "request_id", "provider", "model", "stream",
                                           "content", "regenerate_message_id", "reasoning_effort",
                                           "continue_message_id", "continue_from"});
    const int actions = body.contains("content") + body.contains("regenerate_message_id") + body.contains("continue_message_id");
    const bool continuing = body.contains("continue_message_id");
    if (actions != 1 || continuing != body.contains("continue_from"))
      throw std::invalid_argument("chat action");
    auto stream = body.value("stream", true);
    auto conversation = detail::Id(body, "conversation_id");
    auto request_id = detail::String(body, "request_id");
    const ai::ModelClient *client = nullptr;
    ai::ChatRequest request;
    auto turn = [&] {
      if (continuing)
      {
        if (body.contains("provider") || body.contains("model") || body.contains("reasoning_effort"))
          throw std::invalid_argument("continuation model");
        return detail::HistoryTake(history.Continue(
            context.user_id, conversation, detail::Id(body, "continue_message_id"),
            detail::String(body, "continue_from"), request_id,
            [&](const history::GenerationModel &model) {
              client = PrefixClient(models, model);
              return client != nullptr;
            }));
      }
      history::GenerationModel model{detail::String(body, "provider"), detail::String(body, "model"), ""};
      client = models.Find(model.provider, model.model);
      if (!client) throw std::invalid_argument("model");
      if (body.contains("reasoning_effort"))
      {
        model.reasoning_effort = detail::String(body, "reasoning_effort");
        if (!client->SupportsReasoningEffort() || (model.reasoning_effort != "high" && model.reasoning_effort != "max"))
          throw std::invalid_argument("reasoning_effort");
      }
      else if (client->SupportsReasoningEffort())
        model.reasoning_effort = "high";
      return detail::HistoryTake(body.contains("content")
          ? history.BeginTurn(context.user_id, conversation, request_id, detail::String(body, "content"), model)
          : history.Regenerate(context.user_id, conversation, detail::Id(body, "regenerate_message_id"), request_id, model));
    }();
    if (!turn.created)
    {
      if (turn.assistant_message.state != history::MessageState::Completed)
      { detail::ReplyError(response, "409 Conflict", "conflict", "该请求已处理或正在生成，请刷新历史后重试。"); return; }
      auto meta = Meta(conversation, turn);
      if (continuing) meta["snapshot"] = true;
      auto reply = meta;
      reply["finish_reason"] = turn.assistant_message.finish_reason;
      reply["truncated"] = turn.assistant_message.finish_reason == "length";
      if (stream)
      {
        response->StartStream();
        response->SendEvent("meta", meta.dump());
        auto replay = [&](const char *event, const std::string &text) {
          for (std::size_t start = 0; start < text.size();)
          {
            auto end = std::min(start + 16384, text.size());
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
            if (!response->SendEvent(event, Json{{"text", text.substr(start, end - start)}}.dump())) return false;
            start = end;
          }
          return true;
        };
        if (!replay("delta", turn.assistant_message.content) || !replay("reasoning_delta", turn.assistant_message.reasoning))
        {
          response->SendEvent("error", "{\"code\":\"slow_client\",\"error\":\"回复接收过慢，请通过历史接口查看。\"}", true);
          return;
        }
        response->SendEvent("done", reply.dump(), true);
      }
      else
      {
        reply["reply"] = turn.assistant_message.content;
        reply["reasoning"] = turn.assistant_message.reasoning;
        response->Reply("200 OK", reply.dump());
      }
      return;
    }
    std::shared_ptr<LeaseRenewer::Handle> lease;
    try
    {
      if (FinishIfCancelled(history, context.user_id, conversation, turn.assistant_message, *response))
        return;
      const auto &assistant = turn.assistant_message;
      if (continuing) request.prefix = ai::AssistantPrefix{assistant.content, assistant.reasoning};
      const auto &effort = assistant.generation_model.reasoning_effort;
      if (!effort.empty())
        request.reasoning_effort = effort == "max" ? ai::ReasoningEffort::Max : ai::ReasoningEffort::High;
      auto messages = detail::HistoryTake(history.Context(context.user_id, conversation, turn.user_message.id));
      std::size_t bytes = 0;
      for (const auto &message : messages)
      {
        if (message.role == history::Role::Assistant && message.content.empty())
        { if (!request.messages.empty()) request.messages.pop_back(); continue; }
        bytes += message.content.size();
        if (bytes > 240 * 1024) throw history::Error{history::ErrorCode::InvalidRequest, "对话过长，请新建对话。"};
        request.messages.push_back({message.role == history::Role::User ? ai::Role::User : ai::Role::Assistant, message.content});
      }
      lease = LeaseRenewer::Track(leases, context.user_id, conversation, assistant, response->cancelToken());
      std::function<void()> start_stream;
      if (stream)
        start_stream = [response] { response->StartStream(); };
      if (!model_tasks([&history, user = context.user_id, conversation, turn, client, request, stream, response, lease] {
        detail::Safely(response, [&] {
          Generate(history, user, conversation, turn, client, request, stream, response, lease);
        });
      }, std::move(start_stream), response)) throw history::Error{history::ErrorCode::Unavailable, "模型任务队列已满，请稍后重试。"};
    }
    catch (...)
    {
      if (lease) lease->Stop();
      if (!FinishIfCancelled(history, context.user_id, conversation, turn.assistant_message, *response))
        FinishUnstarted(history, context.user_id, conversation, turn.assistant_message,
                        history::MessageState::Failed, "请求未开始生成，请重试。");
      throw;
    }
  });
}
} // namespace chat
