# Backend 需求文档（从当前代码反推）

## 阅读范围与入口

面向三类调用者：使用 HTTP API 的网页或其他客户端、启动服务的部署者，以及调用后端 C++ 公共接口的模块。

| 入口 | 输入与输出 | 主要调用链 |
| --- | --- | --- |
| `backend/main.cc` 的 `main` | 启动参数、`config.json` → 服务进程或失败退出 | `LoadServerConfig` → `RunChatServer` → `HttpServer::Run` |
| `frontend/src/app.js` 的 `api` | 账号、会话、历史操作 → JSON | `HttpParser` → `HttpRouter` → `RegisterAuthRoutes` / `RegisterChatRoutes` |
| `frontend/src/app.js` 的 `requestReply` | 会话、请求标识、模型和问题 → SSE | `ApiHandler` → `History` → `ModelClient::Stream` → 历史保存与响应 |
| `POST /api/chat` 的 `stream:false` | 同一组聊天参数 → 完整 JSON 回复 | `ApiHandler` → `History` → `ModelClient::Generate` → 历史保存与响应 |

依据为 `backend/` 的业务代码、HTTP 与网络代码、数据库封装和 `schema.sql`，并沿入口查看前端调用与构建配置；未阅读现有测试。

来源标记：**明确写在代码里的**表示接口注释、校验条件、返回分支或 SQL 直接规定的行为；**从代码推测的**表示由多个函数的调用顺序或数据流组合推断的对外保证。文件路径均相对仓库根目录。

## 一、核心保证

### 账号与身份

### R1 注册有效且唯一的账号

- 保证：开放注册时，用户可以用去除首尾空白并转为小写的用户名创建账号，用户名须为以字母开头的 3–32 位字母、数字或下划线，重复用户名会被拒绝。
- 依据：`backend/auth/src/Auth.cc` 的 `Auth::Register`；`backend/auth/src/Support.hpp` 的 `Username`、`Lower`；`backend/db/schema.sql` 的 `users_username` 唯一约束；`backend/core/src/AuthRoutes.cc` 的 `RegisterAuthRoutes`（`POST /api/auth/register`）。
- 来源：明确写在代码里的

### R2 约束新密码长度并保护密码内容

- 保证：注册、修改和重置密码要求新密码符合配置的字符数范围（默认 8–128 个字符，另限 512 字节），账号保存与账号查询结果均不包含明文密码。
- 依据：`backend/auth/src/Auth.cc` 的 `ValidPassword`、`Auth::Register`、`Auth::ChangePassword`、`Auth::ResetPassword`；`backend/auth/src/Support.hpp` 的 `PasswordHash`、`ReadUser`；`backend/core/src/HttpSupport.hpp` 的 `User`。
- 来源：明确写在代码里的

### R3 注册时的邮箱需要另行验证

- 保证：注册时填写邮箱只会申请绑定验证码，返回的账号邮箱仍为空，邮件发送失败会通过 `email_delivery` 告知用户且不撤销已创建账号。
- 依据：`backend/auth/Auth.hpp` 的 `Auth::Register` 注释；`backend/auth/src/Auth.cc` 的 `Auth::Register`；`backend/core/src/AuthRoutes.cc` 的 `RegisterAuthRoutes`（注册分支）。
- 来源：明确写在代码里的

### R4 使用用户名或已绑定邮箱登录

- 保证：用户可用去除首尾空白并转为小写的用户名或已绑定邮箱及正确密码登录，账号不存在和密码错误得到相同的凭据错误提示。
- 依据：`backend/auth/src/Auth.cc` 的 `Auth::Login`、`RequirePassword`；`backend/auth/src/Support.hpp` 的 `Lower`；`backend/core/src/AuthRoutes.cc` 的 `RegisterAuthRoutes`（`POST /api/auth/login`）。
- 来源：明确写在代码里的

### R5 受保护 API 使用服务端验证的身份

- 保证：账号管理、模型列表、聊天和历史 API 只接受有效登录 Cookie 所代表的用户身份，缺失、失效或撤销的凭据不能进入相应业务处理。
- 依据：`backend/core/src/Api.cc` 的 `Token`、`ApiHandler`；`backend/core/src/AuthRoutes.cc` 的 `RegisterAuthRoutes`；`backend/core/src/ChatRoutes.cc` 的 `RegisterChatRoutes`；`backend/auth/src/Session.cc` 的 `Auth::Authenticate`。
- 来源：明确写在代码里的

### R6 会话按闲置时间到期并续期

- 保证：有效会话在认证时延长闲置有效期，并同步续期浏览器 Cookie；没有固定的累计登录期限。闲置到期后须重新登录，已撤销会话不能通过续期恢复。
- 依据：`backend/auth/Config.hpp` 的 `Config`；`backend/auth/Auth.hpp` 的 `Auth::Authenticate` 注释；`backend/auth/src/Session.cc` 的 `Auth::Impl::CreateSession`、`Auth::Authenticate`（默认闲置 24 小时）。
- 来源：明确写在代码里的

### R7 查询自己的账号信息

- 保证：登录用户可以查询自己的用户标识、用户名和已验证绑定的邮箱，未绑定邮箱返回空值。
- 依据：`backend/core/src/AuthRoutes.cc` 的 `RegisterAuthRoutes`（`GET /api/auth/me`）；`backend/auth/src/Auth.cc` 的 `Auth::Get`；`backend/core/src/HttpSupport.hpp` 的 `User`。
- 来源：明确写在代码里的

### R8 查看和撤销设备登录

