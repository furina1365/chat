# 测试文档

## 1. 测试目标与范围

验证账号与设备会话、邮箱与密码、聊天与历史、模型协议、HTTP 解析、浏览器交互和部署行为。接口字段见 [接口文档](api.md)，环境搭建见 [README](../README.md)。

本文件是测试计划与操作手册；执行结果填写在第 10 节。优先级约定：

| 优先级 | 含义 | 执行时机 |
| --- | --- | --- |
| P0 | 核心流程、账号隔离、凭据与历史一致性 | 每次发布 |
| P1 | 参数边界、异常恢复、兼容性 | 对应模块变更及发布前 |
| P2 | 长时间运行、容量与性能观察 | 网络、线程池、连接池或部署变更 |

## 2. 环境与数据准备

### 2.1 环境矩阵

| 维度 | 测试组合 |
| --- | --- |
| 网络后端 | epoll Reactor、io_uring Proactor，分别构建并启动 |
| 数据依赖 | 独立 MySQL 测试数据库、独立 Redis 实例或逻辑库；导入 `backend/db/schema.sql` |
| 上游模型 | 可记录请求和控制响应的本地 HTTP 模拟服务；每种协议再做一次真实上游联调 |
| 邮件 | 测试 SMTP 收件端，支持 TLS 或 STARTTLS；自签名证书通过 `email.smtp.ca_file` 配置信任 |
| 浏览器 | Chrome、Firefox；记录实际版本；桌面和 390px 窄屏 |
| 访问方式 | 本机 HTTP、Nginx HTTPS；跨域场景另配前端来源 |
| 多实例 | 两个不同监听端口，共享同一 MySQL、Redis 和业务 namespace |

模拟上游需要支持：固定正文与思考、逐段发送、记录调用次数与上下文、延迟、断流、非法 JSON、HTTP 错误、超限回复，以及观察客户端取消。故障测试在隔离环境执行。

### 2.2 配置与启动

从项目根目录操作，为测试准备 `config.json`：

```bash
cp -n config.json.example config.json
```

填写测试 MySQL、Redis 和模型连接信息，将三个 namespace 设置为本次测试独有的值：`auth.key_namespace`、`auth.session_namespace`、`email.key_namespace`。多实例用例中两个实例使用相同值。

模板中的 `auth.secure_cookie` 为 `true`。本机 HTTP 设置为 `false`，HTTPS 设置为 `true`。同源测试中 `allowed_origins` 可为空；前端单独使用 `http://localhost:8000` 时，将此完整来源加入列表，并统一使用 `localhost` 访问接口。

首次建库和授权按 README 执行，在已准备的测试数据库中导入结构：

```bash
mysql -h 127.0.0.1 -u 测试数据库用户 -p 测试数据库名 < backend/db/schema.sql
```

分别使用两个构建目录，避免切换覆盖：

```bash
cmake -S . -B build-reactor -DNET_BACKEND=reactor
cmake --build build-reactor -j
cmake -S . -B build-proactor -DNET_BACKEND=proactor
cmake --build build-proactor -j
```

每次只启动一个后端，完成同一组用例后用 Ctrl+C 停止，再启动另一个：

```bash
./build-reactor/server
```

```bash
./build-proactor/server
```

记录构建模式、提交号及工作区修改、内核、数据库版本、线程与队列配置。Proactor 还要记录运行环境是否允许 io_uring。

### 2.3 测试数据

- 账号 A、B 使用不同用户名；A 在两个浏览器配置文件中登录，分别记为 A1、A2。
- 准备两个可接收验证码的邮箱，分别用于绑定、更换与找回。
- 密码使用测试专用值，默认长度边界为 8–128 个 Unicode 字符。
- 会话 C1 属于 A，C2 属于 B；为分页准备至少 3 个会话及 3 轮消息，分页 limit 取 1、2。
- 每个独立聊天请求使用新的 `request_id`；去重测试刻意复用同一个标识。

## 3. 接口冒烟测试

Postman 回归使用 [chat 集合](../postman/collections/chat.postman_collection.json)，覆盖账号、设备、历史、幂等、SSE 与回复版本；运行方法见 [Postman 说明](../postman/README.md)。

