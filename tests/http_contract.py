#!/usr/bin/env python3
"""Run the ngh_http contract binary against its loopback test server."""

import re
import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: http_contract.py <test-http-binary>", file=sys.stderr)
        return 2

    test_binary = sys.argv[1]
    server_script = Path(__file__).resolve().with_name("http_server.py")
    server = subprocess.Popen(
        [sys.executable, str(server_script)],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    try:
        line = server.stdout.readline()
        match = re.fullmatch(r"PORT ([0-9]+)\n?", line)
        if match is None:
            if server.poll() is None:
                print("server did not print a valid PORT line", file=sys.stderr)
            else:
                print("server exited before printing its port", file=sys.stderr)
            return 1
        port = match.group(1)
        return subprocess.run(
            [test_binary, f"http://127.0.0.1:{port}"], timeout=240
        ).returncode
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()


if __name__ == "__main__":
    sys.exit(main())
