#include "ChatRoutes.hpp"
#include "HttpSupport.hpp"
#include "ai/ModelManager.hpp"
#include "history/History.hpp"
#include "http/HttpRouter.hpp"
#include <iostream>

namespace chat
{
namespace
{
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
bool FinishIfCancelled(history::History &history, history::UserId user, history::ConversationId conversation,
                       history::MessageId message, const HttpResponse &response)
{
  if (!response.cancelToken()->load())
    return false;
  const auto saved = history.Finish(user, conversation, message, {1, history::MessageState::Interrupted, "", "", "", ""});
  if (std::holds_alternative<history::Error>(saved))
    std::cerr << "Failed to save an interrupted turn\n";
  return true;
}

void Generate(history::History &history, history::UserId user, history::ConversationId conversation,
              history::Turn turn, const ai::ModelClient *client, ai::ChatRequest request,
              bool stream, const std::shared_ptr<HttpResponse> &response)
{
  if (FinishIfCancelled(history, user, conversation, turn.assistant_message.id, *response))
    return;
  history::Completion completion;
  completion.state = history::MessageState::Failed;
  bool save_failed = false;
  const char *error_status = "502 Bad Gateway";
  auto last_save = std::chrono::steady_clock::now();
  std::size_t saved_size = 0;
  try
  {
    auto started = history.SaveProgress(user, conversation, turn.assistant_message.id, {++completion.sequence, "", ""});
    if (auto *error = std::get_if<history::Error>(&started))
    {
      if (stream) response->SendEvent("error", Json{{"code", "history_unavailable"}, {"error", error->message}}.dump(), true);
      else detail::HistoryError(response, *error);
      return;
    }
    if (stream) response->SendEvent("meta", Meta(conversation, turn).dump());
    auto sink = [&](const std::string &text, bool reasoning) {
      (reasoning ? completion.reasoning : completion.content) += text;
      if (response->cancelToken()->load()) return false;
      auto now = std::chrono::steady_clock::now();
      auto size = completion.content.size() + completion.reasoning.size();
      if (size > 4 * 1024 * 1024) return false;
      if (now - last_save >= std::chrono::milliseconds(250) || size - saved_size >= 32768)
      {
        auto saved = history.SaveProgress(user, conversation, turn.assistant_message.id,
            {++completion.sequence, completion.content, completion.reasoning});
        if (std::holds_alternative<history::Error>(saved)) { save_failed = true; return false; }
        saved_size = size;
        last_save = now;
      }
      return !response->cancelToken()->load() && (!stream || response->SendEvent(reasoning ? "reasoning_delta" : "delta", Json{{"text", text}}.dump()));
    };
    ai::Result<ai::ChatResponse> result;
    if (stream)
      result = client->Stream(request, [&](const std::string &text) { return sink(text, false); }, response->cancelToken(), [&](const std::string &text) { return sink(text, true); });
    else result = client->Generate(request, response->cancelToken());
    if (auto *reply = std::get_if<ai::ChatResponse>(&result))
    {
      completion.content = reply->reply;
      completion.reasoning = reply->reasoning;
      completion.finish_reason = FinishName(reply->finish_reason);
      completion.state = response->cancelToken()->load() ? history::MessageState::Interrupted : history::MessageState::Completed;
      if (completion.content.empty() && reply->finish_reason == ai::FinishReason::Refusal) completion.content = "模型拒绝回答此问题。";
    }
    else
    {
      auto &error = std::get<ai::Error>(result);
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
  ++completion.sequence;
  auto saved = history.Finish(user, conversation, turn.assistant_message.id, completion);
  if (auto *error = std::get_if<history::Error>(&saved))
  {
    if (stream) response->SendEvent("error", Json{{"code", "history_unavailable"}, {"error", error->message}}.dump(), true);
    else detail::HistoryError(response, *error);
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
  route("GET", "/api/conversation", [&history](const auto &context, const auto &request, const auto &response) {
    auto query = detail::Query(request, {"id"});
    auto view = detail::HistoryTake(history.GetView(context.user_id, detail::Id(query, "id")));
    response->Reply("200 OK", detail::ConversationView(view).dump());
  });
  route("POST", "/api/conversation/select", [&history](const auto &context, const auto &request, const auto &response) {
    auto body = detail::Body(request, {"conversation_id", "message_id"});
    auto view = detail::HistoryTake(history.SelectReply(context.user_id, detail::Id(body, "conversation_id"), detail::Id(body, "message_id")));
    response->Reply("200 OK", detail::ConversationView(view).dump());
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
  route("POST", "/api/chat", [&history, &models, model_tasks](const auto &context, const auto &http_request, const auto &response) {
    auto body = detail::Body(http_request, {"conversation_id", "request_id", "provider", "model", "stream", "content", "regenerate_message_id", "reasoning_effort"});
    if (body.contains("content") == body.contains("regenerate_message_id")) throw std::invalid_argument("content");
    auto client = models.Find(detail::String(body, "provider"), detail::String(body, "model"));
    if (!client) throw std::invalid_argument("model");
    ai::ChatRequest request;
    if (body.contains("reasoning_effort"))
    {
      const auto effort = detail::String(body, "reasoning_effort");
      if (!client->SupportsReasoningEffort() || (effort != "high" && effort != "max"))
        throw std::invalid_argument("reasoning_effort");
      request.reasoning_effort = effort == "max" ? ai::ReasoningEffort::Max : ai::ReasoningEffort::High;
    }
    auto stream = body.value("stream", true);
    auto conversation = detail::Id(body, "conversation_id");
    auto request_id = detail::String(body, "request_id");
    auto turn = detail::HistoryTake(body.contains("content")
        ? history.BeginTurn(context.user_id, conversation, request_id, detail::String(body, "content"))
        : history.Regenerate(context.user_id, conversation, detail::Id(body, "regenerate_message_id"), request_id));
    if (!turn.created)
    {
      if (turn.assistant_message.state != history::MessageState::Completed)
      { detail::ReplyError(response, "409 Conflict", "conflict", "该请求已处理或正在生成，请刷新历史后重试。"); return; }
      auto reply = Meta(conversation, turn);
      reply["finish_reason"] = turn.assistant_message.finish_reason;
      reply["truncated"] = turn.assistant_message.finish_reason == "length";
      if (stream)
      {
        response->StartStream();
        response->SendEvent("meta", Meta(conversation, turn).dump());
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
    try
    {
      if (FinishIfCancelled(history, context.user_id, conversation, turn.assistant_message.id, *response))
        return;
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
      std::function<void()> start_stream;
      if (stream)
        start_stream = [response] { response->StartStream(); };
      if (!model_tasks([&history, user = context.user_id, conversation, turn, client, request, stream, response] {
        detail::Safely(response, [&] {
          Generate(history, user, conversation, turn, client, request, stream, response);
        });
      }, std::move(start_stream), response)) throw history::Error{history::ErrorCode::Unavailable, "模型任务队列已满，请稍后重试。"};
    }
    catch (...)
    {
      if (!FinishIfCancelled(history, context.user_id, conversation, turn.assistant_message.id, *response))
        history.Finish(context.user_id, conversation, turn.assistant_message.id, {1, history::MessageState::Failed, "", "", "", "请求未开始生成，请重试。"});
      throw;
    }
  });
}
} // namespace chat