前置条件：服务已启动、注册已开放、模型上游可用，安装 Bash、curl、jq、Python 3。以下命令在同一个 Bash 会话执行，默认访问本机 HTTP，执行一次非流式生成和一次流式生成。

```bash
set -euo pipefail
TEST_BASE_URL="${TEST_BASE_URL:-http://localhost:53411}"
TEST_DIR=$(mktemp -d)
chmod 700 "$TEST_DIR"
trap 'rm -rf "$TEST_DIR"' EXIT
TEST_USERNAME="test_$(date +%s)_${RANDOM}"
TEST_PASSWORD='chat test password 12345'

post_json() {
  local path="$1" body="$2" output="$3" expected="$4" status
  status=$(curl --silent --show-error --max-time 120 \
    -b "$TEST_DIR/cookies" -c "$TEST_DIR/cookies" \
    -H 'Content-Type: application/json' -H 'X-Chat-Request: 1' \
    --data "$body" -o "$output" -w '%{http_code}' "$TEST_BASE_URL$path")
  if [ "$status" != "$expected" ]; then
    printf '%s: expected %s, got %s\n' "$path" "$expected" "$status" >&2
    return 1
  fi
}

touch "$TEST_DIR/cookies"
status=$(curl -sS --max-time 10 -o "$TEST_DIR/anonymous.json" \
  -w '%{http_code}' "$TEST_BASE_URL/api/auth/me")
test "$status" = 401
jq -e '.code == "unauthenticated"' "$TEST_DIR/anonymous.json" >/dev/null

post_json /api/auth/register \
  "$(jq -nc --arg u "$TEST_USERNAME" --arg p "$TEST_PASSWORD" \
    '{username:$u,password:$p}')" "$TEST_DIR/register.json" 201
post_json /api/auth/login \
  "$(jq -nc --arg u "$TEST_USERNAME" --arg p "$TEST_PASSWORD" \
    '{identifier:$u,password:$p}')" "$TEST_DIR/login.json" 200

curl -fsS --max-time 10 -b "$TEST_DIR/cookies" \
  "$TEST_BASE_URL/api/auth/me" > "$TEST_DIR/me.json"
jq -e --arg u "$TEST_USERNAME" '.user.username == $u' "$TEST_DIR/me.json" >/dev/null
curl -fsS --max-time 10 -b "$TEST_DIR/cookies" \
  "$TEST_BASE_URL/api/models" > "$TEST_DIR/models.json"
TEST_PROVIDER=$(jq -er '.models[0].provider' "$TEST_DIR/models.json")
TEST_MODEL=$(jq -er '.models[0].model' "$TEST_DIR/models.json")

post_json /api/conversations '{"title":"接口冒烟测试"}' "$TEST_DIR/create.json" 201
TEST_CONVERSATION=$(jq -er '.conversation.id' "$TEST_DIR/create.json")
TEST_CHAT_BODY=$(jq -nc --arg c "$TEST_CONVERSATION" \
  --arg p "$TEST_PROVIDER" --arg m "$TEST_MODEL" \
  '{conversation_id:$c,request_id:"smoke_json",provider:$p,model:$m,
    stream:false,content:"请回复：测试成功"}')
post_json /api/chat "$TEST_CHAT_BODY" "$TEST_DIR/reply.json" 200
jq -e '.reply | type == "string"' "$TEST_DIR/reply.json" >/dev/null

# 重试已完成请求：消息 ID 与已保存回复保持一致。
post_json /api/chat "$TEST_CHAT_BODY" "$TEST_DIR/replay.json" 200
jq -e -s '.[0].assistant_message_id == .[1].assistant_message_id
  and .[0].user_message_id == .[1].user_message_id
  and .[0].reply == .[1].reply' \
  "$TEST_DIR/reply.json" "$TEST_DIR/replay.json" >/dev/null

TEST_STREAM_BODY=$(jq -nc --arg c "$TEST_CONVERSATION" \
  --arg p "$TEST_PROVIDER" --arg m "$TEST_MODEL" \
  '{conversation_id:$c,request_id:"smoke_stream",provider:$p,model:$m,
    stream:true,content:"再回复一句简短的话"}')
post_json /api/chat "$TEST_STREAM_BODY" "$TEST_DIR/stream.txt" 200
python3 - "$TEST_DIR/stream.txt" <<'PY'
import pathlib, sys
events = [line[6:].strip() for line in pathlib.Path(sys.argv[1]).read_text().splitlines()
          if line.startswith('event:')]
assert events and events[0] == 'meta', events
assert events[-1] == 'done' and 'error' not in events, events
PY

curl -fsS --max-time 10 -b "$TEST_DIR/cookies" \
  "$TEST_BASE_URL/api/conversation?id=$TEST_CONVERSATION" > "$TEST_DIR/history.json"
jq -e '.messages | length == 4' "$TEST_DIR/history.json" >/dev/null
post_json /api/conversation/delete \
  "$(jq -nc --arg c "$TEST_CONVERSATION" '{conversation_id:$c}')" "$TEST_DIR/delete.json" 200
post_json /api/auth/logout '{}' "$TEST_DIR/logout.json" 200
status=$(curl -sS --max-time 10 -b "$TEST_DIR/cookies" \
  -o "$TEST_DIR/logged-out.json" -w '%{http_code}' "$TEST_BASE_URL/api/auth/me")
test "$status" = 401
printf '接口冒烟测试通过，测试账号：%s\n' "$TEST_USERNAME"
```

