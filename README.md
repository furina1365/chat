# 网页 AI 对话

原生 HTML/CSS/JavaScript 前端与 C++17 后端。支持选择模型、多轮文字对话、SSE 流式回复、停止生成、新建/删除会话、复制回复和重新生成。支持用户名/已验证邮箱登录、邮箱绑定和密码找回；账号与聊天历史保存在 MySQL，设备会话、验证码和共享限流保存在 Redis。

## 启动

需要 C++17 编译器、CMake 3.23+、MySQL 8、Redis，以及 libcurl、OpenSSL、MySQL Client 和 liburing 开发包。首次在 Ubuntu/Debian 上准备依赖：

```bash
sudo apt install libcurl4-openssl-dev liburing-dev libssl-dev libmysqlclient-dev mysql-client
```

curl、OpenSSL 和 MySQL Client 使用系统安装的头文件和库。MySQL 使用 [SOCI 4.1.4](https://github.com/SOCI/soci/tree/v4.1.4) 的 MySQL 后端，Redis 使用 [redis-plus-plus 1.3.15](https://github.com/sewenew/redis-plus-plus/tree/1.3.15)。CMake 首次配置时下载并校验这些库及其依赖 fmt 11.2.0、hiredis 1.3.0 的 SHA-256，静态链接到 server。启动配置和模型配置共用 nlohmann JSON v3.12.0，单头文件放在 `backend/nlohmann/json.hpp`，随项目保留，无需额外下载。

HTTP 请求由 [llhttp 9.4.3](https://github.com/nodejs/llhttp/releases/tag/release/v9.4.3) 增量解析；官方生成的 C 源码、头文件和许可证保存在 `backend/llhttp/`，随 CMake 编译，无需 Node.js 或构建时下载。

先创建 MySQL 数据库和应用账号，在 `config.json` 填写对应连接配置，再导入初始表结构。以下命令按示例数据库 `chat` 和账号 `chat` 执行：

```bash
mysql -h 127.0.0.1 -u chat -p chat < backend/db/schema.sql
```

`schema.sql` 创建 InnoDB 表、唯一约束和外键，不创建数据库或数据库用户。运行账号需要这些表的 SELECT、INSERT、UPDATE、DELETE 权限，导入结构时另需 CREATE、REFERENCES 权限。启动会检查账号及历史表是否就绪。

在项目根目录构建并启动，设置数据库密码与 `model.json` 引用的密钥（默认 `DEEPSEEK_API_KEY`）：

```bash
export CHAT_MYSQL_PASSWORD='数据库账号的密码'
export DEEPSEEK_API_KEY='模型密钥'
cmake -S . -B build
cmake --build build -j
./build/server
```

打开 [http://localhost:8080](http://localhost:8080)，同一服务提供页面和接口。Enter 发送，Shift+Enter 换行，生成时发送按钮变成停止按钮。开发时也可用 `python3 -m http.server 8000 --directory frontend` 单独提供前端，在 `auth.allowed_origins` 中加入 `http://localhost:8000`，并以相同主机名访问 8080 接口。浏览器请求会携带 Cookie。

回复支持 Markdown 标题、嵌套列表、引用、表格、任务列表、链接、图片、删除线和代码高亮。公式使用 KaTeX，支持 `$...$`、`$$...$$`、`\(...\)`、`\[...\]` 及 `math` 代码块；普通代码中的这些符号保留原样。流式回复和历史记录共用同一个渲染器。模型输出的原始 HTML 作为文字展示，生成的 HTML 经过 DOMPurify 清理。

前端目录按用途组织：

```text
frontend/
├── index.html             # 页面入口
├── favicon.svg
├── src/                   # 业务源码：app.js、styles.css、markdown.js
├── scripts/build.cjs      # Markdown 依赖打包脚本
├── vendor/                # 本地运行资源：渲染脚本、公式样式、字体和许可证
├── package.json
└── package-lock.json
```

Markdown 脚本、KaTeX 样式及字体已包含在 `frontend/vendor/`，运行无需 npm、前端构建或 CDN。只有更新渲染依赖或 `frontend/src/markdown.js` 时，才需要重新打包并提交生成的资源：

```bash
cd frontend
npm ci
npm run vendor
```

依赖版本固定在 `frontend/package-lock.json`，许可证保存在 `frontend/vendor/THIRD-PARTY-NOTICES.txt`。

程序生成在 `build/server`。程序固定读取当前工作目录下的 `config.json` 和 `model.json`，因此从项目根目录运行，不再接受端口参数。修改配置后重启生效。

`config.json` 控制监听地址、端口、线程数，以及 MySQL、Redis 的连接参数：

```json
{
  "server": {
    "host": "0.0.0.0",
    "port": 8080,
    "io_threads": 14,
    "worker_threads": 16,
    "business_threads": 4,
    "business_queue_size": 64,
    "model_queue_size": 8
  },
  "mysql": {
    "host": "127.0.0.1",
    "port": 3306,
    "database": "chat",
    "username": "",
    "password_env": "",
    "pool_size": 8,
    "acquire_timeout_ms": 1000,
    "connect_timeout_seconds": 5,
    "io_timeout_seconds": 5
  },
  "redis": {
    "host": "127.0.0.1",
    "port": 6379,
    "username": "",
    "password_env": "",
    "database": 0,
    "pool_size": 8,
    "acquire_timeout_ms": 1000,
    "connect_timeout_ms": 3000,
    "io_timeout_ms": 3000
  },
  "auth": {
    "registration_enabled": true,
    "min_password_length": 15,
    "max_password_length": 128,
    "key_namespace": "auth:account",
    "login_attempts": 10,
    "login_window_seconds": 300,
    "operation_attempts": 20,
    "operation_window_seconds": 60,
    "password_verification_seconds": 300,
    "session_namespace": "auth:session",
    "session_idle_seconds": 86400,
    "session_absolute_seconds": 604800,
    "cookie_name": "chat_session",
    "secure_cookie": false,
    "allowed_origins": []
  },
  "email": {
    "key_namespace": "auth:email",
    "code_timeout_seconds": 600,
    "resend_interval_seconds": 60,
    "max_code_attempts": 5,
    "smtp": {
      "host": "",
      "port": 465,
      "security": "tls",
      "username": "",
      "password_env": "",
      "from_address": "",
      "from_name": "Chat",
      "ca_file": "",
      "timeout_ms": 10000
    }
  }
}
```

`server` 集中配置监听地址、端口和线程数，可省略，也可只覆盖部分字段。`server.host` 默认 `0.0.0.0`，监听所有 IPv4 网卡，其他设备可通过服务器的局域网 IP 访问。设为 `127.0.0.1` 时仅允许本机 IPv4 访问；`::1` 监听 IPv6 本机地址，浏览器使用 `http://[::1]:8080`。也可填写本机主机名，绑定解析得到的第一个 IPv4 或 IPv6 地址。地址无法解析或绑定时启动失败，修改后重启生效。

项目配置为 14 个 IO 线程、16 个模型 worker。`io_threads` 为 0 时按 CPU 逻辑核数自动配置，正整数指定处理连接的 IO 线程数。这些线程之外还有一个负责接入连接的主线程。`worker_threads` 是执行模型调用的线程数，必须为正整数；账号、密码哈希、邮件和历史访问使用 `business_threads` 个独立业务线程，默认 4 个；因此服务自身线程总数为 `1 + 实际 IO 线程数 + worker_threads + business_threads`。`business_queue_size` 和 `model_queue_size` 分别限制排队任务，默认 64 和 8，满时返回 503。上述线程与队列配置对 Reactor 和 Proactor 都生效；省略字段时，端口默认 8080、IO 线程数自动计算、worker 默认 2 个。配置缺失、格式错误、未知或重复字段、端口越界及非法线程数会报错退出。

`mysql` 和 `redis` 可省略，也可只填写需要覆盖的字段，其余使用数据库配置结构中的默认值。`pool_size` 为每个服务实例的连接池上限，`acquire_timeout_ms` 为等待借用连接的毫秒数。MySQL 的连接及 IO 超时使用秒，Redis 使用毫秒；端口范围为 1–65535，连接池大小和超时必须为正整数，Redis 的 `database` 为非负整数。嵌套对象中的未知或重复字段同样会报错。

MySQL 的 `database`、`username` 按实际账号填写，示例不会创建数据库或用户。`password_env` 填保存密码的环境变量名，实际连接时读取；空字符串表示不提供密码。Redis 的 `username` 可留空。启动时建立并检查真实连接，连接池按需扩展到上限；运行期连接故障会丢弃坏连接，后续请求重新建立连接。写入结果未知时不自动重放。

Linux 上 MySQL 的 `host=localhost` 使用 Unix socket，`host=127.0.0.1` 使用 TCP。使用 `auth_socket` 认证的本机账号应配置 `localhost`，并以对应的系统用户启动服务。

MySQL 语句通过 SOCI `use()` 绑定参数；其 MySQL 后端采用客户端转义模拟绑定，并缓冲完整查询结果；建立连接时仅在当前会话移除不兼容的 `NO_BACKSLASH_ESCAPES` 模式。适配层返回最多 2000 行、累计 16 MiB，调用方仍须在 SQL 中限制范围；历史列表通过索引和游标分页。账号名、邮箱使用唯一约束，历史与账号通过外键关联。多实例连接同一数据库和 Redis 逻辑主节点，并使用相同的业务 namespace。

账号设置首页按功能提供入口，修改密码和邮箱在独立页面操作。邮箱设置先验证当前密码，通过后再显示绑定、更换与解绑；验证仅对当前登录会话有效，默认 300 秒，可通过 `auth.password_verification_seconds` 调整（1–3600 秒），过期后重新验证。“已登录设备”显示浏览器、系统、登录及最近活动时间，可单独退出某台设备，也可退出当前设备或全部设备。

`auth` 配置注册开关、密码长度、Cookie、共享限流和会话时长。登录限流按规范化后的登录标识计数，同一窗口还限制全局尝试总数为 `login_attempts × 100`。默认连续 86400 秒未使用或登录累计 604800 秒后失效；所有实例使用相同 namespace 和时长。账号密码用带随机盐的 scrypt 存储，参数为 N=32768、r=8、p=3，参见 [OWASP 密码存储参数](https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html)。登录和凭据变更锁定同一账号行，密码变更先撤销 Redis 会话再提交 MySQL；提交失败时可能需要重新登录，提交结果未知时由用户查询确认。

示例 `auth.secure_cookie=false` 用于本机 HTTP 调试；HTTPS 部署设为 `true`。允许来源填写完整的来源（协议、主机、可选端口），不含路径或通配符，默认只允许同源。Cookie 使用 HttpOnly、SameSite=Lax；前端不保存密码、会话凭据或聊天内容到 localStorage。原有匿名本地历史保留在浏览器，不自动迁入任何账号。

`email.smtp.host` 留空时邮箱功能不可用，用户名注册和登录仍可使用。配置现有邮箱 SMTP 时填写主机、端口、`security`（`tls` 或 `starttls`）、用户名、发件邮箱及 `password_env` 指向的授权码环境变量。例如设置 `password_env` 为 `CHAT_SMTP_PASSWORD` 后，在启动终端导出该环境变量。`ca_file` 留空使用系统证书；不会关闭证书验证。验证码默认 600 秒有效，60 秒后可重发，最多尝试 5 次。

首次配置默认构建 io_uring Proactor，需要允许使用 io_uring 的 Linux 内核。在 `build` 目录中可以直接切换并编译 epoll Reactor：

```bash
cd build
cmake ..
make reactor
```

切回 Proactor：

```bash
make proactor
```

两个目标都更新当前构建目录中的 `server`。运行时回到项目根目录执行 `./build/server`。

也可以通过 `cmake -S . -B build -DNET_BACKEND=reactor` 或 `-DNET_BACKEND=proactor` 配置，再执行 `cmake --build build -j`。`NET_BACKEND` 会保存在 CMake 缓存中，后续普通构建沿用上次的选择。切换发生在编译期，重新构建后需要重启服务。Proactor 初始化失败会明确报错退出。

Ctrl+C 或 SIGTERM 停止服务，运行中的模型调用会取消，工作线程随后退出。

## 模型配置

`model.json` 是模型配置数组。API 类型采用 Pi 的命名：

| `api` | 调用路径 |
| --- | --- |
| `openai-completions` | 在 `base_url` 后追加 `/chat/completions` |
| `openai-responses` | 追加 `/responses` |
| `anthropic-messages` | 追加 `/messages` |

`base_url` 必须包含需要的 API 前缀，例如 `/v1`，但不包含上述资源路径。文件里的默认项是 DeepSeek Chat Completions；可添加自己的服务商与模型。启动时加载配置，修改后需重启。

```json
[
  {
    "provider": "deepseek",
    "api": "openai-completions",
    "base_url": "https://api.deepseek.com",
    "api_key_env": "DEEPSEEK_API_KEY",
    "model": "deepseek-flash",
    "max_output_tokens": 4096,
    "temperature": null,
    "top_p": null,
    "timeout_ms": 60000,
    "connect_timeout_ms": 10000,
    "max_response_bytes": 4194304
  }
]
```

密钥只从服务端环境读取，不写入配置或返回前端。生成参数由模型配置管理，网页请求不能覆盖。`temperature`、`top_p` 及输出上限缺省或为 `null` 时不发送对应参数；Messages 必须有输出上限，因此未设置时使用 4096。Messages 的 `temperature` 和 `top_p` 只配置其中一项。

超时与大小配置必须为正整数：`timeout_ms` 从实际开始调用上游时计时，`connect_timeout_ms` 限制建立连接时间，`max_response_bytes` 限制累计接收的上游响应字节（包括 SSE 元数据，最大允许 64 MiB）。

两套后端的连接空闲上限均为 30 秒；在 `CMakeLists.txt` 中将 `set(CHAT_LOGGING ON)` 改为 `set(CHAT_LOGGING OFF)` 并重新编译后为 60 秒。从最近一次客户端读取计时，配置更长的模型超时也不能突破这一连接限制。

## 接口

`GET /api/models` 只返回已配置模型的 `provider`、`model`，不包含地址、密钥或生成参数。

先登录，再通过 `POST /api/conversations` 创建会话。聊天只提交本次问题与服务端会话 ID，上下文由后端按归属读取：

```json
{
  "conversation_id": "1",
  "request_id": "question-1",
  "provider": "deepseek",
  "model": "deepseek-flash",
  "stream": true,
  "content": "解释一下 SSE"
}
```

完整账号、历史、分页、重试和错误约定见 [接口文档](docs/api.md)。POST 需携带 `Content-Type: application/json`、`X-Chat-Request: 1` 和登录 Cookie。

- `stream` 默认 true。SSE 使用 `meta` 返回持久化消息 ID，`delta` / `reasoning_delta` 分开发送正文与思考，最终回复提交 MySQL 成功后发送 `done`；上游异常或保存失败使用 `error` 结束。普通响应包含 reply、reasoning、finish_reason 和消息 ID。
- 停止生成会取消上游调用并保存已收到的正文与思考，已成功提交的历史在重启后保留；重新生成成功前仍展示原回答。
- 一个会话同时最多一条生成任务；`request_id` 去重，重复请求不会再次调用模型。进度按递增 sequence 保存，终态不能被旧快照覆盖。生成租约 5 分钟，进度保存时续期；异常退出后的占位在过期读取时转为中断。
- 模型任务与业务任务分别使用有界队列。每条连接的流式待发送队列限制为 1 MiB，接收过慢时停止生成。
- DeepSeek Chat Completions 启用思考模式，Responses 接收公开思考摘要，Messages 接收 thinking 文字，不展示签名或加密字段。思考内容单独保存，不混入下一轮正文上下文。当前仅支持文字。

保留 `GET /api/hello`、`POST /api/echo` 和跨域 OPTIONS。请求使用 Content-Length，不支持 chunked 请求体；每个连接处理一个请求后关闭。

仅接受 HTTP/1.1 普通请求，不处理协议升级。请求头上限为 16 KiB（另计末尾四字节 CRLF 分隔符），请求体上限为 256 KiB；缺少 POST 长度返回 411、超大请求头返回 431、超大请求体返回 413，格式错误或提前断流返回 400。llhttp 使用默认严格解析，非法请求头和重复 Content-Length 会被拒绝。

## 代码

- `backend/ai/Types.hpp`：模型配置、文字消息、回复、错误和取消标记。
- `backend/ai/ModelClient.hpp`、`backend/ai/src/ModelClient.cc`：模型客户端抽象基类，共用 libcurl 传输、超时、取消和错误处理，通过虚函数调用具体协议实现。
- `backend/ai/src/protocols/`：`ChatCompletionsClient`、`ResponsesClient`、`MessagesClient` 三个协议子类，分别负责请求编码、认证、回复与流事件解析、正常结束判断。
- `backend/ai/src/ModelResponseState.hpp/.cc`：每次调用独立的回复、回调和结束状态，不在客户端中保存会话。
- `backend/ai/src/SseParser.hpp/.cc`：增量 SSE 拆包，只处理格式，不解释模型 JSON。
- `backend/ai/ModelManager.hpp`、`backend/ai/src/ModelManager.cc`：配置校验、密钥加载及客户端查找。
- `backend/ServerConfig.hpp/.cc`：读取 `config.json`，校验端口、IO/worker 线程数和 MySQL/Redis 连接配置。
- `backend/chat/Chat.hpp/.cc`：组装账号、会话、历史、数据库、HTTP 与模型服务，并管理独立的有界业务/模型线程池。
- `backend/auth/`：Account、Session、Email 的独立接口与 `src/` 实现，核心不依赖 HTTP 或聊天。
- `backend/db/`：SOCI MySQL 参数绑定与事务、redis-plus-plus 命令和有界连接池；`schema.sql` 为初始结构。
- `backend/history/`：归属检查、游标分页、分支上下文、幂等回合与有序快照保存。
- `backend/chat/src/`：账号/历史路由、Cookie、CSRF、CORS 和生成调度。
- `backend/http/Http.hpp`、`backend/http/src/Http.cc`：通用 HTTP 服务和连接生命周期，解析请求后调用业务 handler，不依赖 AI。
- `backend/http/HttpRequest.hpp`：公共请求数据，包含 method、path、headers 和 body。
- `backend/http/HttpRouter.hpp`、`backend/http/src/HttpRouter.cc`：按 method/path 分发到业务 handler。
- `backend/http/HttpResponse.hpp`、`backend/http/src/HttpResponse.cc`：普通响应、下游 SSE 编码和有界跨线程发送队列，业务通过取消标记感知连接断开。
- `backend/http/src/HttpParser.hpp/.cc`：内部 llhttp 请求解析封装，处理半包、长度限制及 EOF；不依赖网络后端。
- `backend/net/Net.hpp`：公共头文件汇总；也可按需包含根目录的 `EventLoop.hpp`、`TcpConnection.hpp`、`TcpServer.hpp`、`Buffer.hpp`、`TimerQueue.hpp`、`InetAddress.hpp` 和 `Runtime.hpp`，直接查看方法及使用约束。后端选择和实现位于 `src/`，公共类声明不随后端变化。
- `frontend/`：页面、样式和浏览器会话管理，无构建步骤或 CDN 依赖。

网络运行时按图中的公共层与两套实现组织：

```text
backend/
├── main.cc
├── nlohmann/
│   └── json.hpp                # JSON 单头文件
├── auth/                       # 账号、Redis Session、验证码与 SMTP
├── db/                         # 连接池、SOCI 参数绑定、事务与初始 SQL
├── history/                    # 服务端聊天历史与分支
├── common/                     # 随机数、摘要与共享 curl 生命周期
├── chat/                       # 网页路由与服务组合
├── http/                       # 通用 HTTP、路由和 SSE 响应
│   ├── Http.hpp                # 公共：服务和 handler
│   ├── HttpRequest.hpp         # 公共：请求数据
│   ├── HttpResponse.hpp        # 公共：普通响应和 SSE
│   ├── HttpRouter.hpp          # 公共：路由注册与匹配
│   └── src/                    # 实现文件和内部 HttpParser.hpp
├── ai/                         # 模型配置、协议与上游传输
└── net/
    ├── Net.hpp                 # 公共接口汇总
    ├── EventLoop.hpp           # 事件循环、任务投递、定时器入口
    ├── TcpConnection.hpp       # 连接操作、收发缓冲和业务扩展
    ├── TcpServer.hpp           # 连接工厂、初始化和服务循环
    ├── Buffer.hpp              # 有界字节缓冲
    ├── TimerQueue.hpp          # 定时器添加、取消和到期执行
    ├── InetAddress.hpp         # IPv4/IPv6 地址
    ├── Runtime.hpp             # 单调时钟和进程启动准备
    ├── Config.hpp              # 实现文件使用的编译期开关
    └── src/
        ├── Backend.hpp         # 内部后端选择和原生 loop 访问
        ├── EventLoop.cc / TcpConnection.cc / TcpServer.cc
        ├── Buffer.cc / TimerQueue.cc / InetAddress.cc / Runtime.cc
        ├── common/
        │   ├── Socket.hpp      # socket RAII
        │   ├── Logging.hpp / NonCopyable.hpp / Codec.hpp / Runtime.hpp
        │   ├── LoopBase.hpp    # 任务队列、线程归属和唤醒
        │   ├── LoopThreadPool.hpp
        │   └── TcpServerBase.hpp
        ├── reactor/
        │   ├── EventLoop.hpp   # epoll / eventfd / timerfd
        │   ├── Channel.hpp     # Channel 与 EPollPoller
        │   ├── BufferRead.hpp  # 非阻塞 readv
        │   ├── Acceptor.hpp
        │   └── TcpConnection.hpp
        └── proactor/
            ├── UringLoop.hpp   # 完成通知与唤醒
            ├── Uring.hpp       # ring 与在途 Operation
            ├── Acceptor.hpp
            └── TcpConnection.hpp
```

公共类通过私有实现连接所选后端，公共头文件不包含 epoll/io_uring 实现头文件。`TcpServer` 复用连接管理和线程分发；两套后端分别维护连接收发状态机，保留就绪模型与完成模型的区别。业务继续继承公共 `TcpConnection` 并实现 `HandleBusiness()`。网络实现编译为 `chat_net` 静态库，由服务链接。

每条连接的输入/输出 Buffer 各限 64 KiB，跨线程待执行队列每 loop 最多 65535 项，满载或退出后拒绝新任务。Proactor 每 loop 最多 4096 项普通在途操作，另留取消请求空间；提交失败关闭对应连接。每个在途操作持有独立缓冲，部分发送只按 CQE 返回的字节数消费输出；关闭后等原操作的 CQE 全部返回，才销毁连接。退出先停止接入、关闭连接，再处理取消完成，最后释放 loop。取消请求本身的完成不代替原操作的完成，参见 [liburing 取消语义](https://man7.org/linux/man-pages/man3/io_uring_prep_cancel.3.html)。

相较只替换 Poller，这种结构需要分别维护两套连接状态机，但不会把异步完成伪装为 fd 就绪。通过 CMake 的 `NET_BACKEND` 选择后端，重新构建并重启服务。
