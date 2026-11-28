#!/usr/bin/env python3
"""Verify the chat API and the DeepSeek Chat Completions request on the wire."""

import argparse
from contextlib import ExitStack
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
from queue import Queue
import runpy
import threading
import uuid

SUITE = runpy.run_path(str(Path(__file__).with_name("test-sse-idle-timeout.py")))
HOST = SUITE["HOST"]
require = SUITE["require"]


class Upstream(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_error(404)
            return
        payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.calls.put(payload)
        if payload["stream"]:
            reply = (b'data: {"choices":[{"delta":{"content":"mock reply"},"finish_reason":"stop"}]}\n\n'
                     b'data: [DONE]\n\n')
            content_type = "text/event-stream"
        else:
            reply = b'{"choices":[{"message":{"content":"mock reply"},"finish_reason":"stop"}]}'
            content_type = "application/json"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)


def stream_chat(chat, body):
    connection = HTTPConnection(HOST, chat.port, timeout=5)
    try:
        connection.request("POST", "/api/chat", json.dumps({**body, "stream": True}), {
            "Cookie": chat.cookie, "X-Chat-Request": "1", "Content-Type": "application/json",
        })
        response = connection.getresponse()
        data = response.read()
        require(response.status == 200 and b"event: done" in data and b"mock reply" in data,
                f"expected completed chat stream, got HTTP {response.status}: {data!r}")
    finally:
        connection.close()


def run(binary, upstream, mysql_port, redis_port):
    models = [
        {
            "provider": provider, "model": model, "api": api,
            "base_url": f"http://{HOST}:{upstream.server_port}/v1", "api_key": "thinking-effort-regression",
        }
        for provider, model, api in [
            ("deepseek", "deepseek-flash", "openai-completions"),
            ("deepseek", "deepseek-v4-pro", "openai-completions"),
            ("deepseek", "deepseek-v4-flash", "openai-completions"),
            ("deepseek", "deepseek-reasoner", "openai-completions"),
            ("deepseek", "deepseek-v4-flash-vision-exp", "openai-responses"),
            ("local", "deepseek-flash", "openai-completions"),
        ]
    ]
    with SUITE["chat_server"](binary, upstream, mysql_port, redis_port, models=models) as chat:
        result, _ = chat.api("/api/models", 200)
        capabilities = {(entry["provider"], entry["model"]): entry.get("reasoning_efforts")
                        for entry in result["models"]}
        require(capabilities == {
            ("deepseek", "deepseek-flash"): ["high", "max"],
            ("deepseek", "deepseek-v4-pro"): ["high", "max"],
            ("deepseek", "deepseek-v4-flash"): ["high", "max"],
            ("deepseek", "deepseek-reasoner"): None,
            ("deepseek", "deepseek-v4-flash-vision-exp"): None,
            ("local", "deepseek-flash"): None,
        }, f"available thinking efforts: {result}")

        def new_chat(provider="deepseek", model="deepseek-flash", **options):
            data, _ = chat.api("/api/conversations", 201, {"title": "Thinking effort"})
            return {
                "conversation_id": data["conversation"]["id"], "request_id": uuid.uuid4().hex,
                "provider": provider, "model": model, "stream": False, "content": "Use the selected effort",
                **options,
            }

        result, _ = chat.api("/api/chat", 200, new_chat())
        require(result["reply"] == "mock reply", f"default request returned {result}")
        payload = upstream.calls.get(timeout=2)
        require(payload.get("reasoning_effort") == "high" and payload.get("thinking") == {"type": "enabled"},
                f"default requires High and enabled thinking, got {payload}")

        for effort in ("high", "max"):
            stream_chat(chat, new_chat(reasoning_effort=effort))
            payload = upstream.calls.get(timeout=2)
            require(payload.get("reasoning_effort") == effort and payload.get("thinking") == {"type": "enabled"},
                    f"thinking effort {effort} was not sent correctly: {payload}")

        invalid_requests = [new_chat(reasoning_effort=effort) for effort in ("low", "High", "", None, 42)]
        for provider, model in (("local", "deepseek-flash"), ("deepseek", "deepseek-reasoner"),
                                ("deepseek", "deepseek-v4-flash-vision-exp")):
            invalid_requests.append(new_chat(provider, model, reasoning_effort="max"))
        for request in invalid_requests:
            error, _ = chat.api("/api/chat", 400, request)
            require(error["code"] == "invalid_request", f"expected invalid_request, got {error}")
            history, _ = chat.api(f'/api/conversation?id={request["conversation_id"]}', 200)
            require(not history["messages"], f"invalid request saved chat history: {history}")
        require(upstream.calls.empty(), "invalid request reached the upstream model")

        for provider, model in (("local", "deepseek-flash"), ("deepseek", "deepseek-reasoner")):
            result, _ = chat.api("/api/chat", 200, new_chat(provider, model))
            require(result["reply"] == "mock reply", f"existing model request returned {result}")
            payload = upstream.calls.get(timeout=2)
            require("reasoning_effort" not in payload, f"existing model received an effort: {payload}")
            expected_thinking = {"type": "enabled"} if provider == "deepseek" else None
            require(payload.get("thinking") == expected_thinking, f"existing model thinking changed: {payload}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reactor", type=Path)
    parser.add_argument("--proactor", type=Path)
    args = parser.parse_args()
    binaries = [(backend, binary.resolve()) for backend, binary in vars(args).items() if binary]
    if not binaries:
        parser.error("provide at least one of --reactor and --proactor")

    with ExitStack() as stack:
        mysql_port, redis_port = SUITE["start_databases"](stack)
        upstream = stack.enter_context(ThreadingHTTPServer((HOST, 0), Upstream))
        upstream.calls = Queue()
        thread = threading.Thread(target=upstream.serve_forever)
        thread.start()
        stack.callback(thread.join)
        stack.callback(upstream.shutdown)
        for backend, binary in binaries:
            run(binary, upstream, mysql_port, redis_port)
            print(f"{backend}: model capability, default High, explicit High/Max, invalid input and existing models")


if __name__ == "__main__":
    main()