通过标准：命令退出码为 0；重复请求返回相同消息 ID；历史只有两轮消息；退出后鉴权返回 401。上游调用次数由第 5 节的模拟服务验证。脚本保留测试账号，删除测试会话并退出当前设备；失败时先按第 10 节记录现象，临时文件在 shell 退出时清理。

## 4. 账号、设备和邮箱

所有业务 POST 均携带 JSON Content-Type、`X-Chat-Request: 1`；受保护接口同时携带登录 Cookie。错误同时核对 HTTP 状态及 JSON `code`。

| 编号 | 优先级 | 操作与输入 | 预期结果 |
| --- | --- | --- | --- |
| AUTH-01 | P0 | 注册用户名 `Test_A`，再以 `test_a` 注册 | 首次 201，返回小写用户名；第二次 409 `conflict`；注册后直接访问 `/api/auth/me` 为 401 |
| AUTH-02 | P1 | 分别注册 2、3、32、33 字符用户名，数字开头、中文、空格用户名 | 3、32 位且符合字母开头的 ASCII 字母/数字/下划线规则时成功，其余 400 |
| AUTH-03 | P1 | 使用 7、8、128、129 字符密码注册；补充中文、emoji、首尾空格样本 | 默认允许 8–128 字符；超界 400；首尾空格参与密码校验 |
| AUTH-04 | P0 | 用户名登录、错误密码登录、不存在账号登录；绑定后用邮箱登录 | 正确凭据 200；错误密码和不存在账号均为 401 `invalid_credentials`；返回体不含密码哈希与登录 token |
| AUTH-05 | P0 | 登录后查看 Set-Cookie，并分别访问 HTTP、HTTPS 环境 | Cookie 有 HttpOnly、SameSite=Lax、Path=/；HTTPS 配置有 Secure；页面刷新仍登录 |
| AUTH-06 | P1 | 关闭 `registration_enabled` 后重启，注册新账号、登录已有账号 | 注册 403 `registration_closed`；已有账号可登录 |
| AUTH-07 | P1 | 独立 namespace 中连续错误登录超过 `login_attempts`；大小写变换登录标识 | 超限 429 `rate_limited`，有 Retry-After；大小写变换共享计数；窗口结束后可重试 |
| AUTH-08 | P0 | A1、A2、B 分别登录，A1 调用 `/api/auth/logout` | A1 旧 Cookie 返回 401；A2、B 保持有效；当前 Cookie 被清除 |
| AUTH-09 | P0 | 查询设备列表，遍历到 `next_cursor="0"`；A1 撤销 A2 | 列表仅含 A 的有效会话，有时间和 current 字段；不含 Cookie 凭据；A2 后续 401，A1 有效 |
| AUTH-10 | P0 | A1 撤销 B 的设备或不存在设备；再撤销自己 | 前两项统一 404；撤销自己后 Cookie 清除，原 Cookie 失效 |
| AUTH-11 | P0 | A1 调用 `/api/auth/logout/all`，分别在两个实例访问；再以原密码登录 | A 的所有旧 Cookie 在两个实例均为 401；B 有效；原密码仍可重新登录 |
| AUTH-12 | P0 | 错误旧密码修改密码，再用正确旧密码修改 | 错误旧密码 401；成功后所有设备重新登录，旧密码失败、新密码成功 |
| AUTH-13 | P1 | 缩短测试配置中的闲置会话时间；分别静置和持续访问，并模拟已登录超过 7 天 | 闲置超时后 401；持续访问可续期且 Cookie 同步续期；登录超过 7 天仍可使用；过期会话不出现在设备列表 |
| MAIL-01 | P0 | 未验证密码就发送、绑定、解绑邮箱；A1 验证后由 A2 操作 | 均为 403 `reauthentication_required`；A1 验证不授权 A2 |
| MAIL-02 | P0 | A1 验证密码、发送验证码、正确绑定，再用邮箱登录 | 验证返回 expires_in；验证码可达；成功后 user.email 更新，邮箱登录成功 |
| MAIL-03 | P1 | 验证超时后操作邮箱；修改密码后重新登录再操作 | 邮箱操作要求再次验证；单纯验证过期不使登录会话失效 |
| MAIL-04 | P1 | 重发过快、错误验证码、过期验证码、重复使用已消费验证码 | 按配置拒绝；重发过快为 429，错误或失效验证码为 400 `invalid_code`；达到尝试上限后正确旧码也不可使用 |
| MAIL-05 | P0 | 更换邮箱时先提交错误码，再提交正确码；A 绑定 B 已绑定邮箱 | 错误码不替换旧邮箱；正确码才替换；已占用邮箱冲突，账号归属保持一致 |
| MAIL-06 | P0 | 验证密码后解绑邮箱，再用原邮箱和用户名登录 | email 为 null；原邮箱不能登录该账号，用户名仍可登录 |
| MAIL-07 | P0 | 已绑定邮箱与不存在邮箱分别请求找回；正确码重置密码 | 队列可用时找回均返回相同 202 说明；成功重置后旧密码和全部旧会话失效 |
| MAIL-08 | P1 | SMTP 未配置、连接失败、证书不受信任；注册时传 email | 用户名注册/登录可用；注册返回 email_delivery=failed、邮箱仍未绑定；发送失败不假报已送达，TLS 证书校验生效 |

