#!/usr/bin/env python3
"""Fetch libmediapipe.{dll,so,dylib} -- the MediaPipe Tasks C API runtime.

Google does not publish this library on its own: the only official builds live
inside the `mediapipe` wheels on PyPI, where the Python API loads them through
ctypes. This script downloads the right wheel, verifies its SHA256 and extracts
the one file ngh needs.

    python3 scripts/fetch_libmediapipe.py                 # host platform
    python3 scripts/fetch_libmediapipe.py --platform windows-x86_64
    python3 scripts/fetch_libmediapipe.py --out build/

The result is Apache-2.0 licensed. It statically links OpenCV, libjpeg-turbo,
libpng, libtiff, protobuf, XNNPACK, Eigen and zlib, whose notices the wheel does
not carry -- see THIRDPARTY.md before redistributing it.
"""

import argparse
import hashlib
import io
import platform
import sys
import urllib.request
import zipfile
from pathlib import Path

MEDIAPIPE_VERSION = "0.10.35"

# filename -> (url, sha256, member inside the wheel, output name)
WHEELS = {
    "linux-x86_64": (
        "https://files.pythonhosted.org/packages/32/8f/1bc57dbc9b7b03c8f875aac23380ec57e9002cc02fe6720045fb263f3966/mediapipe-0.10.35-py3-none-manylinux_2_28_x86_64.whl",
        "db9a579df48cffe9570cd3e93f6a5d2dd089a1103b846c60c5b5de8a21c38db0",
        "mediapipe/tasks/c/libmediapipe.so",
        "libmediapipe.so",
    ),
    "windows-x86_64": (
        "https://files.pythonhosted.org/packages/5b/f6/763477e9aeed98accc984ed6ee3f11a21a0c5fd1d1c6586b8d07067748ff/mediapipe-0.10.35-py3-none-win_amd64.whl",
        "b08f001cf3c3cd0d88d9ed68f3368dc8a4913f568281a93117f083115aa672ba",
        "mediapipe/tasks/c/libmediapipe.dll",
        "libmediapipe.dll",
    ),
    "windows-arm64": (
        "https://files.pythonhosted.org/packages/07/b3/5c7fa594c731e8dafab9f1a46ab6cef670fa62dbbfb6248cc70e42ec6fc5/mediapipe-0.10.35-py3-none-win_arm64.whl",
        "46255326a6213118aaa518a7aa25e35f93337e82677960cc2a945f117bff8444",
        "mediapipe/tasks/c/libmediapipe.dll",
        "libmediapipe.dll",
    ),
    "macos-arm64": (
        "https://files.pythonhosted.org/packages/aa/c2/439e948d2a9a542498aee5d8fa3fb91ac6f4478be5d502f0c78ca3fb2333/mediapipe-0.10.35-py3-none-macosx_11_0_arm64.whl",
        "3b31376f34ca3665e34b834565996464cd66c9c91316e914fa7f149c891ce7ac",
        "mediapipe/tasks/c/libmediapipe.dylib",
        "libmediapipe.dylib",
    ),
}


def host_platform() -> str:
    system = platform.system()
    machine = platform.machine().lower()
    arm = machine in ("arm64", "aarch64")
    if system == "Linux":
        if arm:
            raise SystemExit(
                "no official MediaPipe wheel for linux-arm64; build "
                "//mediapipe/tasks/c:mediapipe_source from source"
            )
        return "linux-x86_64"
    if system == "Windows":
        return "windows-arm64" if arm else "windows-x86_64"
    if system == "Darwin":
        if not arm:
            raise SystemExit(
                "no official MediaPipe wheel for macos-x86_64 at "
                f"{MEDIAPIPE_VERSION}"
            )
        return "macos-arm64"
    raise SystemExit(f"unsupported platform: {system} {machine}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--platform", choices=sorted(WHEELS), default=None)
    ap.add_argument("--out", default="vendor", type=Path)
    args = ap.parse_args()

    target = args.platform or host_platform()
    url, sha256, member, out_name = WHEELS[target]
    out_path = args.out / out_name

    if out_path.exists():
        print(f"{out_path} already exists; delete it to re-download")
        return 0

    print(f"mediapipe {MEDIAPIPE_VERSION} ({target})")
    print(f"  fetching {url}")
    blob = urllib.request.urlopen(url).read()

    got = hashlib.sha256(blob).hexdigest()
    if got != sha256:
        print(f"  SHA256 mismatch\n    expected {sha256}\n    got      {got}",
              file=sys.stderr)
        return 1
    print(f"  sha256 ok ({len(blob) / 1e6:.1f} MB)")

    with zipfile.ZipFile(io.BytesIO(blob)) as zf:
        data = zf.read(member)
        license_text = None
        for name in zf.namelist():
            if name.endswith("dist-info/licenses/LICENSE") or name.endswith(
                    "dist-info/LICENSE"):
                license_text = zf.read(name)
                break

    args.out.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(data)
    if sys.platform != "win32":
        out_path.chmod(0o755)
    print(f"  wrote {out_path} ({len(data) / 1e6:.1f} MB)")

    if license_text:
        lic = args.out / "LICENSE.mediapipe.txt"
        lic.write_bytes(license_text)
        print(f"  wrote {lic}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
