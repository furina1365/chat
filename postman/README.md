# Postman 接口回归

[chat 集合](collections/chat.postman_collection.json) 对照当前 `docs/api.md` 和后端路由维护。更新日期：2026-11-11。

在 Postman 中导入文件，替换旧的 `chat` 集合。设置 `base_url`（默认 `http://localhost:53411`，不带末尾 `/`），按完整集合顺序运行。首个请求初始化本轮随机账号；后续请求自动传递 Cookie、对话 ID、消息 ID 和分页游标。运行状态使用 `pm.variables`，不会把登录凭据保存进集合变量。

| 文件夹 | 覆盖范围 |
| --- | --- |
| 01 公共路由与访问保护 | 首页、回显、配置文件保护、CORS、匿名鉴权、CSRF |
| 02 注册登录与账号设置 | 用户名与密码边界、登录、改用户名、重新验证密码、找回统一响应 |
| 03 对话与参数校验 | 模型信息、创建与改名、对话分页、非法 JSON、旧参数拒绝 |
| 04 聊天持久化、幂等与回复版本 | JSON、SSE、默认流式、已完成请求重放、历史一致性、重新生成、分支恢复、消息分页、上下文 |
| 05 账号隔离与设备会话 | 跨账号读取与写入、设备列表、撤销其他设备 |
| 06 邮箱绑定与找回 | 发送验证码、绑定、邮箱登录、会话验证隔离、重置密码、解绑 |
| 07 修改密码、清理与退出全部设备 | 删除测试对话、改密撤销会话、退出全部设备、账号隔离、当前设备退出 |

使用独立测试数据库与 Redis。测试配置开放注册，使用默认密码长度 8–128 字符；本机 HTTP 设置 `auth.secure_cookie=false`，并将 `auth.operation_attempts`、`auth.login_attempts` 调高至 `1000`，避免整组回归触发业务限流。HTTPS 时 Cookie 和同源预检按 HTTPS 配置运行。

集合包含 145 个请求。邮箱文件夹需要 `test_mail_url`，未设置时跳过其中 13 个请求。聊天请求自动选择 `/api/models` 的第一个模型，成功路径共生成四次回复；其中幂等重试应重放已有回复。运行结束删除本轮两个测试对话并退出测试账号；保留注册的测试账号。中途失败时按响应记录清理已创建的测试数据。

## 命令行运行

使用独立的 Postman CLI（不是 Postman 桌面程序）：

```bash
postman collection run postman/collections/chat.postman_collection.json \
  --env-var base_url=http://localhost:53411 \
  --no-report-events --timeout-request 120000 --timeout-script 10000 \
  --output checks/artifacts/postman/manual
```

`--no-report-events` 将运行报告保留在本地。生成的报告包含本轮测试数据和 Cookie，留在被 Git 忽略的 `checks/artifacts/`。

## 本地隔离验收

本项目已有 `checks/auth_support.py` 和 `checks/integration.py` 时，可以复用其 MySQL/Redis、TLS SMTP、模型模拟服务。`checks/postman_integration.py` 会启动临时服务，运行集合，确认模型上游恰好调用四次，结束后清理临时数据库、容器和进程。

```bash
CHAT_SERVER=build-binary/server python3 checks/postman_integration.py \
  --postman /实际路径/postman \
  --output checks/artifacts/postman/reactor

CHAT_SERVER=build/server python3 checks/postman_integration.py \
  --postman /实际路径/postman \
  --output checks/artifacts/postman/proactor
```

需要 Docker，或本机已有可访问的 MySQL 和 `redis-server`。`CHAT_SERVER` 指向对应网络后端的最新构建。加 `--no-mail` 可以验证未配置收件桥时的默认运行。

测试收件桥只绑定 `127.0.0.1`，`GET /code?email=...` 从本次临时 SMTP 收件端取出新邮件中的六位验证码，返回 `{"code":"123456"}`。它只接受 `pm_` 前缀且以 `@example.test` 结尾的测试邮箱；业务服务无需增加接口。集合通过 `pm.sendRequest` 取码，再执行绑定或重置。不要将真实邮箱收件端接入这个测试桥。

接口完整测试计划见 [docs/testing.md](../docs/testing.md)。

## 本次验证

在当前源码构建的 C++ 服务、隔离 MySQL/Redis、TLS SMTP 收件端和本地模型模拟上游上执行，Postman CLI 1.87.0：

| 模式 | 集合请求 | 断言 | 结果 |
| --- | --- | --- | --- |
| Reactor，含邮箱流程 | 145（另有 2 次测试取码请求） | 308 | 全部通过，退出码 0 |
| Proactor，含邮箱流程 | 145（另有 2 次测试取码请求） | 308 | 全部通过，退出码 0 |
| Reactor，未配置收件桥 | 132，跳过 13 个邮箱请求 | 278 | 全部通过，退出码 0 |

三次运行均确认模型上游调用恰好四次。报告分别在 `checks/artifacts/postman/reactor-final/`、`checks/artifacts/postman/proactor/`、`checks/artifacts/postman/default/`。这些结果验证本地服务与模拟上游的交互；真实模型和真实邮件投递需另外联调。