- 保证：登录用户可以分页查看自己的有效设备会话及当前设备标记，并退出当前设备、指定的本人设备或全部设备，指定不存在或不属于自己的设备会话返回未找到。
- 依据：`backend/core/src/AuthRoutes.cc` 的 `RegisterAuthRoutes`（`/api/auth/sessions`、`/api/auth/sessions/revoke`、`/api/auth/logout`、`/api/auth/logout/all`）；`backend/auth/src/Session.cc` 的 `Auth::ListSessions`、`Auth::Logout`、`Auth::LogoutDevice`、`Auth::Impl::RevokeAll`。
- 来源：明确写在代码里的

### R9 修改密码成功后退出全部旧登录

- 保证：用户提供正确旧密码并成功修改密码后，全部旧会话和旧邮箱验证码失效，通知邮件投递失败不会撤销密码修改。
- 依据：`backend/auth/Auth.hpp` 的密码变更注释；`backend/auth/src/Auth.cc` 的 `Auth::ChangePassword`；`backend/core/src/AuthRoutes.cc` 的 `RegisterAuthRoutes`（`POST /api/auth/password`）。
- 来源：明确写在代码里的

### R10 邮箱管理需要近期密码确认

- 保证：申请绑定验证码、绑定或解绑邮箱前，用户须在当前会话确认当前密码，该授权默认有效 300 秒且不能跨会话使用或沿用到密码变更之后。
- 依据：`backend/auth/src/Auth.cc` 的 `Auth::VerifyPassword`、`Impl::VerificationKey`、`Impl::RequireVerification`、`Auth::SendEmailCode`、`Auth::BindEmail`、`Auth::UnbindEmail`；`backend/auth/Config.hpp` 的 `Config`。
- 来源：明确写在代码里的

### R11 验证后才能绑定或更换邮箱

- 保证：用户只能绑定通过本人相应用途验证码验证的目标邮箱，已被其他账号绑定的邮箱会被拒绝，绑定或解绑成功后此前的邮箱验证码失效。
- 依据：`backend/auth/src/Auth.cc` 的 `Auth::BindEmail`、`Auth::UnbindEmail`；`backend/auth/src/Codes.cc` 的 `Auth::Impl::ConsumeCode`、`Auth::Impl::InvalidateCodes`；`backend/auth/src/Support.hpp` 的 `EmailAddress`；`backend/db/schema.sql` 的 `users_email` 唯一约束。
- 来源：明确写在代码里的

### R12 验证码有有效期、冷却期和尝试上限

- 保证：邮箱验证码为六位数字且限定用户、邮箱和用途，默认有效 600 秒、同一用户同一用途申请间隔至少 60 秒，成功验证后立即失效，格式正确的错误验证码累计达到默认 5 次后失效。
- 依据：`backend/auth/Config.hpp` 的 `Config`；`backend/auth/src/Codes.cc` 的 `Auth::Impl::SendCode`、`Auth::Impl::ConsumeCode`。
- 来源：明确写在代码里的

### R13 找回申请不公开邮箱是否存在

- 保证：找回密码 HTTP 申请在参数通过入口校验且成功入队后统一返回 `202 Accepted`，是否存在绑定账号及后续邮件投递结果不改变这次公开响应。
- 依据：`backend/core/src/AuthRoutes.cc` 的 `RegisterAuthRoutes`（`POST /api/auth/password/forgot` 及其注释）；`backend/auth/src/Auth.cc` 的 `Auth::ForgotPassword`。
- 来源：明确写在代码里的

### R14 凭邮箱验证码重置密码

- 保证：用户可以凭已绑定邮箱和有效的重置验证码设置符合长度要求的新密码，成功后全部旧登录和旧邮箱验证码失效，通知邮件失败不撤销重置结果。
- 依据：`backend/auth/src/Auth.cc` 的 `Auth::ResetPassword`；`backend/core/src/AuthRoutes.cc` 的 `RegisterAuthRoutes`（`POST /api/auth/password/reset`）；`backend/auth/src/Codes.cc` 的 `Auth::Impl::ConsumeCode`。
- 来源：明确写在代码里的

### R15 限制高频账号操作

- 保证：超过配置窗口内的账号操作次数会被拒绝，其中登录默认限制为同一规范化标识每 300 秒 10 次及全局每 300 秒 1000 次，注册默认全局每 60 秒 20 次，其他设置了限流的敏感操作默认按用户或邮箱每 60 秒 20 次。
- 依据：`backend/auth/Config.hpp` 的 `Config`；`backend/auth/src/Auth.cc` 的 `Impl::Limit` 及各调用点；`backend/auth/src/Support.hpp` 的 `RateLimit`。
- 来源：明确写在代码里的

### R16 检查账号及聊天 API 的请求来源

- 保证：账号、模型、聊天和历史 API 拒绝显式携带未允许来源的请求，并要求非 GET 的业务请求携带 `X-Chat-Request: 1` 和以 `application/json` 开头的内容类型。
- 依据：`backend/core/src/HttpSupport.hpp` 的 `Prepare`；`backend/core/src/Api.cc` 的 `ApiHandler`、找回密码路由；`backend/core/src/ChatRoutes.cc` 的 `RegisterChatRoutes`。
- 来源：明确写在代码里的

### 对话与历史

### R17 隔离不同用户的历史

- 保证：用户只能读取和修改自己的会话及其中的消息，不存在的会话和他人的会话均返回未找到。
- 依据：`backend/history/History.hpp` 的归属校验注释；`backend/history/src/History.cc` 的 `Impl::Owner`、各公开读写方法；`backend/core/src/ChatRoutes.cc` 的 `RegisterChatRoutes`（使用认证所得 `user_id`）。
- 来源：明确写在代码里的

### R18 创建、重命名和删除对话

