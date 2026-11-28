#!/usr/bin/env python3
"""Exercise generation lease renewal, crash recovery, and stale-task protection."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import ExitStack, contextmanager
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import runpy
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
SUITE = runpy.run_path(str(Path(__file__).with_name("test-sse-idle-timeout.py")))
HOST = SUITE["HOST"]
DB_PASSWORD = SUITE["DB_PASSWORD"]
require = SUITE["require"]
wait_for = SUITE["wait_for"]
docker = SUITE["docker"]
docker_output = SUITE["docker_output"]
published_container = SUITE["published_container"]

LEASE_MS = 3000
OVER_LEASE_SECONDS = 4.6
MODEL_TIMEOUT_MS = 20000
CASES = (
    "silent-stream",
    "heartbeat-stream",
    "non-stream",
    "queued-request",
    "crash-recovery",
    "cancel-release",
    "stale-takeover-renew",
    "stale-takeover-finish",
    "config-validation",
)


def start_databases_with_handles(stack):
    mysql, mysql_port = published_container(
        stack, "mysql:8.4", 3306,
        "-e", f"MYSQL_ROOT_PASSWORD={DB_PASSWORD}",
        "-e", "MYSQL_DATABASE=sse_test",
        "-e", "MYSQL_USER=sse_test",
        "-e", f"MYSQL_PASSWORD={DB_PASSWORD}",
    )
    redis, redis_port = published_container(stack, "redis:8.0-alpine", 6379)
    mysql_client = (
        "exec", "-i", "-e", f"MYSQL_PWD={DB_PASSWORD}", mysql,
        "mysql", "--protocol=tcp", "-h", HOST, "-u", "sse_test", "sse_test",
    )
    wait_for("MySQL", 60, lambda: docker(*mysql_client, "-e", "SELECT 1").returncode == 0)
    docker_output(*mysql_client, input=(ROOT / "backend/db/schema.sql").read_text())
    wait_for("Redis", 10, lambda: docker("exec", redis, "redis-cli", "ping").stdout.strip() == "PONG")
    return mysql_client, mysql_port, redis_port


class Scenario:
    def __init__(self, mode, delay=0.0, reply="ok", partial=None):
        self.mode = mode  # "silent", "heartbeat", "gate", "non-stream"
        self.delay = delay
        self.reply = reply
        self.partial = partial
        self.started = threading.Event()
        self.release = threading.Event()
        self.released = threading.Event()
        self.released_at = None


class LeaseModelHandler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        key = request["messages"][-1]["content"]
        with self.server.lock:
            entry = self.server.scenarios[key]
            scenario = entry.pop(0) if isinstance(entry, list) else entry
        stream = request["stream"]
        try:
            if stream:
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                if scenario.partial:
                    if isinstance(scenario.partial, tuple):
                        self._send_delta(scenario.partial[0])
                        if self.server.stopping.wait(0.35):
                            return
                        self._send_delta(scenario.partial[1])
                    else:
                        self._send_delta(scenario.partial)
            scenario.started.set()
            if scenario.mode == "silent":
                if self.server.stopping.wait(scenario.delay):
                    return
                self._finish(stream, scenario.reply)
            elif scenario.mode == "heartbeat":
                deadline = time.monotonic() + scenario.delay
                while time.monotonic() < deadline:
                    if self.server.stopping.wait(min(0.4, max(0.0, deadline - time.monotonic()))):
                        return
                    self.wfile.write(b": keep-alive\n\n")
                    self.wfile.flush()
                self._finish(stream, scenario.reply)
            elif scenario.mode == "non-stream":
                if self.server.stopping.wait(scenario.delay):
                    return
                self._finish(False, scenario.reply)
            elif scenario.mode == "gate":
                while not self.server.stopping.is_set():
                    if scenario.release.is_set():
                        self._finish(stream, scenario.reply)
                        return
                    if select.select([self.connection], [], [], 0.02)[0] and not self.connection.recv(1):
                        break
                else:
                    return
            else:
                raise ValueError(f"unknown scenario mode {scenario.mode}")
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        scenario.released_at = time.monotonic()
        scenario.released.set()

    def _send_delta(self, text):
        payload = json.dumps({"choices": [{"delta": {"content": text}}]})
        self.wfile.write(f"data: {payload}\n\n".encode())
        self.wfile.flush()

    def _finish(self, stream, text):
        if stream:
            self._send_delta(text)
            self.wfile.write(
                b'data: {"choices":[{"delta":{},"finish_reason":"stop"}]}\n\n'
                b'data: [DONE]\n\n'
            )
            self.wfile.flush()
        else:
            body = json.dumps({
                "choices": [{"message": {"content": text}, "finish_reason": "stop"}]
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()


@contextmanager
def model_server():
    with ThreadingHTTPServer((HOST, 0), LeaseModelHandler) as server:
        server.scenarios = {}
        server.lock = threading.Lock()
        server.stopping = threading.Event()
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
        thread.start()
        try:
            yield server
        finally:
            server.stopping.set()
            server.shutdown()
            thread.join()


class ChatClient:
    def __init__(self, port, process, cookie=""):
        self.port = port
        self.process = process
        self.cookie = cookie

    def api(self, path, expected, body=None, timeout=15):
        headers = {"X-Chat-Request": "1", "Cookie": self.cookie}
        method = "GET" if body is None else "POST"
        payload = None
        if body is not None:
            headers["Content-Type"] = "application/json"
            payload = json.dumps(body)
        client = http.client.HTTPConnection(HOST, self.port, timeout=timeout)
        try:
            client.request(method, path, payload, headers)
            response = client.getresponse()
            data = response.read()
            require(response.status == expected, f"{method} {path}: HTTP {response.status}: {data[:500]!r}")
            return json.loads(data), response.getheader("Set-Cookie")
        finally:
            client.close()

    def login(self):
        username = "lease_" + uuid.uuid4().hex[:16]
        password = "isolated lease regression password"
        self.api("/api/auth/register", 201, {"username": username, "password": password})
        _, cookie = self.api("/api/auth/login", 200, {"identifier": username, "password": password})
        require(cookie, "login returned no session cookie")
        self.cookie = cookie.split(";", 1)[0]

    @contextmanager
    def stream_request(self, payload, timeout=15):
        body = json.dumps(payload).encode()
        raw = (
            f"POST /api/chat HTTP/1.1\r\nHost: {HOST}:{self.port}\r\n"
            f"Cookie: {self.cookie}\r\nX-Chat-Request: 1\r\nContent-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n\r\n"
        ).encode() + body
        with socket.create_connection((HOST, self.port), timeout=timeout) as peer:
            peer.settimeout(timeout)
            started = time.monotonic()
            peer.sendall(raw)
            with peer.makefile("rb") as wire:
                status = wire.readline(8193)
                headers = http.client.parse_headers(wire)
                require(
                    status.startswith(b"HTTP/1.1 200 ")
                    and headers.get("Transfer-Encoding", "").lower() == "chunked"
                    and headers.get("Content-Type", "").startswith("text/event-stream"),
                    f"expected HTTP 200 chunked SSE, got {status!r} {headers}",
                )
                yield peer, wire, started


def build_config(port, model_port, mysql_port, redis_port, namespace, *, server_options=None):
    config = json.loads((ROOT / "config.json.example").read_text())
    config["server"].update(
        host=HOST, port=port, io_threads=2, worker_threads=4, business_threads=2,
        generation_lease_ms=LEASE_MS,
    )
    if server_options:
        config["server"].update(server_options)
    config["mysql"].update(
        port=mysql_port, database="sse_test", username="sse_test", password=DB_PASSWORD, pool_size=6
    )
    config["redis"].update(port=redis_port, pool_size=4)
    config["auth"].update(
        secure_cookie=False,
        key_namespace=f"{namespace}:auth",
        session_namespace=f"{namespace}:session",
    )
    config["models"] = [
        {
            "provider": "local-test",
            "model": "lease-model",
            "api": "openai-completions",
            "base_url": f"http://{HOST}:{model_port}/v1",
            "api_key": "local-lease-regression",
            "timeout_ms": MODEL_TIMEOUT_MS,
            "max_response_bytes": 16 * 1024 * 1024,
        }
    ]
    return config


def server_env():
    env = os.environ.copy()
    excluded = env.get("no_proxy") or env.get("NO_PROXY")
    if excluded != "*":
        excluded = f"{HOST},{excluded}" if excluded else HOST
    env["no_proxy"] = env["NO_PROXY"] = excluded
    return env


@contextmanager
def start_server(binary, model, mysql_port, redis_port, *, namespace=None, server_options=None, cookie=None):
    ns = namespace or f"lease-test:{uuid.uuid4().hex}"
    with tempfile.TemporaryDirectory(prefix="chat-lease-") as directory:
        with socket.socket() as reservation:
            reservation.bind((HOST, 0))
            port = reservation.getsockname()[1]
        config = build_config(port, model.server_port, mysql_port, redis_port, ns, server_options=server_options)
        (Path(directory) / "config.json").write_text(json.dumps(config))
        with tempfile.TemporaryFile(mode="w+t") as log:
            process = subprocess.Popen([binary], cwd=directory, env=server_env(), stdout=log, stderr=subprocess.STDOUT)
            try:
                def ready():
                    require(process.poll() is None, f"server exited with code {process.returncode}")
                    try:
                        with socket.create_connection((HOST, port), timeout=0.5):
                            return True
                    except OSError:
                        return False

                try:
                    wait_for("chat HTTP listener", 15, ready)
                    chat = ChatClient(port, process, cookie=cookie or "")
                    if not cookie:
                        chat.login()
                    yield chat
                except Exception as error:
                    log.seek(0)
                    raise RuntimeError(f"{error}\n{log.read()}") from error
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


def read_events(wire, started):
    return [(elapsed, frame["event"], json.loads(frame["data"]))
            for elapsed, frame in SUITE["frames"](wire, started) if "event" in frame]


def create_conversation(chat, title):
    data, _ = chat.api("/api/conversations", 201, {"title": title})
    return data["conversation"]["id"]


def case_silent_stream(chat, model):
    key = f"silent-{uuid.uuid4().hex}"
    model.scenarios[key] = Scenario("silent", delay=OVER_LEASE_SECONDS, reply="ok")
    conv = create_conversation(chat, "silent-stream")
    payload = {
        "conversation_id": conv, "request_id": uuid.uuid4().hex,
        "provider": "local-test", "model": "lease-model", "stream": True, "content": key,
    }
    mid_check = {}

    with chat.stream_request(payload) as (_, wire, started):
        stream_iter = SUITE["frames"](wire, started)
        meta_elapsed, meta_frame = next((e, f) for e, f in stream_iter if f.get("event") == "meta")
        meta = json.loads(meta_frame["data"])
        assistant_id = meta["assistant_message_id"]

        def probe_mid_lease():
            time.sleep(LEASE_MS / 1000.0 + 0.5)
            view, _ = chat.api(f"/api/conversation?id={conv}", 200)
            mid_check["view"] = view
            conflict, _ = chat.api("/api/chat", 409, {
                "conversation_id": conv, "request_id": uuid.uuid4().hex,
                "provider": "local-test", "model": "lease-model", "stream": False, "content": "concurrent",
            })
            mid_check["conflict"] = conflict

        probe = threading.Thread(target=probe_mid_lease)
        probe.start()
        rest = [(e, f["event"], json.loads(f["data"])) for e, f in stream_iter if "event" in f]
        probe.join(timeout=10)
        require(not probe.is_alive(), "mid-lease probe timed out")

    require("view" in mid_check and "conflict" in mid_check, "mid-lease probe did not run")
    mid_view = mid_check["view"]
    require(mid_view["conversation"]["active_message_id"] == assistant_id,
            f"active_message_id expired mid-generation: {mid_view}")
    mid_msg = next(m for m in mid_view["messages"] if m["id"] == assistant_id)
    require(mid_msg["state"] == "generating", f"message expired mid-generation: {mid_msg}")

    events = [name for _, name, _ in rest]
    require(events == ["delta", "done"], f"expected delta -> done after meta, got {rest}")
    require(rest[0][2]["text"] == "ok", f"unexpected delta {rest[0]}")
    require(rest[1][2]["finish_reason"] == "stop", f"unexpected done {rest[1]}")
    elapsed = rest[-1][0]
    require(elapsed > LEASE_MS / 1000.0, f"finished too early at {elapsed:.2f}s")

    final_view, _ = chat.api(f"/api/conversation?id={conv}", 200)
    final_msg = next(m for m in final_view["messages"] if m["id"] == assistant_id)
    require(final_view["conversation"]["active_message_id"] is None and
            final_view["conversation"]["current_message_id"] == assistant_id and
            final_msg["state"] == "completed" and final_msg["content"] == "ok",
            f"unexpected final history: {final_view}")
    return f"done at {elapsed:.2f}s (lease {LEASE_MS}ms), active preserved past lease"


def case_heartbeat_stream(chat, model):
    key = f"heartbeat-{uuid.uuid4().hex}"
    model.scenarios[key] = Scenario("heartbeat", delay=OVER_LEASE_SECONDS, reply="ok")
    conv = create_conversation(chat, "heartbeat-stream")
    payload = {
        "conversation_id": conv, "request_id": uuid.uuid4().hex,
        "provider": "local-test", "model": "lease-model", "stream": True, "content": key,
    }
    with chat.stream_request(payload) as (_, wire, started):
        observed = read_events(wire, started)
    events = [name for _, name, _ in observed]
    require(events == ["meta", "delta", "done"], f"expected meta -> delta -> done, got {observed}")
    require(observed[1][2]["text"] == "ok", f"unexpected delta {observed[1]}")
    elapsed = observed[-1][0]
    require(elapsed > LEASE_MS / 1000.0, f"finished too early at {elapsed:.2f}s")

    view, _ = chat.api(f"/api/conversation?id={conv}", 200)
    assistant_id = observed[0][2]["assistant_message_id"]
    msg = next(m for m in view["messages"] if m["id"] == assistant_id)
    require(view["conversation"]["active_message_id"] is None and
            msg["state"] == "completed" and msg["content"] == "ok",
            f"unexpected history: {view}")
    return f"done at {elapsed:.2f}s after SSE comments"


def case_non_stream(chat, model):
    key = f"nonstream-{uuid.uuid4().hex}"
    model.scenarios[key] = Scenario("non-stream", delay=OVER_LEASE_SECONDS, reply="ok")
    conv = create_conversation(chat, "non-stream")
    started = time.monotonic()
    reply, _ = chat.api("/api/chat", 200, {
        "conversation_id": conv, "request_id": uuid.uuid4().hex,
        "provider": "local-test", "model": "lease-model", "stream": False, "content": key,
    }, timeout=15)
    elapsed = time.monotonic() - started
    require(reply["reply"] == "ok" and reply["finish_reason"] == "stop", f"unexpected JSON reply: {reply}")
    require(elapsed > LEASE_MS / 1000.0, f"finished too early at {elapsed:.2f}s")
    view, _ = chat.api(f"/api/conversation?id={conv}", 200)
    msg = next(m for m in view["messages"] if m["id"] == reply["assistant_message_id"])
    require(view["conversation"]["active_message_id"] is None and
            msg["state"] == "completed" and msg["content"] == "ok",
            f"unexpected history: {view}")
    return f"JSON 200 at {elapsed:.2f}s"


def case_queued_request(binary, model, mysql_port, redis_port):
    with start_server(binary, model, mysql_port, redis_port, server_options={"worker_threads": 1}) as chat:
        key1 = f"q1-{uuid.uuid4().hex}"
        key2 = f"q2-{uuid.uuid4().hex}"
        s1 = model.scenarios[key1] = Scenario("silent", delay=OVER_LEASE_SECONDS, reply="first")
        model.scenarios[key2] = Scenario("silent", delay=0.1, reply="second")
        conv1 = create_conversation(chat, "queued-1")
        conv2 = create_conversation(chat, "queued-2")

        def run_stream(conv, key):
            with chat.stream_request({
                "conversation_id": conv, "request_id": uuid.uuid4().hex,
                "provider": "local-test", "model": "lease-model", "stream": True, "content": key,
            }, timeout=20) as (_, wire, started):
                return read_events(wire, started)

        with ThreadPoolExecutor(max_workers=2) as pool:
            f1 = pool.submit(run_stream, conv1, key1)
            require(s1.started.wait(3), "first request did not reach upstream")
            f2 = pool.submit(run_stream, conv2, key2)
            time.sleep(LEASE_MS / 1000.0 + 0.5)
            view2_mid, _ = chat.api(f"/api/conversation?id={conv2}", 200)
            require(view2_mid["conversation"]["active_message_id"] is not None,
                    f"queued request lease expired before worker started: {view2_mid}")
            r1 = f1.result(timeout=15)
            r2 = f2.result(timeout=15)

        require([n for _, n, _ in r1] == ["meta", "delta", "done"] and r1[1][2]["text"] == "first", str(r1))
        require([n for _, n, _ in r2] == ["meta", "delta", "done"] and r2[1][2]["text"] == "second", str(r2))
        view2, _ = chat.api(f"/api/conversation?id={conv2}", 200)
        require(view2["conversation"]["active_message_id"] is None and
                view2["messages"][-1]["state"] == "completed" and
                view2["messages"][-1]["content"] == "second",
                f"unexpected queued history: {view2}")
        return f"queued request waited >{LEASE_MS}ms and completed at {r2[-1][0]:.2f}s"


def case_crash_recovery(binary, model, mysql_port, redis_port):
    ns = f"lease-crash:{uuid.uuid4().hex}"
    key1 = f"crash-{uuid.uuid4().hex}"
    s1 = Scenario("gate", reply="never", partial=("partial-", "before-crash"))
    s2 = Scenario("non-stream", delay=0.05, reply="recovered")
    model.scenarios[key1] = [s1, s2]

    with start_server(binary, model, mysql_port, redis_port, namespace=ns) as chat:
        cookie = chat.cookie
        conv = create_conversation(chat, "crash-recovery")
        with chat.stream_request({
            "conversation_id": conv, "request_id": uuid.uuid4().hex,
            "provider": "local-test", "model": "lease-model", "stream": True, "content": key1,
        }) as (_, wire, started):
            stream_iter = SUITE["frames"](wire, started)
            meta = json.loads(next(f["data"] for _, f in stream_iter if f.get("event") == "meta"))
            assistant_id = meta["assistant_message_id"]
            d1 = json.loads(next(f["data"] for _, f in stream_iter if f.get("event") == "delta"))
            d2 = json.loads(next(f["data"] for _, f in stream_iter if f.get("event") == "delta"))
            require(d1["text"] + d2["text"] == "partial-before-crash", str((d1, d2)))
            require(s1.started.wait(2), "upstream never started")
            # Wait for SaveProgress (250ms cadence) to persist the partial output before SIGKILL.
            wait_for("persisted partial content", 3, lambda: (
                any(m["id"] == assistant_id and m["content"] == "partial-before-crash"
                    for m in chat.api(f"/api/conversation?id={conv}", 200)[0]["messages"])
            ))
            killed_at = time.monotonic()
            chat.process.kill()
            chat.process.wait(timeout=5)

    # Restart a fresh instance sharing MySQL + Redis + session namespace.
    with start_server(binary, model, mysql_port, redis_port, namespace=ns, cookie=cookie) as recovered:
        remaining = (LEASE_MS / 1000.0 + 0.4) - (time.monotonic() - killed_at)
        if remaining > 0:
            time.sleep(remaining)
        view, _ = recovered.api(f"/api/conversation?id={conv}", 200)
        msg = next(m for m in view["messages"] if m["id"] == assistant_id)
        require(view["conversation"]["active_message_id"] is None and
                msg["state"] == "interrupted" and msg["content"] == "partial-before-crash",
                f"expected expired interrupted placeholder with partial text, got {view}")

        user_id = view["messages"][0]["id"]
        reply, _ = recovered.api("/api/chat", 200, {
            "conversation_id": conv, "request_id": uuid.uuid4().hex,
            "provider": "local-test", "model": "lease-model", "stream": False,
            "regenerate_message_id": user_id,
        })
        require(reply["reply"] == "recovered", str(reply))
        return "SIGKILL placeholder reclaimed after lease and new generation succeeded"


def case_cancel_release(chat, model):
    key = f"cancel-{uuid.uuid4().hex}"
    s = model.scenarios[key] = Scenario("gate", reply="never")
    conv = create_conversation(chat, "cancel-release")
    with chat.stream_request({
        "conversation_id": conv, "request_id": uuid.uuid4().hex,
        "provider": "local-test", "model": "lease-model", "stream": True, "content": key,
    }) as (peer, wire, started):
        meta = next(f for _, f in SUITE["frames"](wire, started) if f.get("event") == "meta")
        assistant_id = json.loads(meta["data"])["assistant_message_id"]
        require(s.started.wait(2), "upstream did not start")
        peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        cancelled_at = time.monotonic()
        peer.close()
    require(s.released.wait(1.5), "upstream was not released after client disconnect")
    latency = s.released_at - cancelled_at
    require(latency < 0.5, f"upstream release took {latency:.3f}s")

    def cleared():
        view, _ = chat.api(f"/api/conversation?id={conv}", 200)
        return view if view["conversation"]["active_message_id"] is None else None

    view = wait_for("immediate cancel history cleanup", 1.5, cleared)
    msg = next(m for m in view["messages"] if m["id"] == assistant_id)
    require(msg["state"] == "interrupted", f"expected interrupted, got {msg}")
    return f"cancelled and cleared in {latency:.3f}s (< lease {LEASE_MS}ms)"


def case_stale_takeover(chat, model, mysql_client, *, trigger_via_finish):
    key_a = f"stale-a-{uuid.uuid4().hex}"
    sa = Scenario("gate", reply="stale-reply")
    sb = Scenario("non-stream", delay=0.05, reply="new-reply")
    model.scenarios[key_a] = [sa, sb]
    conv = create_conversation(chat, f"stale-takeover-{trigger_via_finish}")

    def run_a(meta_holder):
        with chat.stream_request({
            "conversation_id": conv, "request_id": uuid.uuid4().hex,
            "provider": "local-test", "model": "lease-model", "stream": True, "content": key_a,
        }, timeout=15) as (_, wire, started):
            events = []
            for elapsed, frame in SUITE["frames"](wire, started):
                if "event" not in frame:
                    continue
                data = json.loads(frame["data"])
                events.append((elapsed, frame["event"], data))
                if frame["event"] == "meta":
                    meta_holder["meta"] = data
                    meta_holder["ready"].set()
            return events

    meta_holder = {"ready": threading.Event()}
    with ThreadPoolExecutor(max_workers=1) as pool:
        fa = pool.submit(run_a, meta_holder)
        require(meta_holder["ready"].wait(3), "Task A never sent meta")
        require(sa.started.wait(3), "Task A never reached upstream")
        user_id = meta_holder["meta"]["user_message_id"]
        assistant_a_id = meta_holder["meta"]["assistant_message_id"]

        # Simulate lease expiry (e.g. partition / pause) and takeover by Task B.
        docker_output(*mysql_client, "-e", f"UPDATE conversations SET generation_deadline = 1 WHERE id = {int(conv)};")
        reply_b, _ = chat.api("/api/chat", 200, {
            "conversation_id": conv, "request_id": uuid.uuid4().hex,
            "provider": "local-test", "model": "lease-model", "stream": False,
            "regenerate_message_id": user_id,
        })
        require(reply_b["reply"] == "new-reply", f"Task B failed: {reply_b}")
        assistant_b_id = reply_b["assistant_message_id"]

        if trigger_via_finish:
            require(not sa.released.is_set(),
                    "RenewLease cancelled Task A before Finish path could be exercised")
            sa.release.set()
        else:
            # Do NOT release upstream; RenewLease must detect takeover and abort upstream A promptly!
            require(sa.released.wait(LEASE_MS / 1000.0 + 1.5),
                    "RenewLease did not abort stale Task A after takeover")

        events_a = fa.result(timeout=10)

    event_names = [name for _, name, _ in events_a]
    if trigger_via_finish:
        deltas = "".join(data["text"] for _, name, data in events_a if name == "delta")
        require(deltas == "stale-reply",
                f"expected Finish path after stale-reply delta, got {events_a}")
    require(event_names[-1] == "error" and "done" not in event_names,
            f"stale Task A should end with error, got {events_a}")
    require(events_a[-1][2]["code"] == "history_unavailable",
            f"expected history_unavailable for stale Task A, got {events_a[-1]}")

    view, _ = chat.api(f"/api/conversation?id={conv}", 200)
    require(view["conversation"]["active_message_id"] is None and
            view["conversation"]["current_message_id"] == assistant_b_id,
            f"Task B was corrupted by stale Task A: {view}")
    msg_b = next(m for m in view["messages"] if m["id"] == assistant_b_id)
    require(msg_b["state"] == "completed" and msg_b["content"] == "new-reply", str(msg_b))

    msgs, _ = chat.api(f"/api/conversation/messages?conversation_id={conv}", 200)
    msg_a = next(m for m in msgs["items"] if m["id"] == assistant_a_id)
    require(msg_a["state"] == "interrupted" and msg_a["content"] != "stale-reply",
            f"stale Task A overwrote finalized interrupted message: {msg_a}")
    return f"stale Task A rejected ({'finish' if trigger_via_finish else 'renew'} path), Task B intact"


def case_config_validation(binary, model, mysql_port, redis_port):
    for bad in (0, -1, 500, 999, "3000", True, 1000.5):
        with tempfile.TemporaryDirectory(prefix="chat-lease-cfg-") as directory:
            config = build_config(53499, model.server_port, mysql_port, redis_port,
                                  f"cfg:{uuid.uuid4().hex}", server_options={"generation_lease_ms": bad})
            (Path(directory) / "config.json").write_text(json.dumps(config))
            result = subprocess.run([binary], cwd=directory, env=server_env(),
                                    capture_output=True, text=True, timeout=5)
            output = (result.stdout or "") + (result.stderr or "")
            require(result.returncode != 0, f"invalid generation_lease_ms={bad!r} unexpectedly started")
            require("generation_lease_ms" in output,
                    f"invalid generation_lease_ms={bad!r} failed without mentioning field: {output!r}")
    # Default config without generation_lease_ms must still start cleanly.
    with tempfile.TemporaryDirectory(prefix="chat-lease-default-") as directory:
        with socket.socket() as reservation:
            reservation.bind((HOST, 0))
            port = reservation.getsockname()[1]
        config = build_config(port, model.server_port, mysql_port, redis_port, f"cfg:{uuid.uuid4().hex}")
        config["server"].pop("generation_lease_ms")
        (Path(directory) / "config.json").write_text(json.dumps(config))
        proc = subprocess.Popen([binary], cwd=directory, env=server_env(),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            def ready():
                require(proc.poll() is None, f"default server exited with {proc.returncode}")
                try:
                    with socket.create_connection((HOST, port), timeout=0.5):
                        return True
                except OSError:
                    return False

            wait_for("default config server", 10, ready)
        finally:
            proc.terminate()
            proc.wait(timeout=5)
    return "invalid generation_lease_ms rejected, omitted defaults cleanly"


def run_backend(name, binary, mysql_client, mysql_port, redis_port, cases):
    with model_server() as model, start_server(binary, model, mysql_port, redis_port) as chat:
        checks = {
            "silent-stream": lambda: case_silent_stream(chat, model),
            "heartbeat-stream": lambda: case_heartbeat_stream(chat, model),
            "non-stream": lambda: case_non_stream(chat, model),
            "queued-request": lambda: case_queued_request(binary, model, mysql_port, redis_port),
            "crash-recovery": lambda: case_crash_recovery(binary, model, mysql_port, redis_port),
            "cancel-release": lambda: case_cancel_release(chat, model),
            "stale-takeover-renew": lambda: case_stale_takeover(chat, model, mysql_client, trigger_via_finish=False),
            "stale-takeover-finish": lambda: case_stale_takeover(chat, model, mysql_client, trigger_via_finish=True),
            "config-validation": lambda: case_config_validation(binary, model, mysql_port, redis_port),
        }
        failures = 0
        with ThreadPoolExecutor(max_workers=min(4, len(cases))) as pool:
            tasks = {pool.submit(checks[case]): case for case in cases}
            for future in as_completed(tasks):
                case = tasks[future]
                try:
                    print(f"PASS {name}/{case}: {future.result()}", flush=True)
                except Exception as error:
                    failures += 1
                    print(f"FAIL {name}/{case}: {type(error).__name__}: {error}", flush=True)
        return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for backend in ("reactor", "proactor"):
        parser.add_argument(f"--{backend}", type=Path, help=f"path to the {backend} server")
    parser.add_argument("--case", action="append", choices=CASES, help="run only this case (repeatable)")
    args = parser.parse_args()
    servers = {}
    for name in ("reactor", "proactor"):
        path = getattr(args, name)
        if path is not None:
            servers[name] = path.resolve()
    if not servers:
        parser.error("provide at least one of --reactor or --proactor")
    for name, binary in servers.items():
        if not binary.is_file():
            parser.error(f"{name} server does not exist: {binary}")
        reported = subprocess.run([binary, "--network-backend"], check=True,
                                  capture_output=True, text=True, timeout=5).stdout.strip()
        if reported != name:
            parser.error(f"--{name} expects a {name} server, but {binary} reports {reported!r}")

    cases = tuple(dict.fromkeys(args.case)) if args.case else CASES
    with ExitStack() as stack:
        mysql_client, mysql_port, redis_port = start_databases_with_handles(stack)
        failures = 0
        for name, binary in servers.items():
            try:
                failures += run_backend(name, binary, mysql_client, mysql_port, redis_port, cases)
            except Exception as error:
                print(f"FAIL {name} setup: {error}", flush=True)
                failures += len(cases)
    total = len(servers) * len(cases)
    print(f"{total - failures} passed, {failures} failed", flush=True)
    return int(failures > 0)


if __name__ == "__main__":
    raise SystemExit(main())
