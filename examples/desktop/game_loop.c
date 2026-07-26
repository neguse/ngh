/* The pattern a real-time application should use.
 *
 *   NGH_MEDIAPIPE_PATH=vendor/libmediapipe.so ./build/ngh_game_loop \
 *       vendor/face_landmarker.task
 *
 * Inference costs about 9ms of one core for face landmarks and 14ms for pose,
 * against a 16.6ms budget at 60fps, and MediaPipe runs it on a single thread no
 * matter how many cores the machine has. Blocking on it would eat most of the
 * frame. With threaded mode, ngh_face_detect() hands the frame to a worker and
 * returns the newest finished result, so the loop never waits: frames submitted
 * while the worker is busy are dropped, which is what you want -- the newest
 * frame is the only one worth tracking.
 *
 * This prints how long detect() actually blocks the loop and how far behind the
 * results are, on synthetic frames so it needs no image decoder.
 *
 * Those frames contain no face, so only the ~1ms detector runs and the 9ms
 * landmark model never does. Read the blocking time, not the result rate: with
 * a real face the worker takes about 9ms per frame and delivers roughly every
 * other frame at 60fps, while the number that matters here -- how long the loop
 * waits -- stays the same.
 */

#define NGH_MEDIAPIPE_IMPLEMENTATION
#include "ngh_mediapipe.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static void sleep_ms(double ms) { Sleep((DWORD)(ms > 0 ? ms : 0)); }
#else
#include <time.h>
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static void sleep_ms(double ms) {
    struct timespec ts;
    if (ms <= 0) return;
    ts.tv_sec = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
}
#endif

#define WIDTH 640
#define HEIGHT 480
#define FRAMES 180
#define FRAME_MS 16.666

static void *read_file(const char *path, size_t *size) {
    void *buf;
    long n;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(fp);
    if (buf) *size = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "vendor/face_landmarker.task";
    ngh_face_options opt = ngh_face_options_default();
    ngh_face_result result;
    ngh_face *face;
    unsigned char *frame;
    void *model;
    size_t model_size = 0;
    double worst = 0.0, total = 0.0, start;
    uint64_t last_result_ts = 0;
    int fresh = 0, i;

    model = read_file(model_path, &model_size);
    if (!model) {
        fprintf(stderr, "cannot read model: %s\n", model_path);
        return 1;
    }

    opt.threaded = 1; /* the whole point of this example */
    opt.blendshapes = 1;
    face = ngh_face_create(model, model_size, &opt);
    if (!face || ngh_face_state(face) != NGH_READY) {
        fprintf(stderr, "create failed: %s\n",
                face ? ngh_face_error(face) : ngh_runtime_error());
        return 1;
    }
    if (ngh_face_result_alloc(&result, opt.max_faces, 1, 0) != NGH_OK) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    frame = (unsigned char *)malloc(WIDTH * HEIGHT * 4);
    if (!frame) return 1;

    printf("runtime  : %s\n", ngh_runtime_path());
    printf("accel    : %s\n",
           ngh_face_accel(face) == NGH_ACCEL_GPU ? "GPU" : "CPU");
    printf("loop     : %d frames at %.1f ms (%dx%d RGBA)\n\n", FRAMES, FRAME_MS,
           WIDTH, HEIGHT);

    for (i = 0; i < FRAMES; ++i) {
        uint64_t timestamp = (uint64_t)(i + 1) * 16;
        double frame_start = now_ms();
        double spent;
        int count;
        int p;

        /* Stand-in for a camera frame. */
        for (p = 0; p < WIDTH * HEIGHT * 4; p += 4) {
            frame[p + 0] = (unsigned char)(p + i);
            frame[p + 1] = (unsigned char)(p >> 8);
            frame[p + 2] = (unsigned char)i;
            frame[p + 3] = 255;
        }

        start = now_ms();
        {
            ngh_image image = ngh_image_rgba(frame, WIDTH, HEIGHT, 0);
            count = ngh_face_detect(face, &image, timestamp, &result);
        }
        spent = now_ms() - start;
        if (count < 0) {
            fprintf(stderr, "detect failed: %s\n", ngh_strerror(count));
            return 1;
        }
        if (spent > worst) worst = spent;
        total += spent;

        if (result.timestamp_ms != last_result_ts) {
            last_result_ts = result.timestamp_ms;
            ++fresh;
        }

        /* Whatever else a frame does goes here; results are already in hand. */
        sleep_ms(FRAME_MS - (now_ms() - frame_start));
    }

    printf("detect() blocked the loop for %.3f ms on average, %.3f ms worst\n",
           total / FRAMES, worst);
    printf("fresh results: %d of %d frames (%.0f Hz effective)\n", fresh, FRAMES,
           (double)fresh / (FRAMES * FRAME_MS / 1000.0));
    printf("last result lagged the submitted frame by %d ms\n",
           (int)((uint64_t)FRAMES * 16 - last_result_ts));

    free(frame);
    ngh_face_result_free(&result);
    ngh_face_destroy(face);
    free(model);
    return 0;
}