- 保证：用户可以创建、重命名和删除自己的对话，标题须为 1–256 字节，删除成功后该对话的消息和请求记录一并移除。
- 依据：`backend/history/src/History.cc` 的 `Title`、`History::Create`、`History::Rename`、`History::Remove`；`backend/db/schema.sql` 的 `messages_conversation`、`requests_conversation` 级联删除约束；`backend/core/src/ChatRoutes.cc` 的相应路由。
- 来源：明确写在代码里的

### R19 按最近更新分页查看会话

- 保证：会话列表按更新时间从新到旧、同时间按会话标识从大到小返回，默认每页 50 条且最多 100 条，有后续数据时提供由更新时间和会话标识组成的续页位置。
- 依据：`backend/history/src/History.cc` 的 `History::List`、`Limit`；`backend/core/src/ChatRoutes.cc` 的 `RegisterChatRoutes`（`GET /api/conversations`）。
- 来源：明确写在代码里的

### R20 分页读取全部消息与分支上下文

- 保证：用户可以按消息标识向前分页读取含各分支的消息记录（默认 200 条、最多 500 条、页内从旧到新），也可以按指定末条消息读取从根到该消息的上下文链，超过链长度上限或遇到循环时返回错误。
- 依据：`backend/history/src/History.cc` 的 `History::ListMessages`、`History::Context`；`backend/core/src/ChatRoutes.cc` 的 `RegisterChatRoutes`（`/api/conversation/messages`、`/api/conversation/context`，后者使用默认链上限 200）。
- 来源：明确写在代码里的

### R21 查看当前对话分支和生成状态

- 保证：用户查询单个对话时可以获得当前选中分支的消息链、当前生成中的消息标识及消息状态，当前分支超过 500 条消息时查询返回错误。
- 依据：`backend/core/src/ChatRoutes.cc` 的 `RegisterChatRoutes`（`GET /api/conversation`）；`backend/core/src/HttpSupport.hpp` 的 `Conversation`、`Message`；`backend/history/src/History.cc` 的 `History::Get`、`History::Context`。
- 来源：明确写在代码里的

### R22 校验聊天请求并先记录问题

- 保证：新聊天请求须指定已有会话、已配置模型和有效请求标识，并在非空且不全为空白的最多 80000 字节正文与重新生成目标之间二选一，通过校验的新轮次会先记录问题和待生成回答再调用模型。
- 依据：`backend/core/src/ChatRoutes.cc` 的 `RegisterChatRoutes`（`POST /api/chat`）；`backend/history/src/History.cc` 的 `Impl::Start`、`History::BeginTurn`、`History::Regenerate`（请求标识为 1–80 位字母、数字、下划线或连字符）。
- 来源：从代码推测的

### R23 重复聊天请求不重复生成

- 保证：同一会话内相同请求标识与相同正文或重新生成目标会复用原轮次，已完成时返回或重放已存回答，未完成时返回冲突，同一标识换成不同正文或目标也返回冲突。
- 依据：`backend/history/History.hpp` 的 `History::BeginTurn` 注释；`backend/history/src/History.cc` 的 `Impl::Start`；`backend/core/src/ChatRoutes.cc` 的 `RegisterChatRoutes`（`!turn.created` 分支）；`backend/db/schema.sql` 的 `turn_requests` 主键。
- 来源：明确写在代码里的

### R24 同一会话避免并行启动多个生成轮次

- 保证：会话已有未过期生成任务时拒绝新的不同请求，生成记录默认保留 5 分钟有效期并随成功的进度保存延长，查询单个对话或开始后续轮次时会将已过期任务标为中断。
- 依据：`backend/history/src/History.cc` 的 `lease_ms`、`Impl::Start`、`Impl::Update`、`Impl::Expire`、`History::Get`。
- 来源：明确写在代码里的

### R25 重新生成保留原回答

- 保证：用户可以为本会话中的已有用户问题创建新的回答分支，原回答保留且当前分支仅在新回答成功完成后切换到新回答，新回答失败或中断时保留原当前分支。
- 依据：`backend/history/History.hpp` 的 `History::Regenerate` 注释；`backend/history/src/History.cc` 的 `Impl::Start`（`regenerate` 分支）、`Impl::Update`（完成与失败分支）。
- 来源：明确写在代码里的

### R26 根据服务端历史构造模型上下文

- 保证：一次聊天的模型上下文来自该问题所在的服务端历史链且只携带角色和正文，链上限为 200 条、送出正文累计上限为 240 KiB，遇到空正文的助手记录时会跳过该记录并移除已加入的上一条消息。
- 依据：`backend/core/src/ChatRoutes.cc` 的 `RegisterChatRoutes`（构造 `ai::ChatRequest` 的循环）；`backend/history/src/History.cc` 的 `History::Context`。
- 来源：从代码推测的

### R27 有序保存进度并保护终态

- 保证：历史调用者可以提交累计回复快照和结束结果，旧序号的进度不覆盖较新进度，已结束回答拒绝后续进度和不同的结束内容，相同终态内容可重复确认。
- 依据：`backend/history/History.hpp` 的 `History::SaveProgress`、`History::Finish` 注释；`backend/history/src/History.cc` 的 `Impl::Update`、`History::Finish`。
- 来源：明确写在代码里的

### R28 回复保存有容量约束

- 保证：历史调用者保存的单条助手正文与思考文字合计不得超过 4 MiB，超过上限会返回错误且不接受该次快照或结束结果。
- 依据：`backend/history/src/History.cc` 的 `Impl::Update`；`backend/core/src/ChatRoutes.cc` 的 `Generate`（流式累计检查）。
- 来源：明确写在代码里的

### 模型选择与回复交付

### R29 只公布和调用已配置模型

