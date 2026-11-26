# 【测试】网页 AI 对话 - chat - SSE 空闲超时回归 · AI 执行与复查

基线 `e6a4ec2`，已合入 `0862672`。本次为 [#5](https://github.com/furina1365/chat/issues/5) 增加两种网络后端的可重复回归。[用户审核文档](sse-idle-regression.md)

## 调用链与改动落点

```text
TCP 接入 -> HttpConnection::HandleBusiness / HttpParser::Feed (backend/http/src/Http.cc:48)
  -> POST /api/chat / 模型线程队列 (backend/core/src/ChatRoutes.cc:179,239)
     -> Generate -> ModelClient::Stream -> curl 超时 (backend/core/src/ChatRoutes.cc:26,63; backend/ai/src/ModelClient.cc:165)
     -> SendEvent(delta) 或 History::Finish -> SendEvent(done/error) (backend/core/src/ChatRoutes.cc:59,88,99,106)
  -> HttpConnection::Tick -> Heartbeat -> ": ping" (backend/http/src/Http.cc:81; backend/http/src/HttpResponse.cc:84)
  -> HttpResponse::Push -> HTTP/网络输出队列 (backend/http/src/HttpResponse.cc:25; backend/http/src/Http.cc:95)
     -> Reactor send 成功 / Proactor 正数发送完成 (backend/net/src/reactor/TcpConnection.hpp:234; backend/net/src/proactor/TcpConnection.hpp:117)
     -> 刷新 last_active_ -> 到期复核并关闭或重排 (backend/net/src/reactor/TcpConnection.hpp:44; backend/net/src/proactor/TcpConnection.hpp:30)
```

测试从回环地址连接正式 `server`，通过 HTTP、SSE、历史查询和 TCP 套接字观察结果。模拟模型从进程外返回协议数据，MySQL 和 Redis 用临时实例。Linux TCP 队列用来确认停读引起真实写阻塞，服务进程释放该套接字用来判定回收时点。[HTTP 调度](../../backend/http/src/Http.cc#L48) · [模型任务](../../backend/core/src/ChatRoutes.cc#L239) · [Reactor](../../backend/net/src/reactor/TcpConnection.hpp#L234) · [Proactor](../../backend/net/src/proactor/TcpConnection.hpp#L117)

## 因素测试矩阵

| 因素 | 场景与输入 | 观察点 | 预期 | 红 → 绿 |
| --- | --- | --- | --- | --- |
| 正文持续写 | 定期发送带序号的正文，模拟模型在 60 秒后正常完成 | SSE 正文、结束事件和 chunked 结尾 | 正文跨过阈值，完整收到 `done` | 60 秒截断 → 17 个 `delta` 和 `done` |
| 心跳持续写 | 上游在 60 秒后首次给正文并正常完成 | 下游阈值前后均有 `: ping`、结尾 `done` | 靠服务端心跳撑过阈值 | 60 秒截断 → 64 次心跳和 `done` |
| 真正静默 | TCP 建连后不发请求 | 客户端 EOF 和单调时钟 | 约 60 秒回收 | 56 秒 → 60 秒 |
| 最后一次读，隐藏 | 请求分两次送达，正文仍缺字节，写端保持打开 | 从第二次发送到客户端 EOF 的时间 | 阈值从最后一段输入重新计算 | 52 秒 → 60 秒 |
| 停读与写阻塞，隐藏 | 小接收窗口读一小段后停止，等待发送队列堵住并保持模拟输入 | 发送队列、未写出正文、服务端套接字释放时刻 | 写出停滞且仍有数据入队时，约 60 秒释放连接 | 到观察上限仍未回收 → 约 59.7 秒回收 |
| 极慢读取，隐藏 | 同样堵住发送队列，客户端每秒读一个字节 | 读取次数、TCP 零窗口、发送队列、释放时刻 | 取走零星字节且未恢复服务端写入时，约 60 秒释放连接 | 到观察上限仍未回收 → 约 59.7 秒回收，读到 58 字节 |
| 模型先超时，隐藏 | 模型总超时 3 秒，先给一段正文并保持上游连接打开 | SSE `error`、完整 HTTP 结尾、聊天历史 | 保留部分回复，状态 `failed`，释放会话生成占用 | 9.03 秒 → 3.05 / 3.07 秒 |
| 并行连接和后端差异，隐藏 | 两个后端都运行同一组并发场景 | 每条连接各自观测 | 持续写入的连接与静默、堵塞的连接各自计时 | 上述对照失败与并发全量通过覆盖 |

## 验收步骤

在允许 `io_uring` 的 Linux 主机按测试手册构建两套 Release `server`，用 `python3 scripts/test-sse-idle-timeout.py --reactor build-reactor/server --proactor build-proactor/server` 跑全量矩阵。脚本为本次运行启动 Docker MySQL、Redis 和本地模拟模型，从实际 `server` 入口注册账号、建会话和发请求。两个后端分别使用 2 个 I/O 线程、2 个业务线程和 6 个模型线程。[测试手册](../testing.md#91-sse-空闲超时回归) · [回归脚本](../../scripts/test-sse-idle-timeout.py)

红例用独立的临时二进制，每组都在两个后端运行。可在临时源码副本中做同样的改动，分别构建后把两个二进制传给同一脚本。

| 对照修改 | 选跑参数 | 两个后端的退出情况 |
| --- | --- | --- |
| 仅移除成功写出后的 `last_active_` 更新，保留 PR #4 的定时重排 | `--case body --case heartbeat` | 退出 1，4 项失败 |
| 建连初始活动时间提前 4 秒，移除成功读取时的更新；模型总超时额外增加 6 秒 | `--case silent --case partial --case model-timeout` | 退出 1，6 项失败 |
| 在 Reactor `FlushOut` 和 Proactor `Send` 入口增加“输出缓冲有数据时刷新活动时间” | `--case backpressure --case slow-reader` | 退出 1，4 项失败 |

## 方案偏差与返工记录

- 将原定的停读检查扩展为停读和每秒仅取 1 字节两条用例。两条都观测 TCP 零窗口、发送队列无进展、模拟模型继续输出和套接字回收。后者多检查实际读到的字节数。
- 第一轮读取 SSE 使用 Python `HTTPResponse.readline()`。对照中它把部分缺失 HTTP 最后一块的响应按 EOF 返回。改用 `read1()` 解码分块，再按行拼 SSE，截断时报告关闭时点。

## 实际结果

Linux `7.0.0-44-generic`，Python `3.14.4`，CMake `4.2.3`，Docker `29.10.1`，MySQL `8.4.13`，Redis `8.0.6`。基线为 `e6a4ec2`，本机用已有的 CMake 依赖源构建两套 Release 服务。

```bash
cmake --build .scratch/issue-5/build-reactor --target server -j 3
cmake --build .scratch/issue-5/build-proactor --target server -j 3
python3 scripts/test-sse-idle-timeout.py \
  --reactor .scratch/issue-5/build-reactor/server \
  --proactor .scratch/issue-5/build-proactor/server
```

全量退出 0，共 14 项通过。慢读和停读从观测到 TCP 零窗口且应用仍有待写数据时计时。四条连接的内核发送队列在观察期均保持 1,792,000 字节，模拟模型持续输出到回收前。

| 场景 | Reactor | Proactor |
| --- | --- | --- |
| 正文流 | `done` 64.08 秒 | `done` 64.09 秒 |
| 心跳流 | `done` 64.07 秒，64 次心跳 | `done` 64.08 秒，64 次心跳 |
| 静默、半截请求 | 均在 60.00 秒 EOF | 均在 60.00 秒 EOF |
| 停读 | 59.70 秒释放套接字 | 59.73 秒释放套接字 |
| 每秒读 1 字节 | 59.70 秒释放套接字 | 59.72 秒释放套接字 |
| 模型先超时 | 3.05 秒返回 `error` | 3.07 秒返回 `error` |

模型超时的两条请求都收到了部分正文和 SSE `error`，HTTP 分块正常结束。历史接口返回 `failed` 和部分正文，`active_message_id` 为 `null`。验证范围为 Linux 回环、真实聊天服务和模拟模型。客户端主动停止或断开由 [#3](https://github.com/furina1365/chat/issues/3) 跟踪。

## 使用的技能

[implement-and-document](../../.agents/skills/implement-and-document/SKILL.md) · [trace-change-code](../../.agents/skills/trace-change-code/SKILL.md) · [plan-change](../../.agents/skills/plan-change/SKILL.md) · [execute-change](../../.agents/skills/execute-change/SKILL.md) · [tdd](../../.agents/skills/implement-and-document/references/skills/tdd/SKILL.md) · [systematic-debugging](../../.agents/skills/implement-and-document/references/skills/systematic-debugging/SKILL.md) · [verification-before-completion](../../.agents/skills/implement-and-document/references/skills/verification-before-completion/SKILL.md)
