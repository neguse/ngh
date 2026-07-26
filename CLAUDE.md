# ngh

Header-only C libraries. Currently one: `ngh_mediapipe.h` (MediaPipe face/pose
tracking, Windows/Linux/macOS/Web). CMake exists only for tests and examples;
the headers themselves need nothing.

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
- `vendor/` is fetched by `scripts/fetch_*.py`, never committed. Third-party
  notice obligations are in `THIRDPARTY.md` — the MediaPipe wheel's LICENSE
  does not carry the notices for its statically linked BSD/zlib dependencies.

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
- Verified in a real browser (Safari, 2026-07): full demo works and the GPU
  delegate is used, no CPU fallback needed. Demo deploy: scratchpad cfdemo/ ->
  `npx wrangler deploy` -> https://ngh-demo.negcee.workers.dev (temporary;
  remove with `npx wrangler delete ngh-demo`).
