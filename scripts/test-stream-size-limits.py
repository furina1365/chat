#!/usr/bin/env python3
"""Verify model response size limits via the chat and history APIs."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import runpy
import subprocess
import threading
import uuid


SUITE = runpy.run_path(str(Path(__file__).with_name("test-sse-idle-timeout.py")))
HOST = SUITE["HOST"]
require = SUITE["require"]
MIB = 1024 * 1024
DONE = b'data: {"choices":[{"delta":{},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n'
REASONING_PART = "思" * 21845 + "."  # Exactly 64 KiB of UTF-8.
CONTENT_PART = "a" * 65536


def delta(**text):
    return event({"choices": [{"delta": text}]})


def event(data):
    return b"data: " + json.dumps(data, ensure_ascii=False).encode() + b"\n\n"


class Model(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        case = request["messages"][-1]["content"]
        require(self.path == "/v1/chat/completions", "unexpected model endpoint")
        if not request["stream"]:
            require(case in {"json-limit", "json-within-limit"}, "unexpected JSON model request")
            self.json_reply(1024 if case == "json-limit" else 0)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        try:
            if case == "many-small-events":
                payload = event({"choices": [{"delta": {"content": "a"}}], "metadata": "." * 4096})
                for _ in range(1024):
                    self.wfile.write(payload)
                self.wfile.write(DONE)
            elif case == "raw-limit":
                # One upstream write crosses the cap after a complete delta.
                self.wfile.write(delta(content="accepted") + (b":" + b"." * 98 + b"\n") * 8
                                 + delta(content="outside budget") + DONE)
            elif case == "event-limit":
                self.wfile.write(delta(content="accepted") + delta(reasoning_content="also accepted"))
                # The accumulated event exceeds the cap, though each line fits.
                self.wfile.write(b'event: message\ndata: {"metadata": ["' + b"a" * 130 + b'",\n'
                                 b'data: "' + b"b" * 130 + b'"],\n'
                                 b'data: "choices":[{"delta":{"content":"outside budget"}}]}\n\n' + DONE)
            elif case == "text-limit":
                for key, text in (("reasoning_content", REASONING_PART), ("content", CONTENT_PART)):
                    payload = delta(**{key: text})
                    for _ in range(32):
                        self.wfile.write(payload)
                self.wfile.write(delta(content="界") + DONE)
            elif case == "refusal-at-text-limit":
                payload = delta(reasoning_content=REASONING_PART)
                for _ in range(64):
                    self.wfile.write(payload)
                self.wfile.write(b'data: {"choices":[{"delta":{},"finish_reason":"content_filter"}]}\n\n'
                                 b'data: [DONE]\n\n')
            else:
                raise ValueError(f"unknown test case: {case}")
        except (BrokenPipeError, ConnectionResetError):
            pass

    def json_reply(self, padding):
        body = json.dumps({"choices": [{"message": {"content": "reply"}, "finish_reason": "stop"}],
                           "metadata": "." * padding}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


@contextmanager
def model_server():
    with ThreadingHTTPServer((HOST, 0), Model) as server:
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
        thread.start()
        try:
            yield server
        finally:
            server.shutdown()
            thread.join()


def check_stream(chat, case, content, reasoning, *, error=None, finish_reason="stop"):
    with chat.stream(case) as (conversation, _, wire, started):
        events = [(frame["event"], json.loads(frame["data"]))
                  for _, frame in SUITE["frames"](wire, started) if "event" in frame]
    kinds = [kind for kind, _ in events]
    terminal = "error" if error else "done"
    require(kinds[:1] == ["meta"] and kinds[-1:] == [terminal] and
            all(kind in {"delta", "reasoning_delta"} for kind in kinds[1:-1]),
            f"expected meta, deltas and {terminal}; got {kinds[:3]} ... {events[-1:]}")
    for name, expected in (("delta", content), ("reasoning_delta", reasoning)):
        actual = "".join(data["text"] for kind, data in events if kind == name)
        require(actual == expected, f"unexpected streamed {name} ({len(actual)} characters)")
    final = events[-1][1]
    if error:
        require(final == {"code": "generation_failed", "error": error}, f"wrong size error: {final}")
    else:
        require(final["finish_reason"] == finish_reason and not final["truncated"], f"wrong done: {final}")
    data, _ = chat.api(f"/api/conversation?id={conversation}", 200)
    assistant_id = events[0][1]["assistant_message_id"]
    reply = next(item for item in data["messages"] if item["id"] == assistant_id)
    require(data["conversation"]["active_message_id"] is None, "generation slot was not released")
    require(reply["state"] == ("failed" if error else "completed") and reply["error"] == (error or ""),
            f"unexpected history state {reply['state']!r} and error {reply['error']!r}")
    if not error:
        require(reply["finish_reason"] == finish_reason, f"wrong saved finish reason {reply['finish_reason']!r}")
    require(reply["content"] == content and reply["reasoning"] == reasoning, "saved reply differs from the stream")


def many_small_events(chat):
    check_stream(chat, "many-small-events", "a" * 1024, "")


def raw_limit(chat):
    check_stream(chat, "raw-limit", "accepted", "", error="模型 SSE 流超过大小限制。")


def event_limit(chat):
    check_stream(chat, "event-limit", "accepted", "also accepted", error="模型 SSE 事件超过大小限制。")


def text_limit(chat):
    check_stream(chat, "text-limit", CONTENT_PART * 32, REASONING_PART * 32,
                 error="模型回复正文与思考合计超过大小限制。")


def refusal_at_text_limit(chat):
    check_stream(chat, "refusal-at-text-limit", "", REASONING_PART * 64, finish_reason="refusal")


def json_limit(chat):
    for case, status in (("json-within-limit", 200), ("json-limit", 502)):
        data, _ = chat.api("/api/conversations", 201, {"title": case})
        conversation = data["conversation"]["id"]
        result, _ = chat.api("/api/chat", status, {
            "conversation_id": conversation, "request_id": uuid.uuid4().hex,
            "provider": "local-test", "model": "long-timeout", "stream": False, "content": case,
        })
        data, _ = chat.api(f"/api/conversation?id={conversation}", 200)
        reply = data["messages"][-1]
        require(data["conversation"]["active_message_id"] is None, "generation slot was not released")
        if status == 200:
            require(result["reply"] == reply["content"] == "reply" and reply["state"] == "completed",
                    "ordinary JSON within its own limit did not complete")
        else:
            message = "模型响应超过大小限制。"
            require(result == {"code": "generation_failed", "error": message}, f"wrong JSON limit error: {result}")
            require(reply["state"] == "failed" and reply["error"] == message and reply["content"] == "",
                    "ordinary JSON over its own limit did not produce a saved failure")


CASES = {
    "many-small-events": ({"max_response_bytes": 4 * MIB}, many_small_events),
    "raw-limit": ({"max_stream_bytes": 512}, raw_limit),
    "event-limit": ({"max_sse_event_bytes": 256}, event_limit),
    "text-limit": ({}, text_limit),
    "refusal-at-text-limit": ({}, refusal_at_text_limit),
    "json-limit": ({"max_response_bytes": 1024, "max_stream_bytes": 64, "max_sse_event_bytes": 64}, json_limit),
}


def run_backend(name, binary, model, ports, cases):
    failures = 0
    for case in cases:
        options, check = CASES[case]
        try:
            with SUITE["chat_server"](binary, model, *ports, model_options=options) as chat:
                check(chat)
            print(f"PASS {name}/{case}", flush=True)
        except Exception as error:
            print(f"FAIL {name}/{case}: {error}", flush=True)
            failures += 1
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reactor", type=Path, required=True)
    parser.add_argument("--proactor", type=Path, required=True)
    parser.add_argument("--case", action="append", choices=CASES, help="run only this case (repeatable)")
    args = parser.parse_args()
    backends = {name: getattr(args, name).resolve() for name in ("reactor", "proactor")}
    for name, binary in backends.items():
        try:
            result = subprocess.run([binary, "--network-backend"], capture_output=True, text=True, check=True, timeout=5)
        except (OSError, subprocess.SubprocessError):
            parser.error(f"cannot query the network backend of {binary}")
        if result.stdout.strip() != name:
            parser.error(f"--{name} expects a {name} binary, got {result.stdout.strip()!r}")
    cases = tuple(dict.fromkeys(args.case)) if args.case else CASES
    with ExitStack() as stack, model_server() as model, ThreadPoolExecutor(max_workers=2) as pool:
        ports = SUITE["start_databases"](stack)
        futures = [pool.submit(run_backend, name, binary, model, ports, cases) for name, binary in backends.items()]
        failures = sum(future.result() for future in futures)
    print(f"{2 * len(cases) - failures} passed, {failures} failed")
    return int(failures > 0)


if __name__ == "__main__":
    raise SystemExit(main())
