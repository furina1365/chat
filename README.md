# 网页 AI 对话

原生 HTML/CSS/JavaScript 前端与 C++17 后端。支持选择模型、多轮文字对话、SSE 流式回复、停止生成、新建/删除会话、复制回复和重新生成。历史保存在当前浏览器的 `localStorage`，无需数据库和登录。

## 启动

需要 C++17 编译器、CMake 3.20+、libcurl 和 liburing 开发包。首次在 Ubuntu/Debian 上准备依赖：

```bash
sudo apt install libcurl4-openssl-dev liburing-dev
```

curl 使用系统安装的头文件和库。启动配置和模型配置共用 nlohmann JSON v3.12.0，单头文件放在 `backend/nlohmann/json.hpp`，随项目保留，无需额外下载。

HTTP 请求由 [llhttp 9.4.3](https://github.com/nodejs/llhttp/releases/tag/release/v9.4.3) 增量解析；官方生成的 C 源码、头文件和许可证保存在 `backend/llhttp/`，随 CMake 编译，无需 Node.js 或构建时下载。

在项目根目录使用 CMake 构建并启动，确保当前终端已经设置 `model.json` 所引用的密钥环境变量（默认 `DEEPSEEK_API_KEY`）：

```bash
cmake -S . -B build
cmake --build build -j
./build/server
```

打开 [http://localhost:8080](http://localhost:8080)，同一服务提供页面和接口。Enter 发送，Shift+Enter 换行，生成时发送按钮变成停止按钮。也可用 `python3 -m http.server 8000 --directory frontend` 单独在 8000 端口提供前端，接口仍连接本机 8080。

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

`config.json` 控制启动端口和线程数：

```json
{
  "port": 8080,
  "io_threads": 14,
  "worker_threads": 2
}
```

项目配置为 14 个 IO 线程、2 个模型 worker。`io_threads` 为 0 时按 CPU 逻辑核数自动配置，正整数指定处理连接的 IO 线程数。这些线程之外还有一个负责接入连接的主线程。`worker_threads` 是执行模型调用的线程数，必须为正整数；因此服务自身线程总数为 `1 + 实际 IO 线程数 + worker_threads`。这两个字段对 Reactor 和 Proactor 都生效；省略字段时，端口默认 8080、IO 线程数自动计算、worker 默认 2 个。配置缺失、格式错误、未知或重复字段、端口越界及非法线程数会报错退出。

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

`POST /api/chat`：

```json
{
  "provider": "deepseek",
  "model": "deepseek-flash",
  "stream": true,
  "messages": [
    {"role": "user", "content": "你好"},
    {"role": "assistant", "content": "你好，有什么可以帮助你？"},
    {"role": "user", "content": "解释一下 SSE"}
  ]
}
```

- `stream` 缺省为 `true`。消息按 user/assistant 交替，末条必须是 user，最多 200 条，请求体最多 256 KiB。
- 流式响应为 `text/event-stream`，使用 HTTP/1.1 chunked 编码。`delta` 事件携带正文 `{"text":"新增文字"}`；`reasoning_delta` 使用同样的数据格式单独携带上游返回的思考文字或摘要；`done` 携带 `finish_reason`、`truncated`；生成失败通过 `error` 事件携带 `{"error":"说明"}`。空闲等待期间发送 SSE 注释心跳。
- 只有识别到对应模型协议的正常结束事件才发送 `done`；中途断流不会当作成功。前端会保留已收到的部分文字并标注中断；重新生成失败或停止时保留原回复。
- `stream: false` 返回 `{"reply":"完整回复","finish_reason":"stop","truncated":false}`；上游提供思考内容时，另有 `reasoning` 字段。
- 校验失败、未配置模型、队列满等在流开始前返回对应 HTTP 错误和 JSON。上游错误说明会脱敏，不直接转发上游原始错误体。
- 用户停止时前端中止请求，后端检测连接断开后设置独立取消标记，终止上游传输。取消也在等待首个文字期间检查。
- 模型工作线程数由 `config.json` 的 `worker_threads` 指定，默认两个，最多另有八个待执行任务。网络 IO 线程只负责 HTTP 收发与路由；后台增量通过 `QueueInLoop()` 交回所属 IO 线程。每条连接的流式待发送队列限制为 1 MiB，接收过慢时停止生成。
- DeepSeek Chat Completions 启用思考模式。支持接收 Chat Completions 的 `reasoning_content`、Responses 的公开思考摘要和 Messages 的 `thinking` 文字；不会展示签名或加密字段。前端在正文上方折叠展示，支持生成中展开及本地历史查看；上游未返回时不显示该区域。Responses/Messages 是否产生这些内容取决于上游模型与服务配置。
- 思考内容不混入回答或下一轮的正文历史。仅收到思考就停止的一轮仍保存在浏览器中，但不作为已完成问答传给模型；重新生成会重新使用原问题。当前只支持文字，不接入工具调用、图片或文件。

保留 `GET /api/hello`、`POST /api/echo` 和跨域 OPTIONS。请求使用 Content-Length，不支持 chunked 请求体；每个连接处理一个请求后关闭。

仅接受 HTTP/1.1 普通请求，不处理协议升级。请求头上限为 16 KiB（另计末尾四字节 CRLF 分隔符），请求体上限为 256 KiB；缺少 POST 长度返回 411、超大请求头返回 431、超大请求体返回 413，格式错误或提前断流返回 400。llhttp 使用默认严格解析，非法请求头和重复 Content-Length 会被拒绝。

## 代码

- `backend/ai/Types.hpp`：模型配置、文字消息、回复、错误和取消标记。
- `backend/ai/ModelClient.hpp`、`backend/ai/src/ModelClient.cc`：模型客户端抽象基类，共用 libcurl 传输、超时、取消和错误处理，通过虚函数调用具体协议实现。
- `backend/ai/src/protocols/`：`ChatCompletionsClient`、`ResponsesClient`、`MessagesClient` 三个协议子类，分别负责请求编码、认证、回复与流事件解析、正常结束判断。
- `backend/ai/src/ModelResponseState.hpp/.cc`：每次调用独立的回复、回调和结束状态，不在客户端中保存会话。
- `backend/ai/src/SseParser.hpp/.cc`：增量 SSE 拆包，只处理格式，不解释模型 JSON。
- `backend/ai/ModelManager.hpp`、`backend/ai/src/ModelManager.cc`：配置校验、密钥加载及客户端查找。
- `backend/ServerConfig.hpp/.cc`：读取 `config.json`，校验端口和 IO/worker 线程数。
- `backend/chat/Chat.hpp/.cc`：组合 HTTP 与 AI，注册网页/API 路由，校验聊天请求，通过有界 worker 调用模型，并映射普通回复、正文/思考 SSE 与错误；退出时取消并等待模型任务。
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
├── chat/                       # 网页路由、聊天调度与 HTTP/AI 组合
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