## 5. 聊天、历史与幂等

使用确定性模拟回复核对正文与数据库内容；通过模拟上游的请求记录核对次数、角色及上下文。

| 编号 | 优先级 | 操作与输入 | 预期结果 |
| --- | --- | --- | --- |
| CHAT-01 | P0 | 匿名读取模型/历史或发送聊天；登录后读取模型 | 匿名 401；模型列表只返回 provider、model，无 Key 或上游连接配置 |
| CHAT-02 | P0 | 创建、改名、读取、删除会话 | 创建 201；改名后可读；删除后 404，相关 messages、turn_requests 一并删除 |
| CHAT-03 | P0 | A 对 B 会话执行读取、分页、上下文、改名、删除、聊天 | 均为 404；B 数据和上游调用次数不变 |
| CHAT-04 | P0 | 非流式单轮、多轮聊天；上轮包含思考文本 | JSON 含 reply、reasoning、finish_reason 和字符串消息 ID；下一轮仅发送当前分支正文上下文，不包含 reasoning |
| CHAT-05 | P0 | 流式生成，记录原始 SSE | 首事件 meta，正文 delta 与 reasoning_delta 分开；成功末事件 done；查询历史与拼接结果一致 |
| CHAT-06 | P0 | 同一会话同时提交相同 request_id、相同内容 | 只调用一次上游；生成中的重试 409，完成后的重试返回同一消息 ID 与保存内容 |
| CHAT-07 | P0 | 已完成请求以同一 request_id 改变 content；相同请求分别以流式、非流式重试 | 改变 content 为 409；同内容重试均回放保存结果，不再次生成 |
| CHAT-08 | P0 | 同一会话以不同 request_id 并发生成，再对另一会话生成 | 同一会话最多一条生成，其余 409；另一会话在容量允许时可执行 |
| CHAT-09 | P0 | 对用户问题 ID 重新生成，观察 current_message_id、active_message_id | 生成期间 current 指向原回答，active 指向新回答；成功后 current 切换、active 清空；旧分支可由消息接口读取 |
| CHAT-10 | P0 | 重新生成中停止或注入上游失败；把助手 ID 传入 regenerate_message_id | 停止/失败保留原当前分支，新增分支保留终态；助手 ID 为 400；跨会话消息 ID 为 404 |
| CHAT-11 | P0 | 流式收到部分正文/思考后停止、关闭标签页或断开连接 | 上游观察到取消；已收到内容在最终保存成功后可读取，state 为 interrupted；可以使用新 request_id 重新生成 |
| CHAT-12 | P0 | 成功回复后重启应用；另一实例读取同一账号 | 已提交账号、会话和历史仍可读取，消息 ID、正文与分支一致 |
| CHAT-13 | P0 | 生成中强制终止测试进程，重启后等待最后续租时间起超过 5 分钟，再读取该会话 | 已提交进度保留；过期占位转 interrupted、active 清空；可开始新生成 |
| CHAT-14 | P1 | limit=1/2 遍历会话与消息；遍历中更新某会话 | 静态数据按游标完整遍历；会话按 updated_at、id 降序；变动时前端按 ID 去重 |
| CHAT-15 | P1 | 标题为空、256/257 字节；正文为空、纯空白、80000/80001 字节 | 标题允许 1–256 字节；非空白正文最多 80000 字节；非法输入 400；按 UTF-8 字节构造中文边界样本 |
| CHAT-16 | P1 | request_id 为空、80/81 位、含空格；content 与 regenerate_message_id 同传或都不传 | 仅 1–80 位字母/数字/下划线/连字符合法；两字段必须二选一；非法输入 400 |
| CHAT-17 | P1 | 无效 provider/model、stream 字符串、数字类型 ID、ID 为 0；分页数量越界 | 400 `invalid_request`；会话 limit 为 1–100，消息 limit 为 1–500 |
| CHAT-18 | P1 | 构造累计正文超过 240 KiB 或分支深度超过 500 条消息的上下文 | 400；不调用上游；生成占位结束，后续可新建会话 |
| CHAT-19 | P0 | 在 History 接口层依次提交 sequence=2、1、3，再补交旧进度；尝试旧任务覆盖新任务 | 旧快照不覆盖新内容；终态不能回到 generating；租约失效或任务归属已变更时拒绝旧任务提交 |