- 保证：登录用户可以获取可选模型的供应方和模型名并按这两个值选择模型，未配置组合会被拒绝，模型列表不返回密钥、上游地址或完整配置。
- 依据：`backend/ai/ModelManager.hpp` 的 `ModelManager::List`、`ModelManager::Find` 注释；`backend/ai/src/ModelManager.cc` 的对应实现；`backend/core/src/ChatRoutes.cc` 的 `RegisterChatRoutes`（`GET /api/models`、`POST /api/chat`）。
- 来源：明确写在代码里的

### R30 生成参数由服务端配置决定

- 保证：聊天调用者只能选择服务端已配置的模型，不能通过请求覆盖上游地址、密钥、输出长度或采样参数，Chat Completions 与 Responses 的未配置生成参数交给上游决定而 Messages 的输出长度缺省为 4096。
- 依据：`backend/ai/ModelClient.hpp` 的生成参数注释；`backend/core/src/ChatRoutes.cc` 的聊天请求字段白名单；`backend/ai/src/ModelManager.cc` 的 `ModelManager::ModelManager`；`backend/ai/src/protocols/ChatCompletionsClient.cc`、`ResponsesClient.cc`、`MessagesClient.cc` 的各 `RequestBody`。
- 来源：从代码推测的

### R31 统一三种上游 API 的文字结果

- 保证：调用者可通过同一聊天入口使用配置为 `openai-completions`、`openai-responses` 或 `anthropic-messages` 的上游，并获得分开的正文、上游提供的思考文字或摘要及统一结束原因。
- 依据：`backend/ai/src/ModelManager.cc` 的 `ModelManager::ModelManager`；`backend/ai/src/protocols/ChatCompletionsClient.cc`、`ResponsesClient.cc`、`MessagesClient.cc` 的 `ParseBody`、`ParseEvent`；`backend/ai/src/ModelResponseState.cc` 的 `Reason`、`ModelResponseState::Append`。
- 来源：从代码推测的

### R32 支持流式或完整回复

- 保证：聊天默认通过 SSE 提供轮次标识、按接收顺序交付的正文与独立思考增量以及结束事件，指定 `stream:false` 时返回含相应标识、完整正文、思考文字和结束原因的 JSON。
- 依据：`backend/core/src/ChatRoutes.cc` 的 `RegisterChatRoutes`、`Meta`、`Generate`；`backend/http/src/HttpResponse.cc` 的 `HttpResponse::StartStream`、`HttpResponse::SendEvent`。
- 来源：明确写在代码里的

### R33 模型流必须具有正常终止标记

- 保证：模型客户端只在收到对应协议认可的终止状态后把流式调用视为成功，缺少终止状态、异常断流或返回非 SSE 内容时返回错误。
- 依据：`backend/ai/ModelClient.hpp` 的 `ModelClient::Stream` 注释；`backend/ai/src/ModelClient.cc` 的 `Write`、`ModelClient::Call`；三种协议客户端的 `ParseEvent`。
- 来源：明确写在代码里的

### R34 区分截断、拒绝和无文字回复

- 保证：正常返回的模型回答区分 `stop`、`length`、`refusal` 和 `other`，长度耗尽的回答标记为截断，空正文仅在拒绝回答时可被接受且历史及完整 JSON 回复会填入拒绝提示。
- 依据：`backend/ai/src/ModelResponseState.cc` 的 `Reason`；`backend/ai/src/ModelClient.cc` 的 `ModelClient::Call`（空回复检查）；`backend/core/src/ChatRoutes.cc` 的 `FinishName`、`Generate`。
- 来源：从代码推测的

### R35 成功响应之前确认最终历史保存

- 保证：聊天成功的完整 JSON 回复或 SSE `done` 事件只在最终回答已成功保存后发出，最终保存失败时返回历史错误。
- 依据：`backend/core/src/ChatRoutes.cc` 的 `Generate`（`History::Finish`、错误分支和成功响应的先后顺序）。
- 来源：从代码推测的

### R36 保存可用的流式中间结果并报告失败

- 保证：流式生成会在收到增量时按时间或累计大小保存进度，后续模型错误或取消时尝试将已收集内容保存为失败或中断，最终保存成功后通过 `error` 事件告知客户端而不发送 `done`。
- 依据：`backend/core/src/ChatRoutes.cc` 的 `Generate`（增量到达时距上次保存至少 250 毫秒或新增至少 32768 字节触发保存，随后调用 `History::Finish`）。
- 来源：从代码推测的

### R37 模型调用有超时和响应大小上限

- 保证：模型客户端按配置限制单次上游传输的总时长、连接等待及累计响应大小，默认分别为 60 秒、10 秒及 4 MiB，超限返回对应错误。
- 依据：`backend/ai/Types.hpp` 的 `ModelConfig`；`backend/ai/src/ModelManager.cc` 的 `ModelManager::ModelManager`；`backend/ai/src/ModelClient.cc` 的 `ModelClient::Call`、`Write`；`backend/ai/src/ModelResponseState.cc` 的 `ModelResponseState::Append`。
- 来源：明确写在代码里的

### R38 模型生成支持主动取消

- 保证：模型调用者可通过取消标记或让增量回调返回 `false` 请求停止生成，客户端会检查取消并返回取消结果，HTTP 连接对象销毁时也会设置该请求的取消标记。
- 依据：`backend/ai/Types.hpp` 的 `CancelToken`、`TextSink`；`backend/ai/src/ModelClient.cc` 的 `ModelClient::Call`、`Progress`、`Write`；`backend/ai/src/ModelResponseState.cc` 的 `ModelResponseState::Append`；`backend/http/src/Http.cc` 的 `HttpConnection::~HttpConnection`。
- 来源：明确写在代码里的

### 资源与失败边界

### R39 等待任务有容量限制

