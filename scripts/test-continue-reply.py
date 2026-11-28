#!/usr/bin/env python3
"""Verify same-reply continuation through chat, history and a mock provider."""

import argparse
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, contextmanager
import http.client
import json
import signal
from pathlib import Path
import runpy
import socket
import subprocess
import threading
import time
import uuid


FIRST = runpy.run_path(str(Path(__file__).with_name("test-first-stream-output.py")))
SUITE = FIRST["SUITE"]
HOST = FIRST["HOST"]
require = FIRST["require"]
history = FIRST["history"]
settled_history = FIRST["settled_history"]
wait_for = FIRST["wait_for"]
TEXT_CAP = 4 * 1024 * 1024


class Model(FIRST["Model"]):
    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        messages = data["messages"]
        continuation = messages[-1].get("prefix", False)
        question = messages[-2 if continuation else -1]["content"]
        case = question.partition(":")[0]
        with self.server.calls_lock:
            self.server.calls[question].append((self.path, data))
            attempt = len(self.server.calls[question])
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            if data["model"] == "ordinary-model":
                self.delta({"content": "sibling"})
                self.end_stream("length")
            elif question.startswith("follow-up:"):
                self.delta({"content": "follow-up answer"})
                self.end_stream("stop")
            elif case == "accumulated-limit" and not continuation:
                self.delta({"content": "Before"})
                remaining = TEXT_CAP - len("Before") - 10
                while remaining:
                    size = min(remaining, 128 * 1024)
                    self.delta({"reasoning_content": "r" * size})
                    remaining -= size
                self.end_stream("length")
            elif case == "accumulated-limit":
                self.delta({"reasoning_content": "1234"})
                self.delta({"content": "abcdef"})
                self.delta({"content": "over limit"})
            elif continuation:
                if case == "repeat" and attempt == 3:
                    self.send('{"error":{"message":"mock failure"}}')
                elif case == "repeat" and attempt == 2:
                    self.delta({"content": " again"})
                    self.end_stream("length")
                elif case == "disconnect" and attempt == 2:
                    self.delta({"content": " joined"})
                    self.wait_for_disconnect()
                elif case == "stale":
                    self.delta({"content": " fresh"})
                    self.wait_for_release(question, "finish-new")
                    self.end_stream("stop")
                else:
                    self.delta({"content": " after"})
                    self.end_stream("stop")
            else:
                self.delta({"reasoning_content": "thought"})
                if case == "stale":
                    # Let the second fragment pass the normal progress-save cadence.
                    time.sleep(0.3)
                self.delta({"content": "Before"})
                if case == "disconnect":
                    self.wait_for_disconnect()
                elif case == "stale":
                    self.wait_for_release(question, "release-old")
                    self.delta({"content": " late"})
                    self.end_stream("stop")
                elif case == "upstream-close":
                    return
                else:
                    self.end_stream("length")
        except (BrokenPipeError, ConnectionResetError):
            pass


    def wait_for_release(self, question, phase):
        while not self.server.stopping.is_set() and not self.server.gates[question, phase].wait(0.05):
            pass


@contextmanager
def model_server():
    with FIRST["ThreadingHTTPServer"]((HOST, 0), Model) as server:
        server.stopping = threading.Event()
        server.calls = defaultdict(list)
        server.gates = defaultdict(threading.Event)
        server.calls_lock = threading.Lock()
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
        thread.start()
        try:
            yield server
        finally:
            server.stopping.set()
            server.shutdown()
            thread.join()


def request(conversation, **fields):
    return {"conversation_id": conversation, "request_id": uuid.uuid4().hex, "stream": True, **fields}


@contextmanager
def stream(chat, body):
    with socket.create_connection((HOST, chat.port), timeout=5) as peer:
        started = time.monotonic()
        FIRST["DISCONNECT"]["send_chat"](chat, peer, body)
        with peer.makefile("rb") as wire:
            status = wire.readline()
            headers = http.client.parse_headers(wire)
            require(status.startswith(b"HTTP/1.1 200 ") and
                    headers.get("Content-Type", "").startswith("text/event-stream"),
                    f"expected HTTP 200 with SSE, got {status!r} {headers}")
            yield ((frame["event"], json.loads(frame["data"]))
                   for _, frame in SUITE["frames"](wire, started) if "event" in frame)


