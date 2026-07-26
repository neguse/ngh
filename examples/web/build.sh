#!/bin/sh
# Build the web example. Needs an activated emsdk (source emsdk_env.sh).
#
#   ./build.sh && python3 -m http.server 8000
#   open http://localhost:8000/examples/web/   (from the repository root)
#
# getUserMedia needs a secure context, so localhost or https only.
set -eu

cd "$(dirname "$0")"

emcc -std=c99 -O2 -Wall -Wextra \
    -I../.. \
    main.c -o ngh_web.js \
    -sEXPORTED_RUNTIME_METHODS=ccall \
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free \
    -sALLOW_MEMORY_GROWTH=1

echo "built examples/web/ngh_web.js"
