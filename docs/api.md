# 账号与聊天接口

所有接口使用 JSON；ID 以十进制字符串表示。错误为 `{ "code": "稳定错误码", "error": "说明" }`。
缺少登录为 401，参数错误为 400，来源或 CSRF 校验失败为 403，归属不符与不存在统一为 404，并发冲突为 409，限流为 429，内部故障为 500 `internal`，依赖故障为 503，超时为 504。SSE 开始后的错误以 `error` 事件结束流。`outcome_unknown` 表示写入结果需通过查询确认，不能直接重复写入。

浏览器携带 `credentials: 'include'`。所有 POST 使用 `Content-Type: application/json` 和 `X-Chat-Request: 1`。浏览器通过 `Sec-Fetch-Site: same-origin` 标记的同源请求可通过来源检查，兼容反向代理改写 Host 和终止 TLS；其他请求的非空 Origin 必须匹配服务来源或 `auth.allowed_origins`。缺少 Fetch Metadata 的客户端沿用 Origin 检查，服务来源按 `auth.secure_cookie` 对应协议和 Host 组成。通过检查的非空 Origin 获得带凭据 CORS 响应。会话 Cookie 使用 HttpOnly、SameSite=Lax，Secure 由部署配置决定。静态资源允许匿名，API 响应禁止缓存。

## 账号

| 接口 | JSON 输入 | 成功结果 |
| --- | --- | --- |
| POST /api/auth/register | username, password, email 可选 | 201，user、email_delivery（sent / failed / not_requested），注册不自动登录 |
| POST /api/auth/login | identifier, password | user，并设置 Cookie |
| GET /api/auth/me | 无 | user |
| POST /api/auth/username | username | user，保留已有登录状态 |
| POST /api/auth/logout | {} | ok，仅退出当前设备 |
| GET /api/auth/sessions | cursor 可选，初始为 "0" | items、next_cursor，当前账号的有效会话 |
| POST /api/auth/sessions/revoke | session_id | ok，退出指定设备；当前设备被退出时清除 Cookie |
| POST /api/auth/logout/all | {} | ok，退出该账号全部设备（含当前设备），清除当前 Cookie |
| POST /api/auth/password | old_password, new_password | ok，所有设备重新登录 |
| POST /api/auth/password/verify | password | ok、expires_in（秒），授权当前会话管理邮箱 |
| POST /api/auth/email/code | email | ok，向目标邮箱发送绑定验证码 |
| POST /api/auth/email/bind | email, code | user，验证成功后才替换原邮箱 |
| POST /api/auth/email/unbind | {} | user，取消绑定 |
| POST /api/auth/password/forgot | email | 202，统一受理说明；异步发送，不暴露是否存在 |
| POST /api/auth/password/reset | email, code, new_password | ok，所有设备重新登录 |

注册、登录、找回和重置允许匿名，其余要求登录。user 包含 id、username、email（未绑定为 null）。用户名规范化为小写，3–32 位 ASCII 字母、数字、下划线，以字母开头。邮箱去除首尾空白并转为小写，仅支持常见 ASCII 邮箱。密码不去空白，默认 8–128 个 Unicode 字符；验证码为 6 位数字。

邮箱管理分两步：先调用 `/api/auth/password/verify` 验证当前密码，再发送验证码、绑定或解绑邮箱。验证记录仅用于当前账号、当前登录会话的邮箱操作，默认有效 300 秒，由 `auth.password_verification_seconds` 配置（1–3600 秒）。后续邮箱接口不再传密码；缺少验证或验证过期时返回 403 `reauthentication_required`，客户端返回密码验证页，登录会话仍然有效。更改密码后旧验证记录失效。前端离开邮箱设置时清空密码及本地验证状态，再次进入从密码验证开始。

修改用户名只需登录，按登录身份确定账号；新用户名沿用注册时的格式和唯一约束，重复返回 409。修改后用户 ID、邮箱、密码、聊天记录和已有会话保持不变，下次登录使用新用户名或已绑定邮箱。原用户名可被重新注册。

退出全部设备从当前登录身份确定账号，不接受客户端传入用户 ID。所有实例共享的旧会话随后鉴权返回 401；其他账号不受影响，退出后仍可使用原密码重新登录。此操作使用账号操作限流，前端在独立确认页提交。

设备列表的 item 包含 `id`、`user_agent`、`created_at`、`last_active_at`、`expires_at`（当前闲置到期时间，Unix 毫秒）、`current`。`id` 是撤销时使用的标识，不能用于登录；不返回 Cookie 凭据。浏览器和系统名称从 User-Agent 推断，用于展示。列表遍历每次以 50 条为提示分页，Redis 可能返回较多或较少项；`next_cursor="0"` 表示结束，客户端按 id 去重，遍历期间变动的会话可能需要重新刷新。

查询和单台撤销只处理当前账号。撤销其他账号或不存在的会话统一返回 404；撤销当前设备后，所有使用同一 Cookie 的标签页后续鉴权均返回 401。过期或已撤销会话不出现在列表中。升级前的会话在下一次成功鉴权时补入账号索引，并记录请求中的浏览器信息。不同浏览器或浏览器配置文件会产生独立会话，同一浏览器中共享 Cookie 的标签页显示为同一设备。