- 保证：账号及历史业务与模型生成分别限制并行处理数和排队数量，业务队列满时返回 `503 busy`，模型队列满时返回 `503 unavailable`。
- 依据：`backend/core/Chat.cc` 的 `ChatWorkers::Submit`、`RunChatServer`；`backend/core/src/Api.cc` 的 `ApiHandler`；`backend/core/src/ChatRoutes.cc` 的模型任务投递分支；`backend/ServerConfig.hpp` 的默认值（业务线程 4、队列 64，模型线程 2、队列 8）。
- 来源：明确写在代码里的

### R40 存储调用提供有界等待和事务结果

- 保证：存储调用者可并发借用独占连接并配置借用及网络等待时间，未提交的健康 MySQL 事务在连接归还时回滚，写入或提交结果无法确认时会返回结果未知而不自动重放该操作。
- 依据：`backend/db/MySql.hpp`、`backend/db/Redis.hpp` 的连接接口注释；`backend/db/src/Pool.hpp` 的 `Pool::Acquire`；`backend/db/src/MySql.cc` 的 `MySql::Acquire`、`Connection::Impl::~Impl`、`Connection::Execute`、`Connection::Commit`；`backend/db/src/Redis.cc` 的 `Redis::Acquire`、`Connection::Command`。
- 来源：明确写在代码里的

### R41 对外错误不透传底层诊断

- 保证：账号、历史和模型调用失败时向 HTTP 调用者提供分类错误码和可展示的说明，数据库原始诊断和模型上游原始错误体不会直接作为公开错误响应返回。
- 依据：`backend/auth/src/Support.hpp` 的 `Guard`；`backend/history/src/History.cc` 的 `Guard`；`backend/ai/src/ModelClient.cc` 的 `ModelClient::Call`；`backend/core/src/HttpSupport.hpp` 的 `AuthError`、`HistoryError`、`Safely`。
- 来源：从代码推测的

### R42 拒绝不支持或过大的 HTTP 请求

- 保证：HTTP 入口只处理普通 HTTP/1.1 请求，拒绝协议升级和 `Transfer-Encoding` 请求体，POST 必须带 `Content-Length`，请求头最多 16388 字节、请求体最多 256 KiB，格式错误或输入不完整时返回相应错误。
- 依据：`backend/http/src/HttpParser.hpp` 的 `kMaxHeader`、`kMaxBody`；`backend/http/src/HttpParser.cc` 的 `HttpParser::Feed`、`Finish`、`OnHeaderComplete`、`OnHeadersComplete`、`OnBody`；`backend/http/src/Http.cc` 的 `HttpConnection::HandleBusiness`。
- 来源：明确写在代码里的

### R43 启动时验证配置和依赖

- 保证：部署者以无额外参数方式启动服务时，配置缺失或无效、模型列表不满足 1–128 个有效且不重复的模型配置、MySQL 或 Redis 无法初始连接、必要表无法查询或监听初始化失败都会使启动失败。
- 依据：`backend/main.cc` 的 `main`；`backend/ServerConfig.cc` 的 `LoadServerConfig` 及各加载函数；`backend/ai/src/ModelManager.cc` 的 `ModelManager::ModelManager`；`backend/core/Chat.cc` 的 `RunChatServer`；`backend/db/src/MySql.cc` 的 `MySql::Impl::Impl`；`backend/db/src/Redis.cc` 的 `Redis::Impl::Impl`；`backend/http/src/Http.cc` 的 `HttpServer::Run`。
- 来源：从代码推测的

## 二、次要细节

### R44 API 参数的表达形式

- 保证：账号及聊天业务请求体须为仅含各路由允许字段的 JSON 对象，历史会话与消息 ID 及其分页大小、时间查询参数使用非零十进制字符串，查询参数重复、未知或缺少等号时返回参数错误。
- 依据：`backend/core/src/HttpSupport.hpp` 的 `Body`、`String`、`Number`、`Id`、`Query`；`backend/core/src/AuthRoutes.cc`、`backend/core/src/ChatRoutes.cc` 中各业务路由调用点（设备列表的 `cursor` 另按 `Auth::ListSessions` 校验并允许 `"0"`）。
- 来源：明确写在代码里的

### R45 错误状态与载荷

- 保证：账号和历史 API 的常规业务错误使用 `{code,error}` JSON，参数错误对应 400、未登录或凭据错误对应 401、权限或注册开关限制对应 403、未找到对应 404、冲突对应 409、限流对应 429、内部错误对应 500、不可用或结果未知对应 503、超时对应 504。
- 依据：`backend/core/src/HttpSupport.hpp` 的 `ReplyError`、`AuthError`、`HistoryError`、`Safely`；`backend/core/src/Api.cc` 的 `ApiHandler`。
- 来源：明确写在代码里的

### R46 模型错误的 HTTP 表达

- 保证：非流式聊天在最终失败状态保存成功后，将模型调用超时报告为 HTTP 504、进度保存失败报告为 503、其他生成失败通常报告为 502，已经开始的 SSE 则通过 `error` 事件表达失败。
- 依据：`backend/core/src/ChatRoutes.cc` 的 `Generate`；`backend/http/src/HttpResponse.cc` 的 `HttpResponse::Reply`。
- 来源：明确写在代码里的

### R47 Cookie 与会话列表字段

- 保证：登录 Cookie 使用可配置名称并带 `Path=/`、`HttpOnly`、`SameSite=Lax`、`Max-Age` 及默认开启的 `Secure`，退出或密码变更成功时清除当前 Cookie，设备列表返回设备标识、用户代理、毫秒时间戳和当前设备标记。
- 依据：`backend/core/src/HttpSupport.hpp` 的 `Cookie`；`backend/core/src/AuthRoutes.cc` 的 `RegisterAuthRoutes`；`backend/auth/src/Session.cc` 的 `UserAgent`、`Auth::ListSessions`；`backend/core/src/Api.hpp` 的 `AuthHttpConfig`。
- 来源：明确写在代码里的

