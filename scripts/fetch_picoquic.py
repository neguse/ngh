#!/usr/bin/env python3
"""Fetch and build the pinned picoquic that backend/wt_backend.c links.

    python3 scripts/fetch_picoquic.py

Clones picoquic at the pinned commit into vendor/picoquic, applies
backend/picoquic_wt_compat.patch (see the patch for why), and builds the
static libraries with -fPIC so the backend shared library can link them.
picotls is pinned by picoquic's own CMake (PICOQUIC_FETCH_PTLS); OpenSSL
comes from the system.

The result feeds the `ngh_wt_backend` CMake target; end users never run
this -- they download the prebuilt backend from the ngh release instead.
"""

import os
import subprocess
import sys
from pathlib import Path

PICOQUIC_REPO = "https://github.com/private-octopus/picoquic.git"
PICOQUIC_COMMIT = "467cb81bcafc652bae2a1c8824b70f12f470039c"


def run(cmd, cwd=None):
    print(f"  $ {' '.join(cmd)}")
    subprocess.run(cmd, cwd=cwd, check=True)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    src = root / "vendor" / "picoquic"
    stamp = src / ".ngh_built"
    if stamp.exists() and stamp.read_text().strip() == PICOQUIC_COMMIT:
        print(f"  have picoquic {PICOQUIC_COMMIT[:12]} (built)")
        return 0

    if not (src / ".git").exists():
        src.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "init", "-q", str(src)])
        run(["git", "remote", "add", "origin", PICOQUIC_REPO], cwd=src)
    run(["git", "fetch", "-q", "--depth", "1", "origin", PICOQUIC_COMMIT],
        cwd=src)
    run(["git", "checkout", "-q", "-f", PICOQUIC_COMMIT], cwd=src)
    run(["git", "apply", str(root / "backend" / "picoquic_wt_compat.patch")],
        cwd=src)

    # e.g. a vcpkg toolchain on Windows:
    #   NGH_PICOQUIC_CMAKE_ARGS=-DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake
    extra = os.environ.get("NGH_PICOQUIC_CMAKE_ARGS", "").split()
    if os.name == "nt":
        # picotls includes wincompat.h, which lives in picoquic's tree; the
        # FetchContent build of picotls does not know that path on its own.
        # wincompat.h pulls Winsock2.h but not ws2tcpip.h, which picotls
        # needs for sockaddr_in6/inet_pton, hence the force-include.
        # Keep MSVC's default flags -- overriding CMAKE_C_FLAGS drops them.
        inc = (src / "picoquic").resolve()
        extra.append(
            f"-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /W3 /I{inc} /FIws2tcpip.h")
    run(["cmake", "-B", "build",
         "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
         "-DPICOQUIC_FETCH_PTLS=ON",
         "-Dpicoquic_BUILD_TESTS=OFF"] + extra, cwd=src)
    run(["cmake", "--build", "build", "--config", "Release",
         "--target", "picoquic-core", "picohttp-core"], cwd=src)

    stamp.write_text(PICOQUIC_COMMIT + "\n")
    print(f"  built picoquic {PICOQUIC_COMMIT[:12]} -> {src}/build")
    return 0


if __name__ == "__main__":
    sys.exit(main())