CHAT-19 需要直接调用 `History::SaveProgress` / `History::Finish` 的测试程序，HTTP 不暴露 sequence。测试程序使用独立数据库，通过消息查询和数据库中的 sequence 核对结果。

### 5.1 三种上游协议

每种协议分别覆盖非流式、流式、多轮、取消及错误响应，并核对实际请求路径、认证头和消息编码。

| 配置 api | 重点核对 |
| --- | --- |
| openai-completions | Chat Completions 请求；正文与 reasoning_content 分开；正常终止与 length 截断；DeepSeek 思考配置 |
| openai-responses | Responses 请求；输出文字与公开 reasoning summary 分开；encrypted_content 不展示、不进入后续上下文 |
| anthropic-messages | Messages 请求及协议认证头；text、thinking 分开；signature 和签名增量不展示；采样参数按配置发送 |

| 编号 | 优先级 | 模拟场景 | 预期结果 |
| --- | --- | --- | --- |
| AI-01 | P1 | 把 SSE 按单字节、随机块、中文 UTF-8 中间位置拆包；使用 LF、CRLF、CR 换行 | 拼接正文与思考准确，无乱码、重复或丢失 |
| AI-02 | P1 | SSE 注释心跳、首行 BOM、多行 data、未知字段 | 格式解析正确；未知字段及注释不成为回复正文 |
| AI-03 | P0 | 缺少协议正常结束事件就断流；返回非法 JSON | 以失败结束；不发送 done，不将半段内容标记 completed |
| AI-04 | P1 | HTTP 401/429/500、拒绝连接、连接或总超时 | 非流式上游失败通常为 502，超时为 504；已开始 SSE 的请求以 error 结束；响应不泄露 Key 或原始敏感错误体 |
| AI-05 | P1 | 返回 length、refusal；超过 max_response_bytes 或正文与思考合计 4 MiB | 截断回复带 truncated=true；拒答有可见提示；超限停止并结束为错误，不无限积累内存 |

## 6. HTTP、安全与配置边界

