#!/usr/bin/env python3
"""Cross-stack interop test driver: starts the wtransport echo server
(tests/wt-echo, Rust), waits for its certificate hash, then runs the echo
client passed as argv[1] against it.

    python3 tests/wt_interop.py <path-to-ngh_wt_echo> [port]

Exit code is the client's. Needs cargo; the first run compiles the server.
"""

import re
import subprocess
import sys
import time
from pathlib import Path

PORT_DEFAULT = 4437


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: wt_interop.py <echo-client> [port]", file=sys.stderr)
        return 2
    client = sys.argv[1]
    port = sys.argv[2] if len(sys.argv) > 2 else str(PORT_DEFAULT)
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
        return subprocess.run(
            [client, f"https://localhost:{port}/", cert_hash],
            timeout=60).returncode
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


if __name__ == "__main__":
    sys.exit(main())