### R48 历史响应字段格式

- 保证：会话及消息标识以字符串表示、无父消息或当前消息时用 `null`，时间以 Unix 毫秒表示，消息同时返回状态、正文、思考文字、结束原因、错误和截断标记。
- 依据：`backend/core/src/HttpSupport.hpp` 的 `Millis`、`Conversation`、`Message`；`backend/core/src/ChatRoutes.cc` 的 `Meta`、`RegisterChatRoutes`。
- 来源：明确写在代码里的

### R49 允许来源的跨域响应

- 保证：账号与聊天 API 对允许的显式来源回显该来源并允许携带凭据，OPTIONS 预检返回 204 并声明允许 GET、POST、OPTIONS 及 `Content-Type`、`X-Chat-Request` 请求头。
- 依据：`backend/core/src/HttpSupport.hpp` 的 `Prepare`；`backend/core/Chat.cc` 的 `RunChatServer`（OPTIONS 回退处理）。
- 来源：明确写在代码里的

### R50 单次连接和 SSE 格式

- 保证：每个 HTTP 连接只处理一个请求且响应声明关闭连接，SSE 使用 chunked 编码、命名事件和单行 JSON 数据，并在流未结束时周期性发送注释心跳。
- 依据：`backend/http/src/HttpParser.cc` 的 `HttpParser::OnMessageComplete`；`backend/http/src/HttpResponse.cc` 的 `HttpResponse::Reply`、`StartStream`、`SendEvent`、`Heartbeat`；`backend/http/src/Http.cc` 的 `HttpConnection::Tick`（约每秒触发）。
- 来源：明确写在代码里的

### R51 上游 SSE 分片兼容

- 保证：上游 SSE 输入可以在任意字节处拆分并使用 LF、CRLF 或 CR 换行、起始 BOM 及多行 data，解析时忽略注释和未使用字段，输入结束时丢弃未以空行结束的事件。
- 依据：`backend/ai/src/SseParser.hpp` 的接口约定；`backend/ai/src/SseParser.cc` 的 `SseParser::Feed`、`ProcessLine`、`DispatchEvent`、`Finish`。
- 来源：明确写在代码里的

### R52 静态资源和辅助接口

- 保证：匿名客户端可以获取白名单内的前端静态文件及受文件名约束的 KaTeX 字体，并使用 `GET /api/hello` 获取问候、`POST /api/echo` 原样返回正文，其他 GET 路径返回 404、其他未匹配方法返回 405。
- 依据：`backend/core/Chat.cc` 的 `StaticFile`、`RunChatServer`（辅助路由和回退处理）；`backend/http/src/HttpRouter.cc` 的 `HttpRouter::operator()`。
- 来源：明确写在代码里的

### R53 启动配置的默认值和校验

- 保证：部署者可省略支持默认值的配置项（默认监听 `0.0.0.0:53411`），但重复 JSON 键、根对象及非模型配置节中的未知字段、错误类型和越界数值会使配置加载失败。
- 依据：`backend/ServerConfig.hpp` 的 `ServerConfig`；`backend/ServerConfig.cc` 的 `LoadServerConfig`、`CheckFields`、`ReadNumber`、`ReadString`、`ReadBool` 及各节加载函数。
- 来源：明确写在代码里的

### R54 邮件传输选项

- 保证：部署者可以为邮件配置 TLS 或 STARTTLS、认证信息、发件地址、自定义 CA 和超时，未配置邮件主机时相关发信操作返回不可用，默认单次邮件传输超时为 10 秒。
- 依据：`backend/auth/Config.hpp` 的 `SmtpConfig`；`backend/ServerConfig.cc` 的 `LoadEmail`；`backend/auth/src/Mailer.cc` 的 `Mailer::Mailer`、`Mailer::Send`；`backend/auth/src/Codes.cc` 的 `Auth::Impl::SendCode`。
- 来源：明确写在代码里的

### R55 存储封装的返回边界

- 保证：MySQL 调用者能区分空值与文本值并获得列名和写入结果，查询封装拒绝超过 2000 行或累计字段内容超过 16 MiB 的结果，Redis 命令须包含 1–1024 项参数且可逐项传递二进制数据。
- 依据：`backend/db/Types.hpp` 的 `Rows`、`WriteResult`、`RedisReply`；`backend/db/src/MySql.cc` 的 `MySql::Connection::Query`、`Execute`；`backend/db/Redis.hpp` 的 `Connection::Command` 注释；`backend/db/src/Redis.cc` 的 `Redis::Connection::Command`。
- 来源：明确写在代码里的

### R56 启动诊断与日志开关

- 保证：部署者能从终端看到启动异常、服务地址、模型数和实际 HTTP IO 线程数，网络调试日志由构建时日志开关控制，当前默认构建关闭这些网络日志。
- 依据：`backend/main.cc` 的 `main`；`backend/core/Chat.cc` 的 `RunChatServer`；`backend/http/src/Http.cc` 的 `HttpServer::Run`；`backend/net/src/common/Logging.hpp` 的日志宏；`CMakeLists.txt` 的 `CHAT_LOGGING` 设置。
- 来源：明确写在代码里的

## 三、疑似缺失

以下仅列待确认的问题。

### Q1 请求是否需要覆盖排队阶段的总截止时间？

模型超时从实际上游调用开始；业务队列和模型队列有容量限制，但未找到排队任务的独立截止时间。是否需要从 HTTP 收到请求起计算总耗时，并取消超时排队任务？