原始 HTTP 用例通过 TCP socket 直接连接应用端口，准确控制报文字节与半关闭；Nginx 环境单独记录代理返回结果。解析层错误核对状态码及连接结束，业务 JSON 错误另核对 `code`。

| 编号 | 优先级 | 操作与输入 | 预期结果 |
| --- | --- | --- | --- |
| HTTP-01 | P1 | `/api/hello`、`/api/echo`；合法请求逐字节发送；一个连接发送两条流水线请求 | hello 为 200 文本，echo 原样返回；半包正确解析；每个连接只处理一条请求并关闭 |
| HTTP-02 | P1 | POST 缺少 Content-Length、声明体长超过 256 KiB、重复 Content-Length、Transfer-Encoding、HTTP/1.0、Upgrade | 分别核对 411、413；其余畸形或不支持请求为 400 |
| HTTP-03 | P1 | 请求体 256 KiB 与多 1 字节；请求行及头部累计 16384 字节与多 1 字节，另加末尾 CRLFCRLF | 使用 echo 隔离业务限制：体边界合法、超限 413；头边界合法、超限 431 |
| HTTP-04 | P1 | 非法请求头、头部/请求体不完整时半关闭写端 | 返回 400，不继续等待无法补齐的内容 |
| SEC-01 | P0 | 受保护业务 POST 缺少 X-Chat-Request 或使用错误 Content-Type；携带未允许 Origin | 403，分别为 csrf_rejected、origin_rejected；业务不执行；hello/echo 为独立诊断路由 |
| SEC-02 | P1 | 允许来源发 OPTIONS 和带 Cookie 请求；拒绝来源预检 | 允许预检 204，含对应 Origin、凭据、方法及头部；拒绝来源 403 |
| SEC-06 | P0 | Chromium 经 Nginx 访问 HTTP localhost 和 HTTPS 域名；代理将 Host 改写成上游地址；另用外站和同站子域发起跨源请求 | 同源注册、登录、Cookie、模型列表、建会话、SSE 聊天与刷新恢复成功；外站和同站子域的预检均为 403；页面脚本无法伪造 Sec-Fetch-Site |
| SEC-03 | P1 | JSON 非对象、字段缺失、类型错误、未知字段；查询参数未知或重复 | 业务接口返回 400；API 响应带 Cache-Control: no-store |
| SEC-04 | P0 | 在用户名、邮箱、标题、正文输入引号、反斜线、SQL 片段；访问 `/config.json`、`/../config.json` | 按字段规则拒绝或作为文本处理；账号隔离不变；静态路径不返回配置与源码；curl 测路径时使用 --path-as-is |
| SEC-05 | P0 | 检查浏览器存储、前端网络响应、错误日志及部署包 | 无密码、API Key、会话凭据或新聊天内容写入 localStorage；模型列表及错误不泄露配置秘密；已有匿名本地历史不迁入账号 |
| CFG-01 | P1 | 缺失/损坏配置，server/mysql/redis/auth/email 嵌套未知或重复字段，非法端口、负数线程、零连接池 | 启动失败，诊断可定位配置问题且不输出密码或 Key |
| CFG-02 | P1 | 合法 IPv4/IPv6/主机名、不可解析地址、端口被占用 | 正确地址可访问；IPv6 用方括号 URL；无法解析或绑定时启动失败 |
| CFG-03 | P1 | 模型为空、重复 provider/model、非法 api、非 HTTP(S) URL、缺少 Key、越界采样参数 | 启动失败；Messages 同时设置 temperature、top_p 时拒绝 |
| CFG-04 | P0 | 启动时 MySQL/Redis 不可达、凭据错误、缺少历史表；io_uring 不可用 | 启动失败并给出诊断；数据库表结构问题提示导入 schema.sql |

## 7. 浏览器交互与渲染

桌面和窄屏分别执行；保存关键页面截图及浏览器控制台错误。

