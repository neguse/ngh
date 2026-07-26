#!/usr/bin/env python3
"""Fetch MediaPipe .task model bundles.

    python3 scripts/fetch_models.py                    # face + pose_lite
    python3 scripts/fetch_models.py --all
    python3 scripts/fetch_models.py face_landmarker --out assets/

Google serves these from a mutable `.../latest/...` path, so the digests below
are what ngh has been tested against rather than a guarantee. A mismatch is
reported but not fatal: pass --strict in CI if you want it to fail.

The models are Apache-2.0 (stated in each model card PDF). They are deliberately
not committed to this repository -- pose_landmarker_heavy alone is 29 MB.
"""

import argparse
import hashlib
import sys
import urllib.request
from pathlib import Path

BASE = "https://storage.googleapis.com/mediapipe-models"

# name -> (url, sha256, bytes, note)
MODELS = {
    "face_landmarker": (
        f"{BASE}/face_landmarker/face_landmarker/float16/latest/face_landmarker.task",
        "64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff",
        3758596,
        "478 landmarks + 52 blendshapes + transformation matrix",
    ),
    "pose_landmarker_lite": (
        f"{BASE}/pose_landmarker/pose_landmarker_lite/float16/latest/pose_landmarker_lite.task",
        "59929e1d1ee95287735ddd833b19cf4ac46d29bc7afddbbf6753c459690d574a",
        5777746,
        "33 landmarks, ~14ms CPU",
    ),
    "pose_landmarker_full": (
        f"{BASE}/pose_landmarker/pose_landmarker_full/float16/latest/pose_landmarker_full.task",
        "4eaa5eb7a98365221087693fcc286334cf0858e2eb6e15b506aa4a7ecdcec4ad",
        9398198,
        "33 landmarks, ~18ms CPU",
    ),
    "pose_landmarker_heavy": (
        f"{BASE}/pose_landmarker/pose_landmarker_heavy/float16/latest/pose_landmarker_heavy.task",
        "64437af838a65d18e5ba7a0d39b465540069bc8aae8308de3e318aad31fcbc7b",
        30664242,
        "33 landmarks, ~56ms CPU -- too slow for a frame budget",
    ),
}

DEFAULT = ["face_landmarker", "pose_landmarker_lite"]


def fetch(name: str, out_dir: Path, strict: bool) -> bool:
    url, sha256, size, note = MODELS[name]
    path = out_dir / f"{name}.task"
    if path.exists():
        print(f"{path} already exists; delete it to re-download")
        return True

    print(f"{name}  ({note})")
    print(f"  fetching {url}")
    blob = urllib.request.urlopen(url).read()
    got = hashlib.sha256(blob).hexdigest()

    if got != sha256:
        msg = (f"  digest changed\n    tested against {sha256}\n"
               f"    served now     {got}\n"
               f"    ({size} -> {len(blob)} bytes)")
        if strict:
            print(msg, file=sys.stderr)
            return False
        print(msg + "\n  continuing -- Google republishes under 'latest'")

    out_dir.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)
    print(f"  wrote {path} ({len(blob) / 1e6:.1f} MB)")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("models", nargs="*", choices=sorted(MODELS) + [[]],
                    default=[])
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--strict", action="store_true",
                    help="fail when a digest does not match")
    ap.add_argument("--out", default="vendor", type=Path)
    args = ap.parse_args()

    names = sorted(MODELS) if args.all else (args.models or DEFAULT)
    return 0 if all(fetch(n, args.out, args.strict) for n in names) else 1


if __name__ == "__main__":
    raise SystemExit(main())
