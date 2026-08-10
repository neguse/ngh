# Third-party components

ngh itself is MIT and has no dependencies beyond libc and the platform loader.
Nothing in this list is committed to the repository — `scripts/fetch_*.py`
downloads it. If you ship any of it, the notice obligations are yours.

## MediaPipe runtime — `libmediapipe.{dll,so,dylib}`

Apache License 2.0, © Google LLC.
<https://github.com/google-ai-edge/mediapipe>

Fetched from the official `mediapipe` wheels on PyPI (0.10.35). Google does not
publish this library separately; the wheels are the only official build.
`scripts/fetch_libmediapipe.py` also writes the wheel's `LICENSE` to
`vendor/LICENSE.mediapipe.txt`.

Redistribution is permitted under Apache-2.0 §4: include the licence text, keep
the copyright notices, and state any changes.

### Statically linked dependencies you must account for yourself

This is the part that is easy to get wrong. `libmediapipe` statically links a
number of third-party libraries, and **the wheel's `LICENSE` file does not carry
their notices** — it contains the Apache-2.0 text plus a single notice for a
Lucent UTF library. The BSD- and zlib-style licences among them require their
copyright notice and disclaimer to appear in the documentation of any binary
redistribution, so if you ship `libmediapipe` you have to assemble those notices
from upstream yourself. Google does not provide them.

Present in the shipped binary (identified from its embedded strings and build
configuration):

| component | licence |
|---|---|
| OpenCV | Apache-2.0 (4.5+) |
| libjpeg-turbo | BSD-3-Clause / IJG |
| libpng | PNG Reference Library License (zlib-like) |
| libtiff | libtiff (BSD-like) |
| zlib | zlib |
| Protocol Buffers | BSD-3-Clause |
| Abseil | Apache-2.0 |
| XNNPACK | BSD-3-Clause |
| TensorFlow Lite / LiteRT | Apache-2.0 |
| Eigen | **MPL-2.0** (file-level copyleft) |
| FlatBuffers | Apache-2.0 |

Eigen's MPL-2.0 is the one worth a second look: it is weak, file-level copyleft
rather than a permissive licence. Static linking is fine, but modifying Eigen's
own sources would carry obligations.

FFmpeg is *not* in the binary. The build banner reports `FFMPEG: YES`, which is
a detection result baked in during cross-compilation; there are no `libav*`
symbols in the shipped library.

## MediaPipe models — `*.task`

Apache License 2.0, © Google LLC.

Stated in each model card PDF (`LICENSED UNDER: Apache License, Version 2.0`),
not in the HTML documentation. A `.task` bundle is a ZIP holding the `.tflite`
models plus metadata.

| model | contents |
|---|---|
| `face_landmarker.task` | BlazeFace short-range detector, Face Mesh V2 (478 landmarks), blendshapes (52) |
| `pose_landmarker_{lite,full,heavy}.task` | BlazePose detector + landmarks (33) |

Served from `https://storage.googleapis.com/mediapipe-models/.../latest/...`.
That path is mutable, so `scripts/fetch_models.py` records the digests ngh was
tested against and reports a change rather than failing.

## WebTransport backend — `ngh_wt_backend.{so,dll}`

Built from source by `scripts/fetch_picoquic.py` + CMake (and prebuilt by
the `wt-backend` workflow for releases). The shared library statically
links:

| component | licence |
|---|---|
| picoquic (pinned, one-line patch in `backend/`) | MIT, © Private Octopus |
| picotls | MIT, © Kazuho Oku, DeNA Co., Ltd. and contributors |
| cifra (picotls minicrypto) | CC0 1.0 (public-domain dedication) |
| micro-ecc (picotls minicrypto) | BSD-2-Clause, © 2014 Kenneth MacKay |
| OpenSSL 3 | Apache-2.0 — **Windows build only** |

micro-ecc's BSD-2-Clause requires its copyright notice and disclaimer in
the documentation of any binary redistribution, and MIT requires the
licence text to accompany copies — if you ship the backend binary, carry
those notices yourself; no upstream file collects them for you.

The Linux build does not embed OpenSSL: it loads the system
`libssl`/`libcrypto` (and brotli, via picotls's certificate compression)
dynamically, which carries no notice obligation here.

Not part of ngh and never shipped with it.

- **stb_image** — public domain (or MIT at your option), Sean Barrett.
  <https://github.com/nothings/stb>. Used to decode JPEGs in the tests and the
  `still_image` example.
- **MediaPipe test data** — Apache-2.0, © Google LLC. The golden landmark files,
  `portrait.jpg`, `pose.jpg` and the test model bundles come from MediaPipe's
  own test suite so that `tests/test_golden.c` checks against the same reference
  the upstream C++ tests use.