| 编号 | 优先级 | 操作 | 预期结果 |
| --- | --- | --- | --- |
| UI-01 | P0 | 注册、登录、刷新、退出；检查登录失败反馈 | 状态与 API 一致；退出后受保护内容清理，登录入口可用 |
| UI-02 | P0 | Enter 发送、Shift+Enter 换行；中文输入法选字时按 Enter；空白输入 | 发送和换行正确；选字不误发送；空白不提交 |
| UI-03 | P0 | 选择模型、多轮发送、复制正文、停止、重新生成 | 操作有效；生成时可停止且同一会话不能重复发送；复制内容不夹带思考 |
| UI-04 | P0 | 生成时切换会话、刷新、另一标签页查看 | 回复归属正确；通过历史和 active_message_id 恢复状态，无跨会话串文 |
| UI-05 | P0 | 重新生成成功、失败和中途停止 | 新回答成功前原回答保留；失败/停止有提示；刷新后当前分支一致 |
| UI-06 | P1 | 标题、列表、引用、表格、任务列表、删除线、代码、行内与块级公式；相同内容从流式与历史加载 | 两种加载方式渲染一致；代码内公式符号保留；未知代码语言、错误公式不使页面崩溃 |
| UI-07 | P0 | 用模拟回复返回 script、img onerror、javascript 链接、data/file 图片地址 | 原始 HTML 作为文本；危险链接和事件不执行；新窗口链接有 noopener noreferrer |
| UI-08 | P1 | 长代码、宽表格、长公式、长单词；390px 页面、键盘导航 | 内容可阅读；表格局部滚动；输入框、模型菜单、侧栏及设置入口可操作 |
| UI-09 | P0 | 进入邮箱设置、验证密码、离开再进入；验证到期后提交 | 离开清空密码和本地验证状态；再次进入先验证；到期返回验证页且登录有效 |
| UI-10 | P0 | 已登录设备中撤销其他设备和当前设备；确认退出全部设备 | 页面与后端失效范围一致；共享 Cookie 的标签页后续鉴权失效；取消确认不执行退出 |
| UI-11 | P1 | 断网、上游错误、API 401/429/503；阻止 CDN 请求后刷新 | 错误可见，按钮状态可恢复；本地 Markdown、KaTeX、字体仍可加载 |

## 8. 故障、并发与部署

### 8.1 故障与恢复

| 编号 | 优先级 | 操作 | 预期结果 |
| --- | --- | --- | --- |
| FAIL-01 | P0 | 运行中断开 MySQL 或 Redis，再恢复依赖 | 请求以依赖错误或超时结束；服务存活；坏连接被丢弃，后续请求可重新连接 |
| FAIL-02 | P0 | 模拟最终历史提交失败和提交结果未知 | SSE 不发送成功 done；提示保存失败或查询确认；outcome_unknown 时先查询消息与幂等记录，客户端不直接重复写入 |
| FAIL-03 | P0 | 以延迟上游占满模型线程与队列，再请求不同会话；另占满业务队列 | 队列满返回 503；失败占位不永久阻塞会话；任务结束后恢复受理 |
| FAIL-04 | P1 | 客户端暂停读取，快速模拟上游持续输出，使 SSE 待发送队列超过 1 MiB | 取消生成并结束慢连接；其他连接可用；资源最终释放 |
| FAIL-05 | P0 | 多个生成请求运行时发送 SIGTERM / Ctrl+C | 停止接入，取消模型调用，线程退出；已提交历史可读取；残留占位按租约恢复 |
| FAIL-06 | P1 | 多连接快速建连、断连、发送中关闭，分别运行两种后端 | 无崩溃或挂起；观察连接、fd、内存与线程数在负载结束后是否稳定 |

### 8.2 性能记录

使用本地确定性上游，固定回复字节数、分块频率、延迟、线程数、队列和连接池配置；对 Reactor、Proactor 使用相同数据与负载。逐级增加并发，直到出现排队或 503，再降低负载观察恢复。

分别测量 hello/echo、登录、历史分页和聊天。历史与聊天复用已登录会话；登录单列，包含密码哈希耗时。记录请求数、成功率、各状态码数量、吞吐量、p50/p95/p99 延迟、流式首事件与首正文时间、CPU、RSS、fd 和线程数。聊天使用不同会话，避免把同会话 409 当作容量结果。

执行前填写目标并发、持续时间、延迟预算与资源上限；执行后记录原始数据。真实上游另记网络和提供方延迟。

### 8.3 部署验收

