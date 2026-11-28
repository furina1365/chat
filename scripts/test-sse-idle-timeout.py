#!/usr/bin/env python3
"""Run the SSE idle-timeout regression against two locally built chat servers."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import ExitStack, contextmanager
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
HOST = "127.0.0.1"
IDLE_SECONDS = 60
MODEL_TIMEOUT_SECONDS = 3
LONG_SECONDS = IDLE_SECONDS + 4
TIMEOUT_MESSAGE = "模型请求超时"
DB_PASSWORD = "sse-regression-only"
PRESSURE_CASES = ("backpressure", "slow-reader")
CASES = ("body", "heartbeat", "silent", "partial", *PRESSURE_CASES, "model-timeout")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def wait_for(description, seconds, probe):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = probe()
        if result:
            return result
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {description}")


def docker(*args, **kwargs):
    return subprocess.run(
        ["docker", *args], capture_output=True, text=True, timeout=120, **kwargs
    )


def docker_output(*args, **kwargs):
    result = docker(*args, **kwargs)
    require(result.returncode == 0, f"docker {args[0]}: {result.stderr.strip()}")
    return result.stdout.strip()


def published_container(stack, image, port, *options):
    container = docker_output(
        "run", "--rm", "-d", "-p", f"{HOST}::{port}", *options, image
    )
    stack.callback(docker, "rm", "-f", "-v", container)
    endpoint = docker_output("port", container, f"{port}/tcp")
    return container, int(endpoint.rsplit(":", 1)[1])


def start_databases(stack):
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
    return mysql_port, redis_port


class Pressure:
    def __init__(self):
        self.fill = threading.Event()
        self.blocked = threading.Event()
        self.stop = threading.Event()
        self.payload_bytes = 0
        self.last_pulse = None

    def send(self, delta):
        while not self.stop.is_set() and not self.fill.wait(0.1):
            pass
        # Each backslash occupies two bytes after the server JSON-encodes a delta.
        while not self.stop.wait(0.04) and not self.blocked.is_set():
            if self.payload_bytes >= 6 * 1024 * 1024:
                return
            delta("\\" * 8192)
            self.payload_bytes += 16384
        while not self.stop.wait(0.5):
            delta("x" * 1024)
            self.payload_bytes += 1024
            self.last_pulse = time.monotonic()


class ModelHandler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        case = request["messages"][-1]["content"]
        if self.path != "/v1/chat/completions" or not request["stream"]:
            self.send_error(400)
            return
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.respond(case)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def respond(self, case):
        def send(payload):
            self.wfile.write(f"data: {payload}\n\n".encode())
            self.wfile.flush()

        def delta(text):
            send(json.dumps({"choices": [{"delta": {"content": text}}]}))

        stop = self.server.stop_model
        if case == "body":
            for tick in range(17):
                if tick and stop.wait(4):
                    return
                delta(f"{tick},")
        elif case == "heartbeat":
            if stop.wait(LONG_SECONDS):
                return
            delta("delayed reply")
        elif case == "model-timeout":
            delta("partial reply")
            stop.wait()
            return
        elif case in PRESSURE_CASES:
            self.server.pressure[case].send(delta)
            return
        else:
            raise ValueError(f"unknown model scenario {case}")
        send('{"choices":[{"delta":{},"finish_reason":"stop"}]}')
        send("[DONE]")


@contextmanager
def model_server():
    with ThreadingHTTPServer((HOST, 0), ModelHandler) as server:
        server.stop_model = threading.Event()
        server.pressure = {case: Pressure() for case in PRESSURE_CASES}
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            yield server
        finally:
            for pressure in server.pressure.values():
                pressure.stop.set()
            server.stop_model.set()
            server.shutdown()
            thread.join()


class Chat:
    def __init__(self, port, process):
        self.port = port
        self.process = process
        self.cookie = ""

    def api(self, path, expected, body=None):
        headers = {"X-Chat-Request": "1", "Cookie": self.cookie}
        method = "GET" if body is None else "POST"
        payload = None
        if body is not None:
            headers["Content-Type"] = "application/json"
            payload = json.dumps(body)
        client = http.client.HTTPConnection(HOST, self.port, timeout=5)
        try:
            client.request(method, path, payload, headers)
            response = client.getresponse()
            data = response.read()
            require(response.status == expected, f"{method} {path}: HTTP {response.status}: {data[:500]!r}")
            return json.loads(data), response.getheader("Set-Cookie")
        finally:
            client.close()

    def login(self):
        username = "sse_" + uuid.uuid4().hex[:16]
        password = "isolated SSE regression password"
        self.api("/api/auth/register", 201, {"username": username, "password": password})
        _, cookie = self.api("/api/auth/login", 200, {"identifier": username, "password": password})
        require(cookie, "login returned no session cookie")
        self.cookie = cookie.split(";", 1)[0]

    @contextmanager
    def stream(self, case, receive_buffer=None):
        data, _ = self.api("/api/conversations", 201, {"title": case})
        conversation = data["conversation"]["id"]
        body = json.dumps({
            "conversation_id": conversation,
            "request_id": uuid.uuid4().hex,
            "provider": "local-test",
            "model": "short-timeout" if case == "model-timeout" else "long-timeout",
            "stream": True,
            "content": case,
        }).encode()
        request = (
            f"POST /api/chat HTTP/1.1\r\nHost: {HOST}:{self.port}\r\n"
            f"Cookie: {self.cookie}\r\nX-Chat-Request: 1\r\nContent-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n\r\n"
        ).encode() + body
        with socket.socket() as peer:
            if receive_buffer is not None:
                peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
            peer.settimeout(10)
            peer.connect((HOST, self.port))
            started = time.monotonic()
            peer.sendall(request)
            with peer.makefile("rb") as wire:
                status = wire.readline(8193)
                headers = http.client.parse_headers(wire)
                require(status.startswith(b"HTTP/1.1 200 ") and
                        headers.get("Transfer-Encoding", "").lower() == "chunked" and
                        headers.get("Content-Type", "").startswith("text/event-stream"),
                        f"{case}: expected HTTP 200 with chunked SSE, got {status!r} {headers}")
                yield conversation, peer, wire, started


@contextmanager
def chat_server(binary, model, mysql_port, redis_port, *, server_options=None, models=None):
    with tempfile.TemporaryDirectory(prefix="chat-sse-") as directory:
        with socket.socket() as reservation:
            reservation.bind((HOST, 0))
            port = reservation.getsockname()[1]
        config = json.loads((ROOT / "config.json.example").read_text())
        config["server"].update(host=HOST, port=port, io_threads=2, worker_threads=6, business_threads=2)
        if server_options:
            config["server"].update(server_options)
        config["mysql"].update(
            port=mysql_port, database="sse_test", username="sse_test", password=DB_PASSWORD, pool_size=4
        )
        config["redis"].update(port=redis_port, pool_size=4)
        namespace = f"sse-test:{uuid.uuid4().hex}"
        config["auth"].update(secure_cookie=False, key_namespace=f"{namespace}:auth",
                              session_namespace=f"{namespace}:session")
        config["models"] = models if models is not None else [
            {
                "provider": "local-test", "model": name, "api": "openai-completions",
                "base_url": f"http://{HOST}:{model.server_port}/v1",
                "api_key": "local-sse-regression", "timeout_ms": timeout * 1000,
                "max_response_bytes": 16 * 1024 * 1024,
            }
            for name, timeout in [("long-timeout", IDLE_SECONDS + 25), ("short-timeout", MODEL_TIMEOUT_SECONDS)]
        ]
        (Path(directory) / "config.json").write_text(json.dumps(config))
        env = os.environ.copy()
        # libcurl prefers the lowercase variable when both are set.
        excluded = env.get("no_proxy") or env.get("NO_PROXY")
        if excluded != "*":
            excluded = f"{HOST},{excluded}" if excluded else HOST
        env["no_proxy"] = env["NO_PROXY"] = excluded
        with tempfile.TemporaryFile(mode="w+t") as log:
            process = subprocess.Popen([binary], cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT)
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
                    chat = Chat(port, process)
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


def chunk_payloads(wire):
    while True:
        line = wire.readline(8193)
        size = line[:-2].split(b";", 1)[0]
        require(line.endswith(b"\r\n") and re.fullmatch(rb"[0-9a-fA-F]+", size),
                "missing or malformed HTTP chunk size line")
        remaining = int(size, 16)
        if not remaining:
            # The zero chunk may carry trailers, but always ends with a blank CRLF line.
            while True:
                trailer = wire.readline(8193)
                if trailer == b"\r\n":
                    return
                require(trailer.endswith(b"\r\n") and b":" in trailer,
                        "incomplete or malformed final HTTP chunk")
        while remaining:
            payload = wire.read1(min(remaining, 4096))
            require(payload, "truncated HTTP chunk payload")
            remaining -= len(payload)
            yield payload
        require(wire.read(2) == b"\r\n", "missing HTTP chunk payload delimiter")


def frames(wire, started):
    fields = {}
    pending = b""
    for data in chunk_payloads(wire):
        require(time.monotonic() - started < IDLE_SECONDS + 15, "SSE did not finish within the test window")
        pending += data
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            line = raw.decode().rstrip("\r")
            if not line and fields:
                yield time.monotonic() - started, fields
                fields = {}
            elif line.startswith(":"):
                fields["comment"] = line[1:].strip()
            elif ":" in line:
                name, value = line.split(":", 1)
                fields[name] = value.strip()
    require(not pending and not fields, "SSE ended mid-event")


def completed_stream(chat, case):
    with chat.stream(case) as (_, _, response, started):
        observed = list(frames(response, started))
    events = [frame["event"] for _, frame in observed if "event" in frame]
    expected_delta_count = 17 if case == "body" else 1
    require(events == ["meta", *(["delta"] * expected_delta_count), "done"],
            f"expected meta, {expected_delta_count} deltas and done, got {events}")
    deltas = [(elapsed, json.loads(frame["data"])["text"]) for elapsed, frame in observed
              if frame.get("event") == "delta"]
    finished = observed[-1][0]
    require(finished > IDLE_SECONDS, f"done arrived at {finished:.2f}s")
    if case == "body":
        require([part for _, part in deltas] == [f"{i}," for i in range(17)], "body deltas missing or out of order")
        require(deltas[0][0] < 5 and deltas[-1][0] > IDLE_SECONDS, "body did not span idle threshold")
        return f"17 deltas, done at {finished:.2f}s"
    pings = [elapsed for elapsed, frame in observed if frame.get("comment") == "ping"]
    require(len(deltas) == 1 and deltas[0][1] == "delayed reply" and deltas[0][0] > IDLE_SECONDS,
            f"expected one delayed delta, observed {deltas}")
    require(any(elapsed < IDLE_SECONDS for elapsed in pings) and
            any(IDLE_SECONDS < elapsed < deltas[0][0] for elapsed in pings),
            f"pings did not span idle threshold ({len(pings)} observed)")
    return f"{len(pings)} pings, done at {finished:.2f}s"


def idle_connection(chat, case):
    with socket.create_connection((HOST, chat.port), timeout=IDLE_SECONDS + 4) as peer:
        started = time.monotonic()
        if case == "partial":
            peer.sendall(b"POST /api/chat HTTP/1.1\r\nHost: localhost\r\nContent-Length: 32\r\n\r\n{")
            time.sleep(4)
            started = time.monotonic()
            peer.sendall(b'"content"')
        require(peer.recv(1) == b"", "idle connection returned data instead of EOF")
        elapsed = time.monotonic() - started
        require(IDLE_SECONDS - 0.5 <= elapsed <= IDLE_SECONDS + 3,
                f"EOF {elapsed:.2f}s after last activity, expected about {IDLE_SECONDS}s")
    return f"EOF {elapsed:.2f}s after last activity"


def model_timeout(chat):
    with chat.stream("model-timeout") as (conversation, _, response, started):
        observed = [(elapsed, frame["event"], json.loads(frame["data"]))
                    for elapsed, frame in frames(response, started) if "event" in frame]
    require([event for _, event, _ in observed] == ["meta", "delta", "error"],
            f"expected meta, delta, error; got {observed}")
    elapsed, _, error = observed[-1]
    require(MODEL_TIMEOUT_SECONDS - 0.25 <= elapsed < MODEL_TIMEOUT_SECONDS + 5 and
            error["code"] == "generation_failed" and TIMEOUT_MESSAGE in error["error"],
            f"expected model timeout SSE error at about {MODEL_TIMEOUT_SECONDS}s, got {error} at {elapsed:.2f}s")
    require(observed[1][2]["text"] == "partial reply", "missing streamed partial reply")
    result, _ = chat.api(f"/api/conversation?id={conversation}", 200)
    assistant_id = observed[0][2]["assistant_message_id"]
    reply = next(item for item in result["messages"] if item["id"] == assistant_id)
    require(result["conversation"]["active_message_id"] is None and reply["state"] == "failed" and
            reply["content"] == "partial reply" and TIMEOUT_MESSAGE in reply["error"],
            f"expected a failed reply with saved partial text and released generation slot, got {reply}")
    return f"error at {elapsed:.2f}s, failed reply and no active generation"


def server_tcp_socket(pid, server_port, client_port):
    # Match the accepted loopback connection in /proc/net/tcp by its endpoints.
    address = int.from_bytes(socket.inet_aton(HOST), sys.byteorder)
    local, peer = f"{address:08X}:{server_port:04X}", f"{address:08X}:{client_port:04X}"
    for row in Path(f"/proc/{pid}/net/tcp").read_text().splitlines()[1:]:
        fields = row.split()
        if fields[1] == local and fields[2] == peer:
            pending = int(fields[4].split(":")[0], 16)
            zero_window = fields[5].startswith("04:")
            return int(fields[9]), pending, zero_window
    return None


def backpressure(chat, case, pressure):
    try:
        with chat.stream(case, receive_buffer=4096) as (_, peer, response, started):
            for _, frame in frames(response, started):
                if frame.get("event") == "meta":
                    break
            else:
                raise AssertionError("backpressure stream ended before meta")
            client_port = peer.getsockname()[1]
            pid = chat.process.pid
            pressure.fill.set()

            def blocked_socket():
                state = server_tcp_socket(pid, chat.port, client_port)
                # Allow room for the client's 8 KiB receive buffer and SSE framing.
                if state and state[0] and state[2] and pressure.payload_bytes - state[1] >= 128 * 1024:
                    return state
                return None

            inode, pending, _ = wait_for("zero window with unwritten SSE data", 15, blocked_socket)
            blocked = time.monotonic()
            pressure.blocked.set()
            queued_at_block = pressure.payload_bytes
            last_read = blocked
            reads = 0
            if case == "slow-reader":
                peer.setblocking(False)

            def released():
                nonlocal reads, last_read
                require(chat.process.poll() is None, "server exited while the socket was blocked")
                state = server_tcp_socket(pid, chat.port, client_port)
                if not state or state[0] != inode:
                    return time.monotonic()
                require(state[1] == pending and state[2], f"TCP write queue moved after the receive window closed: {state}")
                now = time.monotonic()
                if case == "slow-reader" and now - last_read >= 1:
                    # The HTTP response parser is no longer in use. Consume a byte from the
                    # kernel receive buffer without opening a usable TCP receive window.
                    require(peer.recv(1) != b"", "slow reader encountered unexpected EOF")
                    last_read = now
                    reads += 1
                return None

            finished = wait_for("release of the blocked socket", IDLE_SECONDS + 3, released)
            elapsed = finished - blocked
            require(elapsed >= IDLE_SECONDS - 2,
                    f"blocked socket released early, {elapsed:.2f}s after observing zero window")
            require(pressure.payload_bytes - queued_at_block >= 50 * 1024 and
                    pressure.last_pulse is not None and finished - pressure.last_pulse < 2,
                    "mock model stopped enqueueing more data before the idle deadline")
            if case == "slow-reader":
                require(reads >= 45, f"slow reader consumed only {reads} bytes")
            return f"zero window, {pending} pending bytes, read {reads} more bytes, released after {elapsed:.2f}s"
    finally:
        pressure.stop.set()


def run_backend(name, binary, database_ports, cases):
    with model_server() as model, chat_server(binary, model, *database_ports) as chat:
        checks = {
            "body": lambda: completed_stream(chat, "body"),
            "heartbeat": lambda: completed_stream(chat, "heartbeat"),
            "silent": lambda: idle_connection(chat, "silent"),
            "partial": lambda: idle_connection(chat, "partial"),
            "backpressure": lambda: backpressure(chat, "backpressure", model.pressure["backpressure"]),
            "slow-reader": lambda: backpressure(chat, "slow-reader", model.pressure["slow-reader"]),
            "model-timeout": lambda: model_timeout(chat),
        }
        failures = 0
        with ThreadPoolExecutor(max_workers=len(cases)) as pool:
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
        parser.add_argument(f"--{backend}", type=Path, required=True, help=f"path to the {backend} server")
    parser.add_argument("--case", action="append", choices=CASES, help="run only this case (repeatable)")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("requires Linux for the Proactor backend and TCP queue inspection")
    servers = {name: getattr(args, name).resolve() for name in ("reactor", "proactor")}
    for name, binary in servers.items():
        if not binary.is_file():
            parser.error(f"{name} server does not exist: {binary}")
        try:
            reported = subprocess.run([binary, "--network-backend"], check=True,
                                      capture_output=True, text=True, timeout=5).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            parser.error(f"cannot query the network backend of {binary}; rebuild server from this checkout")
        if reported != name:
            parser.error(f"--{name} expects a {name} server, but {binary} reports {reported!r}")
    cases = tuple(dict.fromkeys(args.case)) if args.case else CASES
    with ExitStack() as stack:
        database_ports = start_databases(stack)
        failures = 0
        with ThreadPoolExecutor(max_workers=len(servers)) as pool:
            runs = {pool.submit(run_backend, name, binary, database_ports, cases): name
                    for name, binary in servers.items()}
            for future in as_completed(runs):
                try:
                    failures += future.result()
                except Exception as error:
                    print(f"FAIL {runs[future]} setup: {error}", flush=True)
                    failures += len(cases)
    print(f"{len(servers) * len(cases) - failures} passed, {failures} failed", flush=True)
    return int(failures > 0)


if __name__ == "__main__":
    raise SystemExit(main())
