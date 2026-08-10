# ngh

Header-only C libraries for neguse.

Documentation lives in each header, stb-style: copy the file into your
project, read the comment at the top. This README only covers the repository
itself.

| library | what |
|---|---|
| [`ngh_mediapipe.h`](ngh_mediapipe.h) | MediaPipe face / pose tracking. Windows 10+, Linux, macOS, Web (Emscripten); Android/iOS planned. 478 face landmarks + 52 blendshapes + head transform, 33 pose landmarks + metric world coordinates. |
| [`ngh_webtransport.h`](ngh_webtransport.h) | WebTransport client. Linux, Windows, Web (Emscripten); macOS planned. Sessions, datagrams, bidi/uni streams, certificate pinning; poll-based, no callbacks. Desktop needs the `ngh_wt_backend` library (picoquic, built by `scripts/fetch_picoquic.py` + CMake). |

## Repository setup

The headers need nothing, but the tests and examples need the MediaPipe
runtime, models and golden data — none of which is committed here:

```sh
python3 scripts/fetch_libmediapipe.py   # runtime -> vendor/  (from the PyPI wheel)
python3 scripts/fetch_models.py         # .task models -> vendor/
python3 scripts/fetch_testdata.py       # golden data + stb_image -> vendor/testdata/
python3 scripts/fetch_picoquic.py       # webtransport backend deps -> vendor/picoquic/

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Examples (`build/ngh_still_image`, `build/ngh_game_loop`, the WebTransport
echo client `build/ngh_wt_echo`; add `-DNGH_EXAMPLES_SDL3=ON` for the
webcam example `ngh_camera_face`). Add `-DNGH_WT_INTEROP=ON` (needs cargo)
to also run the WebTransport interop test against a Rust echo server.

For the web:

```sh
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -B build-web && cmake --build build-web
ctest --test-dir build-web              # node-hosted tests

./examples/web/build.sh                 # webcam demo
python3 -m http.server 8000             # then open /examples/web/
```

## License

MIT, see `LICENSE.txt`. MediaPipe and its models are Apache-2.0 and are
fetched rather than vendored; the WebTransport backend statically links
picoquic, picotls and (on Windows) OpenSSL. Read `THIRDPARTY.md` before
redistributing either binary — the notice obligations are not covered by
the upstream LICENSE files alone.