定位：`backend/core/Chat.cc` 的 `ChatWorkers::Submit`、`Work`；`backend/core/src/ChatRoutes.cc` 的模型任务投递；`backend/ai/src/ModelClient.cc` 的 `ModelClient::Call`。

### Q2 是否需要模型失败重试或供应方切换？

模型上游请求只执行一次；未找到针对临时断网、429 或 5xx 的退避重试或备用模型选择。哪些失败可重试，已向客户端发送增量后应如何处理？

定位：`backend/ai/src/ModelClient.cc` 的 `ModelClient::Call`；`backend/core/src/ChatRoutes.cc` 的 `Generate`。

### Q3 是否需要专门的请求头和请求体接收期限？

网络空闲检查以最近收到数据为依据；未找到独立的请求头完成期限、请求体完成期限或最低接收速率约束。持续缓慢输入是否也需要在固定时间内结束？

定位：`backend/http/src/Http.cc` 的 `HttpConnection::HandleBusiness`；`backend/net/src/reactor/TcpConnection.hpp`、`backend/net/src/proactor/TcpConnection.hpp` 的接收逻辑与 `HandleIdleTimeout`。

### Q4 撤销登录后，已开始的聊天是否也要停止？

会话只在进入受保护处理函数前认证，未找到生成期间再次认证或撤销登录时主动取消该会话生成任务的机制。退出全部设备或改密是否应同时终止已有流？

定位：`backend/core/src/Api.cc` 的 `ApiHandler`；`backend/auth/src/Session.cc` 的 `Auth::Impl::RevokeAll`；`backend/core/src/ChatRoutes.cc` 的 `Generate`。

### Q5 崩溃后是否需要主动恢复生成状态？

进度保存依赖增量到达，过期清理依赖后续特定访问；未找到重启扫描、后台清理、继续生成或恢复投递。进程崩溃后需要保留到什么粒度、何时向用户确认中断？

定位：`backend/core/src/ChatRoutes.cc` 的 `Generate`；`backend/history/src/History.cc` 的 `Impl::Expire`、`History::Get`、`Impl::Start`。

### Q6 是否需要每用户的资源与费用配额？

账号操作有限流、任务队列有全局容量、单条内容有上限；未找到每用户的聊天频率、并行会话数、累计 token、存储总量或邮件目标地址配额。是否需要这些限制以及公平排队？

定位：`backend/auth/src/Auth.cc` 的 `Impl::Limit`；`backend/auth/src/Codes.cc` 的 `Auth::Impl::SendCode`；`backend/core/Chat.cc` 的 `ChatWorkers`；`backend/history/src/History.cc` 的创建和写入方法。

### Q7 浏览器到服务端的加密传输由谁保证？

HTTP 服务使用普通 TCP，Cookie 默认带 `Secure`；未找到后端 HTTP 入口的 TLS 握手或证书配置。是否要求由外部代理提供 HTTPS，以及后端需要怎样确认代理传来的请求来源？

定位：`backend/http/src/Http.cc` 的 `HttpServer::Impl`；`backend/net/src/common/Socket.hpp` 的 `Socket::CreateListen`；`backend/core/src/HttpSupport.hpp` 的 `Prepare`；`backend/core/src/HttpSupport.hpp` 的 `Cookie`。

### Q8 多接口读取历史是否需要一致快照？

获取会话、沿父链读取上下文和分页读取消息没有共用一个读取快照，未找到客户端可提交的历史版本号。并发删除、重命名或切换当前分支时，客户端是否应获得同一版本的会话和消息？

定位：`backend/core/src/ChatRoutes.cc` 的 `GET /api/conversation` 处理函数；`backend/history/src/History.cc` 的 `History::Get`、`Context`、`ListMessages`、`Rename`、`Remove`。

## 四、可疑行为

### S1 请求幂等性没有包含模型选择

- 观察：请求摘要只包含正文或重新生成目标，不包含 `provider`、`model`；同一 `request_id` 换模型但不换正文，会返回此前轮次的结果。
- 可疑点：客户端可能把模型选择也视为同一次请求内容的一部分，但服务端不会报“请求内容冲突”。
- 定位：`backend/history/src/History.cc` 的 `Impl::Start`（`request_hash`）；`backend/core/src/ChatRoutes.cc` 的 `RegisterChatRoutes`（模型查找及重复请求分支）。

### S2 部分失败路径会留下生成中占位

- 观察：首次 `SaveProgress` 失败时 `Generate` 直接返回；入模型队列前出错时虽调用 `Finish`，却忽略其返回结果；停止 worker 时直接丢弃排队任务。
- 可疑点：问题及助手占位已经提交，以上路径可能没有写入终态，后续要等有效期到达并触发清理才能解除占用。
- 定位：`backend/core/src/ChatRoutes.cc` 的 `Generate` 首次保存分支及 `RegisterChatRoutes` 末尾 `catch`；`backend/core/Chat.cc` 的 `ChatWorkers::Stop`；`backend/history/src/History.cc` 的 `Impl::Start`、`Impl::Expire`。

### S3 连续输出的长回复仍可能触发网络空闲关闭

- 观察：两套网络连接实现只在收到字节时更新 `last_active_`，成功发送回复和 SSE 心跳均不更新该值。
- 可疑点：客户端提交问题后通常只接收数据，即使回复仍在持续输出，也可能在空闲检查时被关闭；当前默认构建的阈值为 60 秒，检查重排还可能推迟实际关闭时间。
- 定位：`backend/net/src/reactor/TcpConnection.hpp` 的 `HandleRead`、`FlushOut`、`HandleIdleTimeout`；`backend/net/src/proactor/TcpConnection.hpp` 的 `Receive`、`Send`、`HandleIdleTimeout`；`backend/net/src/common/Runtime.hpp` 的 `kIdleTimeout`。

