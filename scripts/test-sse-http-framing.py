#!/usr/bin/env python3
"""Exercise the SSE regression client's HTTP framing over loopback."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import runpy
import threading
import unittest


RUNNER = runpy.run_path(str(Path(__file__).with_name("test-sse-idle-timeout.py")))
DONE = b"event: done\ndata: {}\n\n"


def chunk(data, extension=b""):
    return f"{len(data):x}".encode() + extension + b"\r\n" + data + b"\r\n"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers["Content-Length"]))
        if self.path == "/api/conversations":
            body = b'{"conversation":{"id":"1"}}'
            self.send_response_only(201)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/api/chat":
            self.wfile.write(
                b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n" + self.server.body
            )
        else:
            self.send_error(404)


class FramingTest(unittest.TestCase):
    def events(self, body):
        with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            server.body = body
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01})
            thread.start()
            try:
                chat = RUNNER["Chat"](server.server_port, None)
                with chat.stream("body") as (_, _, response, started):
                    return [frame for _, frame in RUNNER["frames"](response, started)]
            finally:
                server.shutdown()
                thread.join()

    def test_done_with_complete_final_chunk(self):
        self.assertEqual(self.events(chunk(DONE) + b"0\r\n\r\n"), [{"event": "done", "data": "{}"}])

    def test_done_without_complete_final_chunk(self):
        for suffix in (b"", b"0", b"0\r\n", b"0\r\n\r", b"0\r\nX-Test: value\r\n"):
            with self.subTest(suffix=suffix), self.assertRaisesRegex(AssertionError, "HTTP chunk"):
                self.events(chunk(DONE) + suffix)

    def test_fragmented_event_with_chunk_extension_and_trailer(self):
        body = chunk(DONE[:8], b";test=1") + chunk(DONE[8:]) + b"0\r\nX-Test: value\r\n\r\n"
        self.assertEqual(self.events(body), [{"event": "done", "data": "{}"}])


if __name__ == "__main__":
    unittest.main()