def conversation(chat, title):
    return chat.api("/api/conversations", 201, {"title": title})[0]["conversation"]["id"]


def complete(chat, body):
    with stream(chat, body) as events:
        return list(events)


def send_question(chat_id, question, **fields):
    return request(chat_id, provider="deepseek", model="deepseek-flash", content=question, **fields)


def continuation(chat_id, saved):
    return request(chat_id, continue_message_id=saved["id"], continue_from=saved["continue_from"])


def latest(chat, chat_id):
    return history(chat, chat_id)["messages"][-1]


def same_reply(chat, model, _start_peer):
    question = "same-reply:" + uuid.uuid4().hex
    chat_id = conversation(chat, question)
    first = send_question(chat_id, question, reasoning_effort="max")
    original = complete(chat, first)
    require(original[-1][0] == "done" and original[-1][1]["truncated"],
            f"expected the initial reply to be truncated: {original}")
    user, partial = history(chat, chat_id)["messages"]
    original_replay = complete(chat, first)
    continued = request(chat_id, continue_message_id=partial["id"],
                        continue_from=partial.get("continue_from", "baseline"))
    frames = complete(chat, continued)
    require([event for event, _ in frames] == ["meta", "delta", "done"],
            f"unexpected continuation events: {frames}")
    require(frames[0][1]["assistant_message_id"] == partial["id"] and
            frames[0][1]["user_message_id"] == user["id"] and frames[1][1]["text"] == " after",
            f"continuation changed the IDs or replayed the prefix: {frames}")
    saved_user, saved = history(chat, chat_id)["messages"]
    require(saved_user == user and saved["id"] == partial["id"] and
            saved["content"] == "Before after" and saved["reasoning"] == "thought",
            f"continuation did not append to the saved answer: {saved}")
    require(partial.get("continue_from") and "continue_from" not in saved,
            "history should only offer continuation for the cutoff reply")
    path, provider = model.calls[question][1]
    require(path == "/beta/chat/completions" and provider["reasoning_effort"] == "max" and
            provider["messages"] == [{"role": "user", "content": question},
                                     {"role": "assistant", "content": "Before", "prefix": True}],
            f"provider did not receive the original question and prefix: {path}, {provider}")
    require(complete(chat, first) == original_replay, "ordinary replay changed after continuation")
    replay = complete(chat, continued)
    require(replay[0][1].get("snapshot") and replay[1] == ("delta", {"text": "Before after"}),
            f"continuation replay did not identify the saved snapshot: {replay}")
    require(len(model.calls[question]) == 2, "replayed request called the provider again")
    follow_up = "follow-up:" + uuid.uuid4().hex
    complete(chat, send_question(chat_id, follow_up))
    require(model.calls[follow_up][0][1]["messages"][-2] ==
            {"role": "assistant", "content": "Before after"},
            "later questions did not see the joined reply")


def repeat(chat, model, _start_peer):
    question = "repeat:" + uuid.uuid4().hex
    chat_id = conversation(chat, question)
    complete(chat, send_question(chat_id, question))
    partial = latest(chat, chat_id)
    first = continuation(chat_id, partial)
    require(complete(chat, first)[-1][1]["finish_reason"] == "length", "second cutoff lost its reason")
    saved = latest(chat, chat_id)
    require(saved["id"] == partial["id"] and saved["content"] == "Before again" and
            saved["continue_from"] != partial["continue_from"], "second cutoff is not resumable")
    chat.api("/api/chat", 409, continuation(chat_id, partial))
    failed = continuation(chat_id, saved)
    require(complete(chat, failed)[-1][0] == "error", "provider failure did not reach client")
    failed_saved = latest(chat, chat_id)
    require(failed_saved["content"] == saved["content"] and failed_saved["state"] == "failed",
            "provider failure erased saved content")
    chat.api("/api/chat", 409, failed)
    require(complete(chat, continuation(chat_id, failed_saved))[-1][0] == "done",
            "could not retry a failed continuation")
    replay = complete(chat, first)
    require(replay[0][1]["snapshot"] and replay[1] == ("delta", {"text": "Before again"})
            and replay[-1][1]["truncated"], "the earlier continuation replay now points to a later result")
    chat.api("/api/chat", 409, failed)
    require(latest(chat, chat_id)["content"] == "Before again after" and len(model.calls[question]) == 4,
            "retry duplicated a suffix or changed the result")