## 用户管理

`/admin` 使用独立管理密码。管理员 Cookie 名为 `auth.cookie_name` 加 `_admin`，闲置有效期为一小时。普通用户 Cookie 与管理 Cookie 分别鉴权。

| 接口 | 输入 | 成功结果 |
| --- | --- | --- |
| POST /api/admin/login | password | ok，并设置管理 Cookie |
| GET /api/admin/me | 无 | ok |
| POST /api/admin/logout | {} | ok，撤销当前管理会话并清除 Cookie |
| GET /api/admin/users | 可选 search、before_id | items、next_before_id，按 ID 降序，每页最多 50 条 |
| POST /api/admin/users/update | user_id，username、disabled 至少一个 | user |
| POST /api/admin/users/logout | user_id | ok，撤销该用户全部设备的会话 |

除管理登录外均需管理会话，所有 POST 沿用来源和 CSRF 校验。用户列表包含 id、username、email、disabled、created_at、updated_at，时间为 Unix 毫秒。search 按用户名、邮箱包含匹配或 ID 完全匹配；next_before_id 为 null 时结束。管理密码未配置返回 403，错误密码返回 401，登录尝试受限流控制。停用账号后普通登录返回 403，已有会话鉴权返回 401；启用后须重新登录。

## 历史与聊天

全部要求登录。用户 ID 来自 Auth 验证后的会话，不接受客户端指定归属。

| 接口 | 输入 | 成功结果 |
| --- | --- | --- |
| GET /api/models | 无 | models，仅 provider、model |
| POST /api/conversations | title | 201，conversation |
| GET /api/conversations | 可选 limit（1–100）、before_updated_at、before_id | items、next；按更新时间、ID 降序 |
| GET /api/conversation | id | conversation、messages（当前分支按发送顺序） |
| POST /api/conversation/rename | conversation_id、title | ok |
| POST /api/conversation/select | conversation_id、message_id（助手回复 ID） | conversation、messages，切换回复版本并恢复该分支的已有续聊 |
| POST /api/conversation/delete | conversation_id | ok，删除消息和请求幂等记录 |
| GET /api/conversation/messages | conversation_id，可选 limit（1–500）、before | items、next_before；包含保留的旧分支 |
| GET /api/conversation/context | conversation_id、last_message_id | items，沿 parent_id 读取上下文 |
| POST /api/chat | conversation_id、request_id、provider、model、stream 可选；content 与 regenerate_message_id 二选一 | 流式 SSE 或普通 JSON |

聊天请求的 request_id 为 1–80 位字母、数字、下划线或连字符。相同对话内同一 request_id 和相同问题只启动一次生成，内容不同时返回 409。已完成的重试返回已保存的回复；生成中的重试返回 409。失败或中断后重新生成使用新的 request_id。每个对话同时最多生成一条回复，不跨模型调用持有事务。

流式请求获得模型队列名额后立即返回 `200` 和 SSE 注释 `: connected`。队列满时返回 JSON `503`。SSE 事件顺序为 `meta`（conversation_id、user_message_id、assistant_message_id）、`delta` / `reasoning_delta`（text）、`done`（finish_reason、truncated）。失败以 `error` 结束。`done` 只在最终回复提交 MySQL 成功后发送。普通 JSON 含 reply、reasoning、finish_reason、消息 ID。重新生成在成功前保留原回答。中断或失败的新增分支仍可通过消息接口查询。

原始 SSE 流、单条上游 SSE 事件、解包后的正文与思考合计大小分别计量。正文与思考合计上限为 4 MiB（UTF-8 字节）。上游流超出任一上限时发送 `error`，其 `code` 为 `generation_failed`，`error` 文本说明具体上限。历史保留已接收的合规增量，回复状态为 `failed`。

进度约每 250ms 或累计 32KiB 保存一次；终态通过递增 sequence 防止旧快照覆盖。生成租约默认 5 分钟（可通过 `server.generation_lease_ms` 调整），在排队与生成活跃期间定期续期，进度保存时也会续期；进程崩溃后过期的占位在读取或新请求时标为中断，旧生成不能提交覆盖新生成。进程退出或崩溃后，已提交内容保留。历史列表使用游标，期间有更新的会话可能移到更靠前位置，客户端按 ID 去重。

conversation 同时返回 current_message_id 和 active_message_id；重新生成期间前者保留原回答，后者标识生成任务，客户端据此禁用同一会话的并发发送并轮询刷新。

`GET /api/conversation` 和 `POST /api/conversation/select` 返回的已结束生成的助手消息包含 `versions`：`index`（从 1 开始）、`total`、`previous_id`、`next_id`（边界为 null）。版本按消息 ID 升序排列，同一个用户问题的已完成、已中断和失败回复均可浏览；正在生成的版本结束后才计入。单个版本不显示切换控件。

切换版本只更新当前分支，不删除消息、不调用模型。目标回复必须属于当前用户的指定对话；生成期间返回 409。选中回复已有续聊时，沿每层最新子消息恢复后续对话；选择结果保存到 MySQL，刷新和重新打开后继续使用该分支，后续提问以该分支作为上下文。
