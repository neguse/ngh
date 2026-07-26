#!/usr/bin/env python3
"""Fetch everything tests/ needs that is not source: images, golden landmarks,
the model bundles upstream tests use, and a JPEG decoder.

    python3 scripts/fetch_testdata.py

tests/test_golden.c deliberately uses MediaPipe's *own* test bundles and golden
files rather than the public models, so it compares ngh against the same
reference the upstream C++ tests use, at the same tolerances (0.03 for
landmarks, 0.12 for blendshapes).

stb_image.h is public domain and only used by the tests -- ngh itself needs
nothing beyond libc and the platform loader.
"""

import hashlib
import sys
import urllib.request
from pathlib import Path

MP_RAW = ("https://raw.githubusercontent.com/google-ai-edge/mediapipe/"
          "v0.10.35/mediapipe/tasks/testdata/vision")
MP_ASSETS = "https://storage.googleapis.com/mediapipe-assets"
MP_TESTDATA = f"{MP_ASSETS}/tasks/testdata/vision"

# url, output name, sha256 (None = not pinned)
FILES = [
    (f"{MP_ASSETS}/portrait.jpg", "portrait.jpg", None),
    (f"{MP_ASSETS}/pose.jpg", "pose.jpg", None),
    (f"{MP_RAW}/portrait_expected_face_landmarks.pbtxt",
     "portrait_expected_face_landmarks.pbtxt", None),
    (f"{MP_RAW}/portrait_expected_blendshapes.pbtxt",
     "portrait_expected_blendshapes.pbtxt", None),
    (f"{MP_RAW}/pose_landmarks.pbtxt", "pose_landmarks.pbtxt", None),
    (f"{MP_TESTDATA}/face_landmarker_v2_with_blendshapes.task"
     "?generation=1782184688504388",
     "face_landmarker_v2_with_blendshapes.task",
     "b261925d4aad812b47a0e8d58c1baa1223270a5d1f663d78338bc881c003879d"),
    (f"{MP_TESTDATA}/pose_landmarker.task?generation=1782185245556889",
     "pose_landmarker.task",
     "fb9cc326c88fc2a4d9a6d355c28520d5deacfbaa375b56243b0141b546080596"),
    ("https://raw.githubusercontent.com/nothings/stb/master/stb_image.h",
     "stb_image.h", None),
]


def main() -> int:
    out = Path("vendor/testdata")
    out.mkdir(parents=True, exist_ok=True)
    ok = True
    for url, name, sha256 in FILES:
        path = out / name
        if path.exists():
            print(f"  have {path}")
            continue
        print(f"  fetching {name}")
        blob = urllib.request.urlopen(url).read()
        got = hashlib.sha256(blob).hexdigest()
        if sha256 and got != sha256:
            print(f"    digest mismatch: expected {sha256}, got {got}",
                  file=sys.stderr)
            ok = False
            continue
        path.write_bytes(blob)
        print(f"       {len(blob):>9} bytes  sha256={got[:16]}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
