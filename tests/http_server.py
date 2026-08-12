#!/usr/bin/env python3
"""Loopback HTTP server for tests/test_http.c."""

import http.server
import time
from urllib.parse import urlsplit


GET_BODY = b"ngh-http-get\n"


class ContractHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format_string, *args):
        del format_string, args

    def _send(self, status, body=b"", headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        if body:
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

    def _read_body(self):
        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            return b""
        try:
            length = int(raw_length)
        except ValueError:
            return None
        if length < 0:
            return None
        return self.rfile.read(length)

    def _echo(self):
        request_body = self._read_body()
        if request_body is None:
            self._send(400)
            return
        chunks = [f"METHOD {self.command}\n".encode("ascii")]
        for name, value in self.headers.raw_items():
            chunks.append(f"HEADER {name}: {value}\n".encode("latin-1"))
        chunks.extend((b"\n", request_body))
        self._send(
            200,
            b"".join(chunks),
            (("content-type", "application/octet-stream"),),
        )

    def _route(self):
        path = urlsplit(self.path).path

        if path == "/get":
            self._send(200, GET_BODY, (("content-type", "text/plain"),))
            return
        if path == "/echo":
            self._echo()
            return
        if path.startswith("/status/"):
            try:
                status = int(path[len("/status/") :])
            except ValueError:
                self._send(404)
                return
            if 100 <= status <= 599:
                self._send(status)
            else:
                self._send(404)
            return
        if path == "/redirect":
            self._send(302, headers=(("Location", "/get"),))
            return
        if path == "/repeat":
            self._send(
                200,
                headers=(
                    ("X-Multi", "a"),
                    ("X-Multi", "b"),
                    ("Set-Cookie", "a=1"),
                    ("Set-Cookie", "b=2"),
                ),
            )
            return
        if path.startswith("/big/"):
            try:
                size = int(path[len("/big/") :])
            except ValueError:
                self._send(404)
                return
            if size < 0:
                self._send(404)
            else:
                self._send(200, b"x" * size)
            return
        if path == "/slow":
            time.sleep(2)
            self._send(200, b"slow\n", (("content-type", "text/plain"),))
            return
        self._send(404)

    def do_GET(self):
        self._route()

    def do_POST(self):
        self._route()


class ContractServer(http.server.ThreadingHTTPServer):
    daemon_threads = True


def main():
    server = ContractServer(("127.0.0.1", 0), ContractHandler)
    print(f"PORT {server.server_port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
