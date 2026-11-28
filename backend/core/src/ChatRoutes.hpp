#pragma once

#include "Api.hpp"

namespace ai { class ModelManager; }
namespace history { class History; }

namespace chat
{
// on_admitted（可为空）在任务获得队列名额后、worker 取任务前执行。
using SubmitModelTask = std::function<bool(std::function<void()> run, std::function<void()> on_admitted,
                                          const std::shared_ptr<HttpResponse> &response)>;

// 模型列表、聊天和历史接口先通过 Auth 鉴权，再把 user_id 交给业务。
// 认证/数据库任务与模型任务分别投递，开始 SSE 前处理认证及历史保存错误。
void RegisterChatRoutes(HttpRouter &router, auth::Auth &auth,
                        history::History &history, const ai::ModelManager &models,
                        SubmitTask business_tasks, SubmitModelTask model_tasks,
                        const AuthHttpConfig &config);
} // namespace chat