- 二进制与 Docker 分别从部署目录启动，核对 `config.json`、`frontend/` 等运行文件可读；Docker 以 UID 10001 读取配置，host 网络能连接本机数据库。
- 经 Nginx HTTPS 完成登录、流式生成、停止和历史刷新，确认分块及时到达、Secure Cookie 可用。
- 重启应用后验证历史；跨实例验证退出、密码变更、会话互斥与幂等。
- 记录镜像或二进制标识、配置摘要和故障恢复步骤；测试结束停止临时进程，清理本次测试数据库、Redis namespace 和测试收件内容。

## 9. 自动化组织与验收标准

当前 CMake 构建 `server`，前端 npm 提供 `typecheck` 严格类型检查和 `build` 资源构建命令（`vendor` 为兼容别名）。开发机上的 `checks/`、`test/`、`tests/` 受 `.gitignore` 排除；新环境先使用第 3 节和表中步骤执行。

后续将可复现用例纳入版本管理时，按以下职责组织：

| 层次 | 内容 | 外部依赖 |
| --- | --- | --- |
| 组件测试 | HttpParser 半包与边界、SseParser 拆包、配置校验、History sequence 与终态 | 历史组件用隔离 MySQL，其余使用内存数据或临时配置 |
| 集成测试 | 账号/设备/邮箱、历史/幂等、模型协议、故障注入 | 测试 MySQL、Redis、SMTP、模拟上游 |
| 浏览器测试 | 登录与账号设置、生成/停止/重试、Markdown/XSS、窄屏 | 已启动的隔离应用和确定性上游 |
| 部署测试 | 二进制、Docker、Nginx HTTPS、重启与多实例 | 对应部署环境 |

发布验收要求：两种后端的 P0 用例全部通过；变更涉及的 P1 通过；故障恢复后可重新登录并完成新一轮聊天；所有失败有问题记录。P2 按预先填写的性能目标判定。

### 9.1 SSE 空闲超时回归

在允许 `io_uring` 的 Linux 主机上安装 Python 3 和本地 Docker，从项目根目录运行。脚本使用本机模拟模型，启动临时 MySQL 8.4 和 Redis 8.0，两种后端一起跑，按实际的 60 秒空闲阈值计时。首次运行可能需要拉取容器镜像。

```bash
cmake -S . -B build-reactor -DNET_BACKEND=reactor -DCMAKE_BUILD_TYPE=Release
cmake --build build-reactor --parallel 4
cmake -S . -B build-proactor -DNET_BACKEND=proactor -DCMAKE_BUILD_TYPE=Release
cmake --build build-proactor --parallel 4
python3 scripts/test-sse-idle-timeout.py \
  --reactor build-reactor/server --proactor build-proactor/server
```

两种后端各验证七种情况：正文流、心跳流、静默连接、半截请求、停止读取、极慢读取、模型先超时。脚本输出每个用例的结果，任一失败时返回非零。可用 `--case` 选跑，用 `--help` 查看用例名。[回归脚本](../scripts/test-sse-idle-timeout.py)

脚本先用 `server --network-backend` 核对两个二进制的编译后端，再启动临时依赖。可用 `python3 scripts/test-sse-http-framing.py -v` 单独验证回归客户端能识别 HTTP 分块截断，仅需 Python 3 和本机回环连接。[分块测试](../scripts/test-sse-http-framing.py)

## 10. 执行记录模板

| 项目 | 填写内容 |
| --- | --- |
| 执行日期、执行人 | 待填写 |
| 提交号、工作区修改摘要 | 待填写 |
| 构建目录、网络后端、构建模式 | 待填写 |
| 系统、内核、CPU/内存、浏览器及依赖版本 | 待填写 |
| 配置摘要 | 线程/队列/连接池、超时、namespace、HTTP/HTTPS；省略秘密 |
| 模型上游、SMTP | 模拟或真实、协议和故障场景 |
| 通过/失败/阻塞/未执行数量 | 待填写 |
| 缺陷与复测结论 | 待填写 |

| 用例编号 | 后端/浏览器 | 实际步骤与结果 | 状态 | 证据/问题链接 |
| --- | --- | --- | --- | --- |
| CHAT-05 | 待填写 | 待填写 | 未执行 | 待填写 |

缺陷记录包含：环境、前置数据、完整复现步骤、预期与实际结果、发生频率、关联请求/会话/消息 ID、脱敏响应与日志。截图、抓包、Cookie 文件和测试报告移除密码、验证码、会话凭据、API Key 及私人聊天内容。
