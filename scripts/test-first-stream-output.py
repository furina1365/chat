#!/usr/bin/env python3
"""Verify streamed output can be recovered through the conversation API."""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import http.client
import json
from pathlib import Path
import runpy
import select
import socket
import subprocess
import threading
import time
import uuid


DISCONNECT = runpy.run_path(str(Path(__file__).with_name("test-client-disconnect-cancel.py")))
SUITE = DISCONNECT["SUITE"]
HOST = SUITE["HOST"]
require = SUITE["require"]
wait_for = SUITE["wait_for"]
FIRST_TEXT = "first fragment"


class Model(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        question = request["messages"][-1]["content"]
        case = question.partition(":")[0]
        if self.path != "/v1/chat/completions" or not request["stream"] or case not in CASES:
            self.send_error(400)
            return
        with self.server.calls_lock:
            self.server.calls[question] += 1
            attempt = self.server.calls[question]
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            if case == "short-body":
                self.send_header("Content-Length", "4096")
            self.end_headers()
            field = "reasoning_content" if case.endswith("reasoning") else "content"
            if case.startswith("first-"):
                self.delta({field: ""})
                self.delta({field: FIRST_TEXT})
                self.wait_for_disconnect()
            elif case.startswith("length-"):
                self.delta({field: FIRST_TEXT})
                self.end_stream("length")
            elif case in ("unexpected-end", "short-body"):
                self.delta({field: FIRST_TEXT})
                self.send('{"choices":[{"delta":{},"finish_reason":"length"}]}')
            elif case == "zero-output" and attempt == 1:
                self.send('{"error":{"message":"mock failure"}}')
            elif case == "zero-output" or attempt == 1:
                self.delta({"content": "saved answer"})
                self.end_stream("stop")
            else:
                self.delta({"content": "alternate fragment"})
                if case == "regenerate-failed":
                    self.send('{"error":{"message":"mock failure"}}')
                else:
                    self.wait_for_disconnect()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def send(self, payload):
        self.wfile.write(f"data: {payload}\n\n".encode())

    def delta(self, delta):
        self.send(json.dumps({"choices": [{"delta": delta}]}))

    def end_stream(self, reason):
        self.send(json.dumps({"choices": [{"delta": {}, "finish_reason": reason}]}))
        self.send("[DONE]")

    def wait_for_disconnect(self):
        while not self.server.stopping.is_set():
            if select.select([self.connection], [], [], 0.02)[0] and not self.connection.recv(1):
                return


@contextmanager
def model_server():
    with ThreadingHTTPServer((HOST, 0), Model) as server:
        server.stopping = threading.Event()
        server.calls = Counter()
        server.calls_lock = threading.Lock()
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
        thread.start()
        try:
            yield server
        finally:
            server.stopping.set()
            server.shutdown()
            thread.join()


def history(chat, conversation):
    return chat.api(f"/api/conversation?id={conversation}", 200)[0]


def settled_history(chat, conversation):
    def finished():
        data = history(chat, conversation)
        return data if data["conversation"]["active_message_id"] is None else None
    return wait_for("history to record the terminal state", 3, finished)


@contextmanager
def stream(chat, case, *, conversation=None, question_id=None):
    if conversation is None:
        data, _ = chat.api("/api/conversations", 201, {"title": case})
        conversation = data["conversation"]["id"]
    request = {
        "conversation_id": conversation, "request_id": uuid.uuid4().hex,
        "provider": "local-test", "model": "long-timeout", "stream": True,
    }
    if question_id is None:
        request["content"] = case
    else:
        request["regenerate_message_id"] = question_id
    with socket.create_connection((HOST, chat.port), timeout=5) as peer:
        started = time.monotonic()
        DISCONNECT["send_chat"](chat, peer, request)
        with peer.makefile("rb") as wire:
            status = wire.readline()
            headers = http.client.parse_headers(wire)
            require(status.startswith(b"HTTP/1.1 200 ") and
                    headers.get("Content-Type", "").startswith("text/event-stream"),
                    f"expected HTTP 200 with SSE, got {status!r} {headers}")
            events = ((frame["event"], json.loads(frame["data"]))
                      for _, frame in SUITE["frames"](wire, started) if "event" in frame)
            yield conversation, events


def first_output(chat, case):
    event = "reasoning_delta" if case == "first-reasoning" else "delta"
    field = "reasoning" if case == "first-reasoning" else "content"
    with stream(chat, case) as (conversation, events):
        meta_name, meta = next(events)
        require(meta_name == "meta", f"first event was {meta_name}")
        require(next(events) == (event, {"text": FIRST_TEXT}), f"expected first {event}")
        in_progress = history(chat, conversation)["messages"][-1]
        require(in_progress["id"] == meta["assistant_message_id"], "unexpected in-progress message ID")
        require(in_progress[field] == FIRST_TEXT,
                f"received {event}, but history has {in_progress[field]!r}")
    finished = settled_history(chat, conversation)["messages"][-1]
    require(finished["state"] == "interrupted" and finished[field] == FIRST_TEXT,
            f"reopened conversation lost the first {field}: {finished}")


def length_ending(chat, case):
    reasoning = case == "length-reasoning"
    with stream(chat, case) as (conversation, events):
        require(next(events)[0] == "meta", "missing meta")
        require(next(events) == ("reasoning_delta" if reasoning else "delta", {"text": FIRST_TEXT}),
                "missing first fragment")
        terminal, payload = next(events)
        saved = history(chat, conversation)
        require(saved["conversation"]["active_message_id"] is None,
                "terminal event arrived before history committed")
        answer = saved["messages"][-1]
        if reasoning:
            require(terminal == "error" and "长度上限" in payload["error"],
                    f"expected a length-limit error, got {terminal}: {payload}")
            require(answer["state"] == "failed" and answer["reasoning"] == FIRST_TEXT
                    and answer["error"] == payload["error"] and not answer["content"],
                    f"lost the only reasoning fragment or failure description: {answer}")
        else:
            require(terminal == "done" and payload["finish_reason"] == "length" and payload["truncated"],
                    f"expected truncated completion, got {terminal}: {payload}")
            require(answer["state"] == "completed" and answer["content"] == FIRST_TEXT,
                    f"truncated content did not survive in history: {answer}")
        require(answer["finish_reason"] == "length" and answer["truncated"],
                f"history lost the length marker: {answer}")
        require(list(events) == [], "received another event after the terminal event")


def unexpected_end(chat, case):
    with stream(chat, case) as (conversation, events):
        require(next(events)[0] == "meta", "missing meta")
        require(next(events) == ("delta", {"text": FIRST_TEXT}), "missing partial content")
        terminal, payload = next(events)
        require(terminal == "error" and payload["error"] == "模型流意外中断，请重新生成。",
                f"expected a stream interruption, got {terminal}: {payload}")
        answer = history(chat, conversation)["messages"][-1]
        require(answer["state"] == "failed" and answer["content"] == FIRST_TEXT
                and answer["error"] == payload["error"], f"history lost the interrupted reply: {answer}")
        require(answer["finish_reason"] == "length",
                f"history lost the reason announced before interruption: {answer}")


def zero_output(chat, case):
    with stream(chat, case) as (conversation, events):
        frames = list(events)
        require([event for event, _ in frames] == ["meta", "error"], f"unexpected events {frames}")
    reopened = history(chat, conversation)
    question, failed = reopened["messages"]
    listed, _ = chat.api("/api/conversations", 200)
    require(any(item["id"] == conversation for item in listed["items"]), "accepted conversation missing from history")
    require(question["content"] == case and question["id"] == frames[0][1]["user_message_id"],
            f"accepted question disappeared: {question}")
    require(failed["state"] == "failed" and failed["error"] == frames[-1][1]["error"]
            and not failed["content"] and not failed["reasoning"], f"zero-output error disappeared: {failed}")
    with stream(chat, case, conversation=conversation, question_id=question["id"]) as (_, events):
        frames = list(events)
        require([event for event, _ in frames] == ["meta", "delta", "done"],
                f"regenerate from the saved question did not complete: {frames}")
    saved_question, answer = history(chat, conversation)["messages"]
    require(saved_question["id"] == question["id"] and answer["content"] == "saved answer",
            "regenerate did not answer the original question")


def unsuccessful_regenerate(chat, case):
    with stream(chat, case) as (conversation, events):
        require([event for event, _ in events] == ["meta", "delta", "done"], "initial answer did not complete")
    question, original = history(chat, conversation)["messages"]
    with stream(chat, case, conversation=conversation, question_id=question["id"]) as (_, events):
        event, meta = next(events)
        require(event == "meta" and meta["user_message_id"] == question["id"],
                "regenerate used a different question")
        require(next(events) == ("delta", {"text": "alternate fragment"}), "missing alternate fragment")
        if case == "regenerate-failed":
            require(next(events)[0] == "error", "regenerate did not report failure")
    settled = settled_history(chat, conversation)
    require(settled["conversation"]["current_message_id"] == original["id"]
            and settled["messages"][-1]["content"] == "saved answer",
            "an unsuccessful regenerate replaced the selected answer")
    data, _ = chat.api(f"/api/conversation/messages?conversation_id={conversation}", 200)
    alternate = next(item for item in data["items"] if item["id"] == meta["assistant_message_id"])
    expected_state = "failed" if case == "regenerate-failed" else "interrupted"
    require(alternate["state"] == expected_state and alternate["content"] == "alternate fragment",
            f"alternate fragment did not survive as its own reply: {alternate}")


CASES = {
    "first-reasoning": first_output,
    "first-content": first_output,
    "length-content": length_ending,
    "length-reasoning": length_ending,
    "unexpected-end": unexpected_end,
    "short-body": unexpected_end,
    "zero-output": zero_output,
    "regenerate-failed": unsuccessful_regenerate,
    "regenerate-stopped": unsuccessful_regenerate,
}


def run_backend(name, binary, ports, cases):
    failures = 0
    with model_server() as model, SUITE["chat_server"](binary, model, *ports) as chat:
        for case in cases:
            try:
                CASES[case](chat, case)
            except Exception as error:
                print(f"FAIL {name}/{case}: {error}", flush=True)
                failures += 1
            else:
                print(f"PASS {name}/{case}", flush=True)
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reactor", type=Path, required=True)
    parser.add_argument("--proactor", type=Path, required=True)
    parser.add_argument("--case", action="append", choices=CASES)
    args = parser.parse_args()
    backends = {name: getattr(args, name).resolve() for name in ("reactor", "proactor")}
    for name, binary in backends.items():
        actual = subprocess.check_output([binary, "--network-backend"], text=True, timeout=5).strip()
        require(actual == name, f"--{name} received a {actual} binary")
    cases = args.case or CASES
    with ExitStack() as stack, ThreadPoolExecutor(max_workers=2) as pool:
        ports = SUITE["start_databases"](stack)
        futures = [pool.submit(run_backend, name, binary, ports, cases) for name, binary in backends.items()]
        failures = sum(future.result() for future in futures)
    print(f"{2 * len(cases) - failures} passed, {failures} failed")
    return int(failures > 0)


if __name__ == "__main__":
    raise SystemExit(main())
