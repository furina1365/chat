#!/usr/bin/env python3
"""Exercise chat cancellation against both locally built network backends."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import http.client
import json
from pathlib import Path
import runpy
import select
import socket
import struct
import subprocess
import threading
import time
import uuid


SUITE = runpy.run_path(str(Path(__file__).with_name("test-sse-idle-timeout.py")))
HOST = SUITE["HOST"]
require = SUITE["require"]
RELEASE_LIMIT = 0.3
PARTIAL = b'data: {"choices":[{"delta":{"content":"partial"}}]}\n\n'


class Call:
    def __init__(self, partial):
        self.partial = partial
        self.started = threading.Event()
        self.complete = threading.Event()
        self.released = threading.Event()
        self.started_at = None
        self.released_at = None


class Model(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        case = request["messages"][-1]["content"]
        call = self.server.calls[case]
        require(not call.started.is_set(), f"unexpected duplicate upstream call for {case}")
        stream = request["stream"]
        if stream:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            if call.partial:
                self.wfile.write(PARTIAL)
        call.started_at = time.monotonic()
        call.started.set()
        try:
            while not self.server.stopping.is_set():
                if call.complete.is_set():
                    self.reply(stream)
                    return
                if select.select([self.connection], [], [], 0.02)[0] and not self.connection.recv(1):
                    break
            else:
                return
        except (BrokenPipeError, ConnectionResetError):
            pass
        call.released_at = time.monotonic()
        call.released.set()

    def reply(self, stream):
        if stream:
            self.wfile.write(b'data: {"choices":[{"delta":{"content":"reply"},"finish_reason":"stop"}]}\n\n'
                             b'data: [DONE]\n\n')
        else:
            body = b'{"choices":[{"message":{"content":"reply"},"finish_reason":"stop"}]}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)


@contextmanager
def model_server():
    with ThreadingHTTPServer((HOST, 0), Model) as server:
        server.calls = {}
        server.stopping = threading.Event()
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
        thread.start()
        try:
            yield server
        finally:
            server.stopping.set()
            server.shutdown()
            thread.join()


def turn(chat, model, name, stream=True, conversation=None, partial=False):
    if conversation is None:
        data, _ = chat.api("/api/conversations", 201, {"title": name})
        conversation = data["conversation"]["id"]
    call = model.calls[name] = Call(partial)
    return conversation, call, {
        "conversation_id": conversation, "request_id": uuid.uuid4().hex,
        "provider": "local-test", "model": "long-timeout", "stream": stream, "content": name,
    }


def send_chat(chat, peer, request):
    body = json.dumps(request).encode()
    peer.sendall((
        f"POST /api/chat HTTP/1.1\r\nHost: {HOST}:{chat.port}\r\n"
        f"Cookie: {chat.cookie}\r\nX-Chat-Request: 1\r\nContent-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\n\r\n"
    ).encode() + body)


def reset(peer):
    peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    started = time.monotonic()
    peer.close()
    return started


def check_release(call, started):
    require(call.released.wait(2), "upstream connection stayed open for 2 seconds")
    latency = call.released_at - started
    require(0 <= latency <= RELEASE_LIMIT, f"upstream released at {latency:.3f}s (limit {RELEASE_LIMIT:.3f}s)")
    return latency


def accepted_socket(chat, client_port):
    pid = chat.process.pid
    connection = SUITE["server_tcp_socket"](pid, chat.port, client_port)
    require(connection and connection[0], "could not identify the accepted socket")
    inode = Path(f"socket:[{connection[0]}]")
    for fd in Path(f"/proc/{pid}/fd").iterdir():
        try:
            if fd.readlink() == inode:
                return fd, inode
        except FileNotFoundError:
            continue
    raise AssertionError("the server no longer owns the accepted socket")


def check_socket_close(accepted, started):
    fd, inode = accepted
    while time.monotonic() - started <= RELEASE_LIMIT:
        try:
            still_open = fd.readlink() == inode
        except FileNotFoundError:
            still_open = False
        if not still_open:
            return time.monotonic() - started
        time.sleep(0.005)
    raise AssertionError(f"server still owns the socket fd after {RELEASE_LIMIT:.3f}s")


def check_history(chat, conversation, state, content=None):
    def finished():
        data, _ = chat.api(f"/api/conversation?id={conversation}", 200)
        return data if data["conversation"]["active_message_id"] is None else None

    data = SUITE["wait_for"]("released conversation", 2, finished)
    message = data["messages"][-1]
    require(message["state"] == state, f"expected {state} reply, got {message['state']}")
    if content is not None:
        require(message["content"] == content, f"unexpected saved content {message['content']!r}")


def reset_while_waiting(chat, model, half_closed):
    name = "half-close-then-rst" if half_closed else "first-token-rst"
    conversation, call, request = turn(chat, model, name, stream=not half_closed)
    with socket.create_connection((HOST, chat.port), timeout=3) as peer:
        send_chat(chat, peer, request)
        require(call.started.wait(2), "model never received request")
        if half_closed:
            peer.shutdown(socket.SHUT_WR)
        # Let the server process the FIN and the upstream enter its quiet wait.
        time.sleep(0.1)
        accepted = accepted_socket(chat, peer.getsockname()[1])
        started = reset(peer)
        fd_latency = check_socket_close(accepted, started)
        latency = check_release(call, started)
    check_history(chat, conversation, "interrupted")
    return f"fd closed in {fd_latency:.3f}s, upstream released in {latency:.3f}s, conversation cleared"


def legal_half_close(chat, model):
    for stream, partial in ((False, False), (True, False), (True, True)):
        conversation, call, request = turn(chat, model, f"legal-half-close-{stream}-{partial}",
                                           stream=stream, partial=partial)
        with socket.create_connection((HOST, chat.port), timeout=3) as peer:
            send_chat(chat, peer, request)
            if not partial:
                peer.shutdown(socket.SHUT_WR)
                require(call.started.wait(2), "FIN cancelled the request before the worker started")
                require(not call.released.wait(0.15), "FIN cancelled a client that was still reading")
                call.complete.set()
            with http.client.HTTPResponse(peer) as response:
                response.begin()
                require(response.status == 200, f"unexpected status {response.status}")
                if stream:
                    first = b""
                    if partial:
                        for line in response:
                            first += line
                            if line == b'data: {"text":"partial"}\n':
                                break
                        require(b'"text":"partial"' in first, "partial reply was not received")
                        peer.shutdown(socket.SHUT_WR)
                        require(not call.released.wait(0.15), "FIN cancelled an in-progress reply")
                        call.complete.set()
                    body = (first + response.read()).decode()
                    events = [line for line in body.splitlines() if line.startswith("event: ")]
                    require(events == ["event: meta", *(["event: delta"] * (2 if partial else 1)), "event: done"],
                            str(events))
                else:
                    require(json.load(response)["reply"] == "reply", "incomplete JSON reply")
        check_history(chat, conversation, "completed", "partialreply" if partial else "reply")
    return "complete JSON and SSE, including FIN after a partial reply"


def disconnect_from_stream(chat, model, name, *, partial, use_reset=False):
    conversation, call, request = turn(chat, model, name, partial=partial)
    marker = "delta" if partial else "meta"
    with socket.create_connection((HOST, chat.port), timeout=3) as peer:
        send_chat(chat, peer, request)
        with peer.makefile("rb") as wire:
            require(wire.readline().startswith(b"HTTP/1.1 200 "), "SSE request did not start")
            http.client.parse_headers(wire)
            observed = SUITE["frames"](wire, time.monotonic())
            first = next(frame for _, frame in observed if frame.get("event") == marker)
            if partial:
                require(json.loads(first["data"])["text"] == "partial", "expected a partial reply")
        require(call.started.wait(2), "model never received request")
        if use_reset:
            started = reset(peer)
        else:
            started = time.monotonic()
            peer.close()
        latency = check_release(call, started)
    check_history(chat, conversation, "interrupted", "partial" if partial else "")
    return f"upstream released in {latency:.3f}s, history verified"


ABORT_FETCH = """
import { channel } from 'node:diagnostics_channel';
import { once } from 'node:events';
const [url, cookie, body, marker] = process.argv.slice(1);
const go = once(process.stdin, 'data');
let clientPort;
channel('undici:client:connected').subscribe(({socket}) => { clientPort = socket.localPort; });
const controller = new AbortController();
const response = await fetch(url, {
  method: 'POST', body, signal: controller.signal,
  headers: {'Content-Type': 'application/json', 'X-Chat-Request': '1', Cookie: cookie},
});
if (!response.ok) throw new Error(`chat returned HTTP ${response.status}`);
const reader = response.body.getReader();
const decoder = new TextDecoder();
let text = '';
while (!text.includes(marker)) {
  const {done, value} = await reader.read();
  if (done) throw new Error('SSE ended before the expected frame');
  text += decoder.decode(value, {stream: true});
}
if (!clientPort) throw new Error('fetch did not report its local socket port');
console.log(`ready ${clientPort}`);
await go;
controller.abort();
try {
  await reader.read();
  throw new Error('fetch was not aborted');
} catch (error) {
  if (error.name !== 'AbortError') throw error;
}
process.stdin.destroy();
"""


@contextmanager
def fetch_ready_to_abort(chat, request, marker):
    command = ["node", "--input-type=module", "-e", ABORT_FETCH, f"http://{HOST}:{chat.port}/api/chat",
               chat.cookie, json.dumps(request), marker]
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True) as client:
        try:
            readable, _, _ = select.select([client.stdout], [], [], 3)
            ready = client.stdout.readline().split() if readable else []
            require(len(ready) == 2 and ready[0] == "ready" and ready[1].isdigit(),
                    "fetch did not receive the expected frame and report its socket port")
            yield client, int(ready[1])
            _, stderr = client.communicate(timeout=3)
            require(client.returncode == 0, f"AbortController failed: {stderr}")
        finally:
            if client.poll() is None:
                client.kill()


def abort_client(client):
    started = time.monotonic()
    client.stdin.write("abort\n")
    client.stdin.flush()
    return started


def abort_fetch(chat, model, partial):
    name = "abort-fetch" if partial else "first-token-abort"
    conversation, call, request = turn(chat, model, name, partial=partial)
    marker = 'event: delta\ndata: {"text":"partial"}\n\n' if partial else "event: meta\n"
    with fetch_ready_to_abort(chat, request, marker) as (client, port):
        require(call.started.wait(2), "model never received request")
        accepted = accepted_socket(chat, port)
        started = abort_client(client)
        fd_latency = check_socket_close(accepted, started)
        latency = check_release(call, started)
    check_history(chat, conversation, "interrupted", "partial" if partial else "")
    return f"fd closed in {fd_latency:.3f}s, upstream released in {latency:.3f}s after AbortController.abort()"


def queued(chat, model, mode):
    name = f"queue-{mode}"
    cancel = mode != "order"
    _, running, first = turn(chat, model, f"{name}-running", stream=False)
    conversation, pending, second = turn(chat, model, name)
    with socket.create_connection((HOST, chat.port), timeout=3) as active_peer:
        send_chat(chat, active_peer, first)
        require(running.started.wait(2), "first request did not occupy the only worker")
        with socket.create_connection((HOST, chat.port), timeout=2) as queued_peer:
            send_chat(chat, queued_peer, second)
            with queued_peer.makefile("rb") as wire:
                require(wire.readline().startswith(b"HTTP/1.1 200 "), "queued request did not start SSE")
                headers = http.client.parse_headers(wire)
                require(headers.get("transfer-encoding") == "chunked", "queued SSE is not chunked")
                frames = SUITE["frames"](wire, time.monotonic())
                require(next(frames)[1] == {"comment": "connected"}, "SSE must start with the connected comment")
                require(not pending.started.is_set(), "queued request got a busy worker")
                _, rejected, third = turn(chat, model, f"{name}-rejected")
                rejection, _ = chat.api("/api/chat", 503, third)
                require("队列" in rejection["error"] and not rejected.started.is_set(), "queue-full JSON response changed")
                if not cancel:
                    pending.complete.set()
                    running.complete.set()
                    events = [frame["event"] for _, frame in frames if "event" in frame]
                    require(events == ["meta", "delta", "done"], f"unexpected queued SSE events {events}")
            if cancel:
                accepted = accepted_socket(chat, queued_peer.getsockname()[1])
                if mode == "rst":
                    started = reset(queued_peer)
                else:
                    started = time.monotonic()
                    queued_peer.close()
                fd_latency = check_socket_close(accepted, started)
        if cancel:
            check_history(chat, conversation, "interrupted", "")
            require(not pending.started.is_set(), "cancelled queued job called upstream")
            _, following, request = turn(chat, model, f"{name}-retry", conversation=conversation)
            with socket.create_connection((HOST, chat.port), timeout=3) as retry_peer:
                send_chat(chat, retry_peer, request)
                with retry_peer.makefile("rb") as wire:
                    status = wire.readline()
                    require(status.startswith(b"HTTP/1.1 200 "), f"retry could not enter the queue: {status!r}")
                    http.client.parse_headers(wire)
                    frames = SUITE["frames"](wire, time.monotonic())
                    require(next(frames)[1] == {"comment": "connected"}, "retry was not admitted")
                    require(not running.released.is_set() and not following.started.is_set(),
                            "original call was no longer holding the worker")
                    following.complete.set()
                    running.complete.set()
                    events = [frame["event"] for _, frame in frames if "event" in frame]
                    require(events == ["meta", "delta", "done"], f"unexpected retry events {events}")
        with http.client.HTTPResponse(active_peer) as response:
            response.begin()
            require(response.status == 200 and json.load(response)["reply"] == "reply", "running request did not finish")

    if cancel:
        require(not pending.started.is_set(), "cancelled queued job called upstream")
        check_history(chat, conversation, "completed", "reply")
        return f"fd closed in {fd_latency:.3f}s, queue slot and conversation freed while worker was busy"
    check_history(chat, conversation, "completed", "reply")
    return "503 on saturation, connected comment before meta, delta and done"


def queued_fetch(chat, model):
    _, running, first = turn(chat, model, "queue-abort-running", stream=False)
    conversation, pending, second = turn(chat, model, "queue-abort")
    with socket.create_connection((HOST, chat.port), timeout=3) as peer:
        send_chat(chat, peer, first)
        require(running.started.wait(2), "first request did not occupy the only worker")
        with fetch_ready_to_abort(chat, second, ": connected\n\n") as (client, port):
            require(not pending.started.is_set(), "queued request got a busy worker")
            accepted = accepted_socket(chat, port)
            fd_latency = check_socket_close(accepted, abort_client(client))
        check_history(chat, conversation, "interrupted", "")
        running.complete.set()
        with http.client.HTTPResponse(peer) as response:
            response.begin()
            require(response.status == 200, "running request did not finish")
            response.read()
    require(not pending.started.is_set(), "cancelled queued fetch called upstream")
    return f"fd closed in {fd_latency:.3f}s while queued, AbortController.abort() made no model call"


def worker_released(chat, model):
    conversation, running, first = turn(chat, model, "worker-cancel-running")
    _, pending, second = turn(chat, model, "worker-cancel-following")
    with socket.create_connection((HOST, chat.port), timeout=3) as peer:
        send_chat(chat, peer, first)
        require(running.started.wait(2), "first request did not occupy the only worker")
        with socket.create_connection((HOST, chat.port), timeout=3) as next_peer:
            send_chat(chat, next_peer, second)
            with next_peer.makefile("rb") as wire:
                require(wire.readline().startswith(b"HTTP/1.1 200 "), "next request did not start SSE")
                http.client.parse_headers(wire)
                frames = SUITE["frames"](wire, time.monotonic())
                require(next(frames)[1] == {"comment": "connected"}, "request was not admitted")
                require(not pending.started.is_set(), "queued request got a busy worker")
                started = reset(peer)
                require(pending.started.wait(2), "worker did not start the next upstream call")
                latency = pending.started_at - started
                require(0 <= latency <= RELEASE_LIMIT, f"worker took {latency:.3f}s to start the next request")
                pending.complete.set()
                events = [frame["event"] for _, frame in frames if "event" in frame]
                require(events == ["meta", "delta", "done"], f"unexpected next reply events {events}")
    check_history(chat, conversation, "interrupted")
    return f"worker started the next queued call in {latency:.3f}s"


CASES = {
    "first-token-rst": lambda chat, model: reset_while_waiting(chat, model, False),
    "half-close-then-rst": lambda chat, model: reset_while_waiting(chat, model, True),
    "legal-half-close": legal_half_close,
    "first-token-fin": lambda chat, model: disconnect_from_stream(chat, model, "first-token-fin", partial=False),
    "first-token-abort": lambda chat, model: abort_fetch(chat, model, False),
    "partial-rst": lambda chat, model: disconnect_from_stream(chat, model, "partial-rst", partial=True, use_reset=True),
    "partial-fin": lambda chat, model: disconnect_from_stream(chat, model, "partial-fin", partial=True),
    "abort-fetch": lambda chat, model: abort_fetch(chat, model, True),
    "queue-cancel": lambda chat, model: queued(chat, model, "fin"),
    "queue-rst": lambda chat, model: queued(chat, model, "rst"),
    "queue-abort": queued_fetch,
    "queue-order": lambda chat, model: queued(chat, model, "order"),
    "worker-release": worker_released,
}


def run_backend(name, binary, ports, cases):
    failures = 0
    options = {"worker_threads": 1, "model_queue_size": 1}
    for case in cases:
        try:
            with model_server() as model, SUITE["chat_server"](binary, model, *ports, server_options=options) as chat:
                result = CASES[case](chat, model)
            print(f"PASS {name}/{case}: {result}", flush=True)
        except Exception as error:
            failures += 1
            print(f"FAIL {name}/{case}: {error}", flush=True)
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
    with ExitStack() as stack, ThreadPoolExecutor(max_workers=2) as pool:
        ports = SUITE["start_databases"](stack)
        futures = [pool.submit(run_backend, name, binary, ports, cases) for name, binary in backends.items()]
        failures = sum(future.result() for future in futures)
    print(f"{2 * len(cases) - failures} passed, {failures} failed")
    return int(failures > 0)


if __name__ == "__main__":
    raise SystemExit(main())