### S4 模型超时可以长于历史生成有效期

- 观察：模型 `timeout_ms` 可配置超过 5 分钟，历史生成有效期固定为 5 分钟；非流式调用在等待完整回复期间没有中间进度续期，流式长时间无增量时也不会续期。
- 可疑点：上游在自身时限内成功完成，最终结果仍可能因历史生成已过期而保存失败。
- 定位：`backend/ai/src/ModelManager.cc` 的 `ModelManager::ModelManager`；`backend/core/src/ChatRoutes.cc` 的 `Generate`；`backend/history/src/History.cc` 的 `lease_ms`、`Impl::Update`。

### S5 列表可能长期显示已经过期的生成状态

- 观察：过期清理只由单个会话查询或开始轮次调用，列会话、列消息和取上下文均不调用 `Expire`。
- 可疑点：只访问列表的用户仍可能看到早已过期的 `active_message_id` 或 `generating` 状态。
- 定位：`backend/history/src/History.cc` 的 `Impl::Expire`、`History::Get`、`Impl::Start`、`History::List`、`History::ListMessages`、`History::Context`。

### S6 账号变更失败仍可能已经撤销登录或消耗验证码

- 观察：改密、重置和邮箱变更会先撤销会话、消费或作废验证码，再提交账号数据变更；这些先行操作没有随数据库回滚恢复。
- 可疑点：调用返回失败不等于用户仍可使用原登录和验证码，同一操作也可能无法直接重试。
- 定位：`backend/auth/src/Auth.cc` 的 `Auth::ChangePassword`、`ResetPassword`、`BindEmail`、`UnbindEmail`；`backend/auth/src/Codes.cc` 的 `Auth::Impl::ConsumeCode`。

### S7 首次流式拒绝提示与存储结果不一致

- 观察：模型以拒绝结束且未给正文时，`Generate` 会补出“模型拒绝回答此问题。”并保存，但不会为这段补文发送 `delta`；同一请求的 SSE 重放会发送该正文。
- 可疑点：首次流式调用与重放、非流式调用收到的可见正文不同，客户端需自行补文才能与历史一致。
- 定位：`backend/core/src/ChatRoutes.cc` 的 `Generate`（拒绝补文与 `done` 分支）、`RegisterChatRoutes`（重复请求重放）；`frontend/src/app.js` 的 `requestReply`（`done` 分支补文）。

### S8 Messages 非流式拒绝标记会被后续赋值覆盖

- 观察：`MessagesClient::ParseBody` 遇到 `refusal` 内容块时先设置拒绝状态，循环结束后又无条件用 `Reason(stop_reason)` 覆盖它。
- 可疑点：当内容块与停止原因传达不同信息时，回复可能保留拒绝文字却被标为普通结束。
- 定位：`backend/ai/src/protocols/MessagesClient.cc` 的 `MessagesClient::ParseBody`；`backend/ai/src/ModelResponseState.cc` 的 `Reason`。

### S9 响应队列的大小上限不适用于最终响应

- 观察：`HttpResponse::Push` 只对 `final == false` 的输出执行 1 MiB 队列限制，普通 JSON 响应及最终 SSE 事件均可绕过该限制。
- 可疑点：大历史查询或完整回复可能一次排入远超 1 MiB 的内容，不能把该上限理解成每连接输出内存总上限。
- 定位：`backend/http/src/HttpResponse.cc` 的 `HttpResponse::Push`、`Reply`、`SendEvent`；`backend/core/src/ChatRoutes.cc` 的历史查询路由。

### S10 不同入口的重复键和未知字段校验不一致

- 观察：普通 API JSON 解析没有重复键检查，重复键保留解析后的值；服务启动检查重复 JSON 键，但模型条目没有其他配置节使用的未知字段检查。
- 可疑点：请求体歧义和模型配置拼写错误可能被静默接受，校验行为在不同入口间不一致。
- 定位：`backend/core/src/HttpSupport.hpp` 的 `Body`；`backend/ServerConfig.cc` 的 `LoadServerConfig`、`CheckFields`；`backend/ai/src/ModelManager.cc` 的 `ModelManager::ModelManager`。

### S11 两次读取配置且模型路径固定

- 观察：`LoadServerConfig` 接受文件路径，但 `RunChatServer` 又固定用 `config.json` 构造模型管理器。
- 可疑点：C++ 调用者指定其他配置文件时，模型配置仍来自工作目录中的 `config.json`，两次读取之间文件变化也可能使本次启动使用两份不同配置。
- 定位：`backend/ServerConfig.hpp`、`backend/ServerConfig.cc` 的 `LoadServerConfig`；`backend/core/Chat.cc` 的 `RunChatServer`。

### S12 限流提示的等待时间固定为 60 秒

- 观察：所有账号限流错误都带 `Retry-After: 60`，实际登录窗口默认 300 秒，验证码冷却和其他窗口也可另行配置。
- 可疑点：客户端按提示等待后仍可能继续被限流，响应头没有表达实际剩余等待时间。
- 定位：`backend/core/src/HttpSupport.hpp` 的 `AuthError`；`backend/auth/Config.hpp` 的 `Config`；`backend/auth/Config.hpp` 的 `Config`。

### S13 SSE 响应重复给出缓存策略

- 观察：`Prepare` 先加入 `Cache-Control: no-store`，`StartStream` 再直接写入 `Cache-Control: no-cache`，同一 SSE 响应因此具有两条该字段。
- 可疑点：缓存策略的表达没有统一出口，需要确认是否有意同时发送这两个值。
- 定位：`backend/core/src/HttpSupport.hpp` 的 `Prepare`；`backend/http/src/HttpResponse.cc` 的 `HttpResponse::StartStream`。