def disconnect(chat, model, _start_peer):
    question = "disconnect:" + uuid.uuid4().hex
    chat_id = conversation(chat, question)
    with stream(chat, send_question(chat_id, question)) as frames:
        require([next(frames)[0] for _ in range(3)] == ["meta", "reasoning_delta", "delta"],
                "original reply did not start")
    partial = settled_history(chat, chat_id)["messages"][-1]
    require(partial["state"] == "interrupted" and partial["content"] == "Before", str(partial))
    first = continuation(chat_id, partial)
    with stream(chat, first) as frames:
        require(next(frames)[1]["assistant_message_id"] == partial["id"], "continuation changed IDs")
        require(next(frames) == ("delta", {"text": " joined"}), "first new suffix missing")
        during = latest(chat, chat_id)
        require(during["state"] == "generating" and during["content"] == "Before joined",
                f"the first new SSE delta preceded its history write: {during}")
        chat.api("/api/chat", 409, first)
        chat.api("/api/chat", 409, continuation(chat_id, partial))
        chat.api("/api/conversation/select", 409, {"conversation_id": chat_id, "message_id": partial["id"]})
    stopped = settled_history(chat, chat_id)["messages"][-1]
    require(stopped["id"] == partial["id"] and stopped["content"] == "Before joined" and
            stopped["state"] == "interrupted", "stopping the continuation lost text")
    chat.api("/api/chat", 409, first)
    complete(chat, continuation(chat_id, stopped))
    require(latest(chat, chat_id)["content"] == "Before joined after" and len(model.calls[question]) == 3,
            "repeated request appended twice after stopping")


def branch(chat, _model, _start_peer):
    question = "upstream-close:" + uuid.uuid4().hex
    chat_id = conversation(chat, question)
    other_chat = conversation(chat, "unrelated")
    first = send_question(chat_id, question)
    require(complete(chat, first)[-1][0] == "error", "unexpected EOF should be an upstream error")
    user, partial = history(chat, chat_id)["messages"]
    chat.api("/api/chat", 404, continuation(other_chat, partial))
    chat.api("/api/chat", 409, request(chat_id, continue_message_id=user["id"],
                                       continue_from=partial["continue_from"]))
    # A successful regeneration selects a sibling reply for the same question.
    regenerate = request(chat_id, provider="ordinary", model="ordinary-model", regenerate_message_id=user["id"])
    alternate = complete(chat, regenerate)
    require(alternate[-1][0] == "done", f"sibling regeneration failed: {alternate}")
    sibling = latest(chat, chat_id)
    pending = continuation(chat_id, partial)
    chat.api("/api/chat", 409, pending)
    require(latest(chat, chat_id) == sibling and sibling["id"] != partial["id"], "late continuation switched versions")
    chat.api("/api/chat", 400, request(chat_id, continue_message_id=sibling["id"],
                                       continue_from=regenerate["request_id"]))
    require("continue_from" not in sibling, "history offered continuation through an unsupported model")
    view, _ = chat.api("/api/conversation/select", 200, {"conversation_id": chat_id, "message_id": partial["id"]})
    reselected = view["messages"][-1]
    require(reselected["id"] == partial["id"] and reselected["continue_from"] == partial["continue_from"],
            "branch selection did not restore the original revision")
    frames = complete(chat, continuation(chat_id, reselected))
    require(frames[-1][0] == "done" and latest(chat, chat_id)["content"] == "Before after",
            "the selected interrupted version could not continue")
    chat.api("/api/chat", 409, first)
    chat.api("/api/conversation/select", 200, {"conversation_id": chat_id, "message_id": sibling["id"]})
    require(latest(chat, chat_id)["content"] == sibling["content"], "continuation changed another version")


