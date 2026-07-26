# ngh

Header-only C libraries for neguse.

## ngh_mediapipe.h

MediaPipe face and body tracking from C, with one API across Windows and the
web. Copy `ngh_mediapipe.h` into your project and define the implementation
macro in exactly one translation unit:

```c
#define NGH_MEDIAPIPE_IMPLEMENTATION
#include "ngh_mediapipe.h"
```

There is nothing to link. On desktop the header resolves MediaPipe's official
Tasks C API out of `libmediapipe.{dll,so,dylib}` at run time; on the web it
bridges to `@mediapipe/tasks-vision` through `EM_JS`, with no extra `emcc`
flags.

### Why this exists

MediaPipe is two unrelated things depending on where you build it. Desktop has
an official C API shipped only inside the Python wheels; the web has a
JavaScript package and no C API at all. Neither is pleasant to reach from a C
codebase, and the official C headers are not even valid C — they include
`<cstdint>`, use C++ default member initialisers and declare a typedef inside a
struct. This header is the seam.

### Getting the pieces

```sh
python3 scripts/fetch_libmediapipe.py   # libmediapipe.{dll,so,dylib} -> vendor/
python3 scripts/fetch_models.py         # face + pose .task bundles  -> vendor/
```

`ngh_runtime_load(NULL)` looks for the library in `$NGH_MEDIAPIPE_PATH`, then
next to your executable, then on the loader's default search path. Calling it is
optional — the first `create()` loads lazily — but doing it up front lets you
report a missing runtime before anything else.

### Using it

```c
ngh_face_options options = ngh_face_options_default();
ngh_face *face = ngh_face_create(model_bytes, model_size, &options);

ngh_face_result result;
ngh_face_result_alloc(&result, options.max_faces, /*blendshapes=*/1, 0);

/* Per frame. Timestamps must strictly increase. */
if (ngh_face_state(face) == NGH_READY) {
    ngh_image image = ngh_image_rgba(pixels, width, height, /*stride=*/0);
    int faces = ngh_face_detect(face, &image, timestamp_ms, &result);
    for (int i = 0; i < faces; ++i) {
        const float *lm = result.landmarks + i * NGH_FACE_LANDMARKS * 3;
        float jaw_open = result.blendshapes[i * NGH_FACE_BLENDSHAPES + 25];
        /* ... */
    }
}
```

`ngh_pose_*` mirrors this, with 33 landmarks of `(x, y, z, visibility)` and
optional metric world coordinates.

Two API decisions are worth knowing about:

**Creation is polled, not awaited.** `ngh_face_create()` returns immediately and
`ngh_face_state()` reports `NGH_PENDING` until the task is up. On desktop it is
`NGH_READY` before `create()` returns, so the same loop is correct on both. The
alternative on the web would be ASYNCIFY, a link flag a header cannot impose
that costs roughly 50% in size and speed across the whole program.

**Detection is non-blocking by default.** With `options.threaded` (the default
on desktop), `ngh_face_detect()` hands the frame to a worker and returns the
newest finished result — so `result.timestamp_ms` may lag the frame you just
submitted, and frames submitted while the worker is busy are dropped. That is
deliberate: inference costs ~9ms of a single core against a 16.6ms budget at
60fps. Set `threaded = 0` when you want the result for the frame you passed in,
as `examples/desktop/still_image.c` does.

### Platform support

| | backend | acceleration | status |
|---|---|---|---|
| Windows 10+ x64 / arm64 | `libmediapipe.dll` | **CPU only** | supported |
| Linux x64 | `libmediapipe.so` | CPU, GPU opt-in | supported |
| macOS arm64 | `libmediapipe.dylib` | CPU, GPU opt-in | supported |
| Web (Emscripten) | `@mediapipe/tasks-vision` | GPU (WebGL), CPU fallback | supported |
| Android / iOS | — | — | planned |

**Windows cannot use the GPU, and this is not a packaging problem.** MediaPipe's
only desktop GPU inference path is an OpenGL ES compute shader, there is no WGL
context implementation in the tree, and `mediapipe/gpu/BUILD` puts
`@platforms//os:windows` unconditionally into its `disable_gpu` group. So
`NGH_ACCEL_GPU` is a hint: ask `ngh_face_accel()` what you actually got.

It matters less than it sounds. Measured on a Ryzen 7 PRO 5750GE with a Vega
iGPU, in VIDEO mode, p50 over 270 frames:

| | CPU | GPU | |
|---|---|---|---|
| face landmarks | **9.2 ms** | 12.0 ms | GPU is *slower* |
| pose lite | 14.4 ms | **10.4 ms** | |
| pose full | 19.7 ms | **11.7 ms** | |
| pose heavy | 57.3 ms | **24.5 ms** | |

The face models are small enough (192×192 detector, 256×256 landmarks) that the
per-frame upload and sync cost more than the GPU saves. Only the heavier pose
models benefit, and `pose_lite` on CPU already fits a frame budget.

