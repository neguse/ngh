# ngh

Header-only C libraries: `ngh_mediapipe.h` (MediaPipe face/pose tracking,
Windows/Linux/macOS/Web) and `ngh_webtransport.h` (WebTransport client,
Linux/Web now, Windows next). CMake exists only for tests, examples and the
webtransport backend library; the headers themselves need nothing.

## Decisions

Recorded so they don't get relitigated. Change only with a stated reason.

- Public API is C99. One header per library. No dependencies beyond libc and
  the platform loader; platform includes stay inside the implementation
  section.
- The MediaPipe ABI is pinned (`NGH_MEDIAPIPE_ABI` = 0.10.35) and enforced by
  static asserts plus `tests/test_abi.c`. Upstream makes no ABI promise and
  master has already diverged (+16 bytes in MpBaseOptions). Bumping the
  runtime version means re-deriving the layouts and re-running the golden
  tests — a mismatch fails as "Unsupported running mode", not loudly.
- Task creation is poll-based (`NGH_PENDING` → `NGH_READY`) because it is
  asynchronous on the web. Never require ASYNCIFY: it is a link flag a header
  cannot impose, and MediaPipe's detect calls are synchronous anyway.
- Desktop detect defaults to a worker thread with newest-result semantics.
  Inference is ~9ms of one core and MediaPipe will not parallelise it
  (single-threaded XNNPACK on desktop, measured 1.0 core over 18 threads).
- `accel` is a hint; report reality via `ngh_*_accel()`. Windows libmediapipe
  has no GPU path at all (upstream disables GPU for os:windows
  unconditionally). Measured: GPU loses to CPU on face landmarks even where
  available; only pose full/heavy benefit. AUTO = CPU on desktop.
- Web: never expose MediaPipe's `canvas` option (its auto-resize destroys the
  host app's canvas). Shared JS state lives in `globalThis.__NGH`; handles are
  plain ints because EM_JS bodies cannot share scope.
- **Cameras are not an ngh library.** SDL3 already is the unified camera API
  (enumeration, permission, frames on every platform ngh targets); a wrapper
  would only rename it, and the real consumer (../lub) uses SDL3 directly.
  The wiring pattern lives in `examples/desktop/camera_face.c`. Revisit only
  if a non-SDL consumer actually appears.
- On the web, never route camera frames through SDL's camera backend (per-frame
  `getImageData` readback); pass the `<video>` element via
  `ngh_web_source_from_selector()`.
- 3D decode belongs to ngh, scene mapping to the consumer. ngh's documented
  target is the "ngh camera space" (right-handed, y-up, -z forward, metres);
  helpers convert each MediaPipe output into it. Verified conventions
  (tests/test_space.c): face metric space is y-up/-z-forward in CENTIMETRES
  with scale absorbed upstream (always ~1.0); pose world is y-DOWN/+z-away
  metres at the hip origin; normalized z is width-scaled, negative = closer.
  ngh does not do mirroring, smoothing or retargeting -- consumer's call.
- `ngh_webtransport.h`: WebTransport client. Backend is picoquic
  (tag-pinned), compiled by GitHub Actions into a shim shared library that
  exports only an ngh-designed ABI — picoquic's own API never crosses it.
  Binaries attach to ngh Releases; a fetch script downloads them with SHA256
  pinning. v1 is client-only, but the shim ABI must not preclude adding a
  server role later. Web + Linux + Windows ship as one set; macOS follows.
  Server side: wtransport (Rust/quinn) is the first-candidate production
  server (chosen for performance). ctest uses picoquic's own demo server as
  the reference, plus one cross-stack interop test against wtransport —
  same-stack-only testing would let draft-interpretation bugs cancel out.
- `vendor/` is fetched by `scripts/fetch_*.py`, never committed. Third-party
  notice obligations are in `THIRDPARTY.md` — the MediaPipe wheel's LICENSE
  does not carry the notices for its statically linked BSD/zlib dependencies.

- Documentation lives in the header, stb-style: the top comment of
  ngh_mediapipe.h is the user manual and travels with the copied file.
  README.md is repository-level only (setup, scripts, licence pointers).
  Never document the same fact in both places.

## Working notes

- Full check: `scripts/fetch_libmediapipe.py && scripts/fetch_testdata.py`,
  then `cmake -B build && cmake --build build && ctest --test-dir build`.
- Golden tolerances are upstream's: landmarks 0.03, blendshapes 0.12. Compare
  in IMAGE-equivalent conditions (threaded=0); VIDEO tracking output is
  nondeterministic upstream.
- TSan reports hundreds of races — all inside the uninstrumented libmediapipe,
  none from ngh accesses. ASan/UBSan are clean; leak reports trace to
  MediaPipe's global registries under dlopen.
- Intended consumer: `../lub` (SDL3 submodule, release-3.2.30). CI's
  FetchContent pin for the camera example must track lub's SDL tag.
- Linux GPU delegate needs `EGL_PLATFORM=surfaceless` when headless.
- WebTransport backend (2026-08): backend/wt_backend.{h,c} is the shim ABI
  and its picoquic implementation. scripts/fetch_picoquic.py stages the
  pinned picoquic (467cb81) with backend/picoquic_wt_compat.patch, which
  drops picoquic's hard requirement on the peer's RESET_STREAM_AT transport
  parameter (WT draft-13) -- quinn/quic-go era servers do not implement it.
  Verified against wtransport 0.7.1 on localhost (build/ngh_wt_backend_check
  <url> <cert_sha256_hex>): session accept, datagram echo, bidi stream echo,
  capsule close, and rejection of a wrong cert pin. Pinning is
  serverCertificateHashes-style SHA-256 plus a real CertificateVerify
  signature check (OpenSSL EVP); CA-store verification is unimplemented, so
  connect() requires cert_hashes. h3zero quirk: joint data+FIN arrives as
  one post_fin callback with length > 0 -- consume bytes there too.
  Cross-stack interop is a ctest: `cmake -B build -DNGH_WT_INTEROP=ON`
  runs tests/wt_interop.py (wtransport echo server in tests/wt-echo,
  needs cargo). The wt-backend workflow runs the full round-trip on both
  Linux and Windows CI (vcpkg static OpenSSL on Windows): pinned-cert
  session, datagram echo, bidi stream echo, capsule close. The
  picotls/picoquic CMake path is unmaintained upstream on Windows (they
  ship VS projects): the gaps are patched in scripts/fetch_picoquic.py
  (pkg-config, wincompat.h include path, /FIws2tcpip.h) and CMakeLists
  (compile picotlsvs wintimeofday.c into the backend, link bcrypt).
  Releases: push a `wt-backend-v*` tag and the workflow attaches the
  per-platform binaries.
- Verified in a real browser (Safari, 2026-07): full demo works and the GPU
  delegate is used, no CPU fallback needed. Demo deploy: scratchpad cfdemo/ ->
  `npx wrangler deploy` -> https://ngh-demo.negcee.workers.dev (temporary;
  remove with `npx wrangler delete ngh-demo`).