def accumulated_limit(chat, _model, _start_peer):
    question = "accumulated-limit:" + uuid.uuid4().hex
    chat_id = conversation(chat, question)
    require(complete(chat, send_question(chat_id, question))[-1][1]["truncated"], "initial cutoff failed")
    partial = latest(chat, chat_id)
    frames = complete(chat, continuation(chat_id, partial))
    require([event for event, _ in frames] == ["meta", "reasoning_delta", "delta", "error"]
            and "大小限制" in frames[-1][1]["error"], f"combined text cap was not enforced: {frames}")
    saved = latest(chat, chat_id)
    require(saved["id"] == partial["id"] and saved["content"] == "Beforeabcdef" and
            len(saved["content"]) + len(saved["reasoning"]) == TEXT_CAP and "continue_from" not in saved,
            "the combined cap did not retain the exact saved and accepted text")


def stale(chat, model, start_peer):
    question = "stale:" + uuid.uuid4().hex
    username = "continue_" + uuid.uuid4().hex[:12]
    password = "isolated previous writer fixture"
    chat.api("/api/auth/register", 201, {"username": username, "password": password})
    credentials = {"identifier": username, "password": password}
    _, cookie = chat.api("/api/auth/login", 200, credentials)
    chat.cookie = cookie.split(";", 1)[0]
    chat_id = conversation(chat, question)
    with start_peer() as second:
        _, cookie = second.api("/api/auth/login", 200, credentials)
        second.cookie = cookie.split(";", 1)[0]
        with stream(chat, send_question(chat_id, question)) as old:
            require([next(old)[0] for _ in range(3)] == ["meta", "reasoning_delta", "delta"],
                    "stale task did not produce a prefix")
            chat.process.send_signal(signal.SIGSTOP)
            try:
                partial = wait_for("stopped server's lease to expire", 3, lambda: (
                    latest(second, chat_id) if history(second, chat_id)["conversation"]["active_message_id"] is None
                    else None))
                require(partial["state"] == "interrupted" and partial["content"] == "Before",
                        f"expired generation did not retain prefix: {partial}")
                with stream(second, continuation(chat_id, partial)) as new:
                    require(next(new)[1]["assistant_message_id"] == partial["id"], "takeover changed message ID")
                    require(next(new) == ("delta", {"text": " fresh"}), "new writer did not persist its first delta")
                    model.gates[question, "release-old"].set()
                    chat.process.send_signal(signal.SIGCONT)
                    events = list(old)
                    require(events and events[-1][0] == "error" and
                            all(event != "delta" for event, _ in events),
                            f"the expired task emitted after takeover: {events}")
                    current = latest(second, chat_id)
                    require(current["state"] == "generating" and current["content"] == "Before fresh",
                            "the old task overwrote a reopened reply")
                    model.gates[question, "finish-new"].set()
                    require(next(new)[0] == "done", "new task could not finalize same ID")
            finally:
                chat.process.send_signal(signal.SIGCONT)
                model.gates[question, "release-old"].set()
                model.gates[question, "finish-new"].set()
    require(latest(chat, chat_id)["content"] == "Before fresh" and len(model.calls[question]) == 2,
            "stale data reached the final history")


CASES = {"same-reply": same_reply, "repeat": repeat, "disconnect": disconnect, "branch": branch,
         "accumulated-limit": accumulated_limit, "stale": stale}


def run_backend(name, binary, ports, cases):
    failures = 0
    with model_server() as model:
        models = [{"provider": provider, "model": name, "api": "openai-completions",
                   "base_url": f"http://{HOST}:{model.server_port}/v1", "api_key": "local-test"}
                  for provider, name in (("deepseek", "deepseek-flash"), ("ordinary", "ordinary-model"))]
        def start_peer():
            return SUITE["chat_server"](binary, model, *ports, models=models,
                                       server_options={"generation_lease_ms": 1200})

        with start_peer() as chat:
            for case in cases:
                try:
                    CASES[case](chat, model, start_peer)
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
