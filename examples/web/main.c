/* Face tracking from the webcam, in a browser, from C.
 *
 * Build (see build.sh):
 *   emcc -std=c99 -O2 -I../.. main.c -o ngh_web.js \
 *        -sEXPORTED_RUNTIME_METHODS=ccall,HEAPU8 -sALLOW_MEMORY_GROWTH=1
 *
 * HEAPU8 must be exported explicitly: index.html copies the fetched model into
 * wasm memory with Module.HEAPU8.set(), and current Emscripten no longer puts
 * the heap views on Module by default.
 *
 * The same ngh calls as the desktop examples. Two things differ, and both are
 * visible below:
 *
 *   1. Creation is asynchronous on the web, so ngh_face_state() is polled
 *      instead of being ready when create() returns. Desktop reports READY
 *      immediately, so this loop is correct on both.
 *   2. The frame is handed over as a <video> element via
 *      ngh_web_source_from_selector(). MediaPipe uploads whatever you give it
 *      with texImage2D, so passing the element directly avoids pulling pixels
 *      through wasm memory. ngh_image_rgba() works here too and keeps the code
 *      identical to the desktop examples, at the cost of one copy per frame.
 */

#define NGH_MEDIAPIPE_IMPLEMENTATION
#include "ngh_mediapipe.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>

static ngh_face *g_face;
static ngh_face_result g_result;
static int g_source = -1;
static uint64_t g_timestamp;
static int g_reported;

/* Hands landmark positions to JavaScript for drawing. Passing the pointer keeps
 * this to one call per frame instead of one per point. */
EM_JS(void, draw_landmarks, (const float *xyz, int count, int stride), {
    const canvas = document.getElementById('overlay');
    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    if (!count) return;
    ctx.fillStyle = '#39d353';
    const base = xyz >> 2;
    for (let i = 0; i < count; ++i) {
        const x = HEAPF32[base + i * stride] * canvas.width;
        const y = HEAPF32[base + i * stride + 1] * canvas.height;
        ctx.fillRect(x - 1, y - 1, 2, 2);
    }
});

EM_JS(void, set_status, (const char *text), {
    document.getElementById('status').textContent = UTF8ToString(text);
});

EM_JS(int, video_ready, (void), {
    const v = document.getElementById('camera');
    return v && v.readyState >= 2 ? 1 : 0;
});

static void tick(void) {
    ngh_state state = ngh_face_state(g_face);
    char message[256];
    int count;

    if (state == NGH_PENDING) {
        set_status("loading MediaPipe...");
        return;
    }
    if (state != NGH_READY) {
        snprintf(message, sizeof(message), "failed: %s",
                 ngh_face_error(g_face) ? ngh_face_error(g_face) : "unknown");
        set_status(message);
        emscripten_cancel_main_loop();
        return;
    }
    if (!g_reported) {
        g_reported = 1;
        printf("accel: %s\n",
               ngh_face_accel(g_face) == NGH_ACCEL_GPU ? "GPU" : "CPU");
    }
    if (!video_ready()) {
        set_status("waiting for the camera...");
        return;
    }

    {
        ngh_image image;
        memset(&image, 0, sizeof(image));
        image.kind = NGH_IMAGE_WEB;
        image.web_source = g_source;
        /* MediaPipe requires strictly increasing timestamps. */
        count = ngh_face_detect(g_face, &image, ++g_timestamp, &g_result);
    }

    if (count < 0) {
        snprintf(message, sizeof(message), "detect: %s", ngh_strerror(count));
        set_status(message);
        return;
    }

    draw_landmarks(g_result.landmarks, count * NGH_FACE_LANDMARKS, 3);
    if (count > 0) {
        /* Blendshape 25 is jawOpen: something visibly reactive to show. */
        snprintf(message, sizeof(message), "%d face  jawOpen %.2f  (%s)", count,
                 g_result.blendshapes[25],
                 ngh_face_accel(g_face) == NGH_ACCEL_GPU ? "GPU" : "CPU");
    } else {
        snprintf(message, sizeof(message), "no face");
    }
    set_status(message);
}

/* Called from index.html once the model has been fetched and the camera is up.
 * The model arrives as bytes, exactly like the desktop path. */
EMSCRIPTEN_KEEPALIVE
int ngh_web_start(const unsigned char *model, int model_size) {
    ngh_face_options opt = ngh_face_options_default();

    opt.blendshapes = 1;
    opt.max_faces = 1;
    opt.threaded = 0; /* ignored on the web; there is no worker to hand off to */

    g_face = ngh_face_create(model, (size_t)model_size, &opt);
    if (!g_face) {
        set_status("create failed");
        return 1;
    }
    if (ngh_face_result_alloc(&g_result, opt.max_faces, 1, 0) != NGH_OK) {
        set_status("out of memory");
        return 1;
    }
    g_source = ngh_web_source_from_selector("#camera");
    if (g_source < 0) {
        set_status("no #camera element");
        return 1;
    }
    emscripten_set_main_loop(tick, 0, 0);
    return 0;
}

int main(void) {
    set_status("waiting for start...");
    return 0;
}
