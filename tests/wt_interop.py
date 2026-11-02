#!/usr/bin/env python3
"""Cross-stack interop test driver: starts the wtransport echo server
(tests/wt-echo, Rust), waits for its certificate hash, then runs each
client passed on the command line against it, in order.

    python3 tests/wt_interop.py <client> [<client>...] [port]

Every client is called as `<client> <url> <cert_sha256_hex>`. Exit code is
the first failing client's, else 0. Needs cargo; the first run compiles
the server.
"""

import re
import subprocess
import sys
import time
from pathlib import Path

PORT_DEFAULT = 4437


def main() -> int:
    args = sys.argv[1:]
    port = str(PORT_DEFAULT)
    if args and args[-1].isdigit():
        port = args.pop()
    clients = args
    if not clients:
        print("usage: wt_interop.py <client> [<client>...] [port]",
              file=sys.stderr)
        return 2
    src = Path(__file__).resolve().parent / "wt-echo"

    server = subprocess.Popen(
        ["cargo", "run", "--release", "--quiet", "--", port],
        cwd=src, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    try:
        cert_hash = None
        deadline = time.time() + 600  # the first run compiles dependencies
        while time.time() < deadline:
            line = server.stdout.readline()
            if not line:
                print("server exited before printing its hash",
                      file=sys.stderr)
                return 1
            m = re.match(r"cert_sha256=([0-9a-f]{64})", line)
            if m:
                cert_hash = m.group(1)
                break
        if cert_hash is None:
            print("timed out waiting for the server", file=sys.stderr)
            return 1
        for client in clients:
            print(f"--- {client}", flush=True)
            rc = subprocess.run(
                [client, f"https://localhost:{port}/", cert_hash],
                timeout=60).returncode
            if rc != 0:
                return rc
        return 0
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


if __name__ == "__main__":
    sys.exit(main())