What the GPU does buy is CPU headroom: it drops the process from 1.0 core to
0.2–0.5. On CPU, MediaPipe runs inference on a single thread no matter how many
cores you have — measured as exactly 1.0 core across 18 threads — so the
threaded mode above is what keeps a game loop alive, not the GPU.

On Linux the GPU delegate additionally needs `EGL_PLATFORM=surfaceless` in a
headless process, or task creation fails with `Unable to initialize EGL`.

### Cameras

ngh does not capture video, deliberately. `ngh_image` is the boundary — hand
ngh whatever frames you already have.

On desktop, SDL3's camera API already is the unified answer: enumeration,
permission prompts and frame delivery across Windows (Media Foundation), Linux
(V4L2 / PipeWire), macOS (CoreMedia), Android and iOS. Wrapping that in another
API would just rename it, so ngh doesn't. The entire bridge is one call:

```c
SDL_Surface *frame = SDL_AcquireCameraFrame(cam, &ts_ns);
if (frame) {
    ngh_image img = ngh_image_rgba(frame->pixels, frame->w, frame->h, frame->pitch);
    ngh_face_detect(face, &img, ts_ns / 1000000, &result);
    SDL_ReleaseCameraFrame(cam, frame);
}
```

`examples/desktop/camera_face.c` is the full loop, permission polling included
— SDL's permission state and ngh's creation state are both polls, so they share
one loop. Build it with `-DNGH_EXAMPLES_SDL3=ON`.

On the web, do **not** route frames through SDL's camera backend: it reads
every frame back from a canvas with `getImageData()` (GPU→CPU), and MediaPipe
then uploads it again. Call `getUserMedia` yourself, attach the stream to a
`<video>` element, and pass it with `ngh_web_source_from_selector()` — which is
exactly what `examples/web/` does.

### Web notes

The defaults point at a pinned CDN for both the JS bundle and the ~11.5 MB
MediaPipe wasm. That is fine for prototyping; for production, copy
`node_modules/@mediapipe/tasks-vision/{vision_bundle.mjs,wasm}` into your own
assets and point `ngh_web_set_bundle_url()` / `ngh_web_set_wasm_base()` at them.

`ngh_image_rgba()` works on the web and keeps your code identical across
platforms, at the cost of one copy per frame. For live video, prefer
`ngh_web_source_from_selector("#camera")` and pass the handle: MediaPipe uploads
every input through `texImage2D` regardless, so handing it the `<video>` element
skips a round trip through wasm memory. If you pass a WebGL canvas created with
`preserveDrawingBuffer: false`, do it inside the same `requestAnimationFrame`
callback that drew it — the browser clears it after compositing.

ngh does not touch your canvas or WebGL context. MediaPipe creates its own
`OffscreenCanvas`, and the option that would let it resize yours is deliberately
not exposed.

### The ABI is pinned to one MediaPipe release

`NGH_MEDIAPIPE_ABI` is `0.10.35`. The Tasks C API makes no ABI promise and has
already moved: upstream `master` added three `MpBaseOptions` fields after
0.10.35, which shifts every option struct by 16 bytes. A mismatched library does
not fail loudly — it misreads `running_mode` and reports
`Unsupported running mode: unknown mode`. `tests/test_abi.c` and the
`NGH__ASSERT_LAYOUT` checks in the header pin all of it, so a wrong layout
breaks the build rather than the output.

### Building the tests and examples

```sh
python3 scripts/fetch_libmediapipe.py
python3 scripts/fetch_models.py
python3 scripts/fetch_testdata.py

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

./build/ngh_still_image vendor/face_landmarker.task vendor/testdata/portrait.jpg
./build/ngh_game_loop   vendor/face_landmarker.task
```

`tests/test_golden.c` compares against MediaPipe's own golden files using its
own test bundles and its own tolerances (0.03 for landmarks, 0.12 for
blendshapes). Current deviations: face landmarks 0.0045, blendshapes 0.083, pose
landmarks 0.024.

The suite is clean under ASan and UBSan. Under TSan expect several hundred
reports — every one of them is between MediaPipe's own threads, none from ngh.
`libmediapipe` is not instrumented, so TSan cannot see the locking inside it.

For the web:

```sh
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -B build-web && cmake --build build-web
ctest --test-dir build-web            # node-hosted wiring tests

./examples/web/build.sh
python3 -m http.server 8000           # then open /examples/web/
```

### Configuration macros

| macro | effect |
|---|---|
| `NGH_MEDIAPIPE_IMPLEMENTATION` | emit the implementation (exactly one TU) |
| `NGH_NO_THREADS` | drop the worker-thread path; `threaded` becomes a no-op |
| `NGH_WEB_BUNDLE_URL` | default `@mediapipe/tasks-vision` ESM bundle URL |
| `NGH_WEB_WASM_BASE` | default directory holding `vision_wasm_internal.*` |

## License

MIT, see `LICENSE.txt`. MediaPipe and its models are Apache-2.0 and are fetched
rather than vendored; read `THIRDPARTY.md` before redistributing them.
