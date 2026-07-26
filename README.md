# ngh

Header-only C libraries for neguse.

Documentation lives in each header, stb-style: copy the file into your
project, read the comment at the top. This README only covers the repository
itself.

| library | what |
|---|---|
| [`ngh_mediapipe.h`](ngh_mediapipe.h) | MediaPipe face / pose tracking. Windows 10+, Linux, macOS, Web (Emscripten); Android/iOS planned. 478 face landmarks + 52 blendshapes + head transform, 33 pose landmarks + metric world coordinates. |

## Repository setup

The headers need nothing, but the tests and examples need the MediaPipe
runtime, models and golden data — none of which is committed here:

```sh
python3 scripts/fetch_libmediapipe.py   # runtime -> vendor/  (from the PyPI wheel)
python3 scripts/fetch_models.py         # .task models -> vendor/
python3 scripts/fetch_testdata.py       # golden data + stb_image -> vendor/testdata/

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Examples (`build/ngh_still_image`, `build/ngh_game_loop`; add
`-DNGH_EXAMPLES_SDL3=ON` for the webcam example `ngh_camera_face`).

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
fetched rather than vendored; read `THIRDPARTY.md` before redistributing
them — the notice obligations are not covered by MediaPipe's own LICENSE
file.
