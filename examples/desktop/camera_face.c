/* Webcam face tracking through SDL3's camera API.
 *
 *   cmake -B build -DNGH_EXAMPLES_SDL3=ON && cmake --build build
 *   ./build/ngh_camera_face vendor/face_landmarker.task
 *
 * This stays a sample on purpose. SDL3 already is the cross-platform camera
 * API -- device enumeration, the permission flow and frame delivery over
 * Media Foundation / V4L2 / PipeWire / CoreMedia / Android -- so ngh does not
 * wrap it in another one. The entire bridge to ngh is the ngh_image_rgba()
 * call in the loop below; SDL_Surface's pitch goes straight into the stride
 * argument, and SDL's frame timestamp, taken in milliseconds, satisfies
 * detect()'s strictly-increasing rule because SDL only hands out new frames.
 *
 * Do not copy this pattern to the web. SDL's Emscripten camera backend reads
 * every frame back from a canvas with getImageData() (GPU->CPU) and MediaPipe
 * would upload it again (CPU->GPU). Call getUserMedia yourself and hand the
 * <video> element to ngh_web_source_from_selector() instead, the way
 * examples/web/ does.
 */

#define NGH_MEDIAPIPE_IMPLEMENTATION
#include "ngh_mediapipe.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_FRAMES 300 /* ~10 seconds at 30fps, then exit */

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
    SDL_Camera *cam;
    SDL_CameraID *cameras;
    SDL_CameraSpec want;
    void *model;
    size_t model_size = 0;
    uint64_t last_ts = 0;
    int camera_count = 0;
    int processed = 0;
    int announced = 0;
    int i;

    if (!SDL_Init(SDL_INIT_CAMERA)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* Enumerate before touching the model so a camera-less machine (CI) can
     * exit cleanly without any assets. */
    cameras = SDL_GetCameras(&camera_count);
    if (!cameras || camera_count == 0) {
        printf("no cameras found\n");
        SDL_free(cameras);
        SDL_Quit();
        return 0;
    }
    for (i = 0; i < camera_count; ++i)
        printf("camera %d: %s\n", i, SDL_GetCameraName(cameras[i]));

    /* The spec is a request, not a contract: SDL converts what the hardware
     * produces when it can, and the loop below converts when it does not. */
    SDL_zero(want);
    want.format = SDL_PIXELFORMAT_RGBA32;
    want.width = 640;
    want.height = 480;
    want.framerate_numerator = 30;
    want.framerate_denominator = 1;
    cam = SDL_OpenCamera(cameras[0], &want);
    SDL_free(cameras);
    if (!cam) {
        fprintf(stderr, "SDL_OpenCamera: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    model = read_file(model_path, &model_size);
    if (!model) {
        fprintf(stderr, "cannot read model: %s\n", model_path);
        return 1;
    }
    face = ngh_face_create(model, model_size, &opt); /* threaded by default */
    if (!face) {
        fprintf(stderr, "create failed: %s\n", ngh_runtime_error());
        return 1;
    }
    if (ngh_face_result_alloc(&result, opt.max_faces, 1, 0) != NGH_OK) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    while (processed < MAX_FRAMES) {
        SDL_Surface *frame, *use, *converted = NULL;
        uint64_t ts_ns = 0, ts_ms;
        int count;

        /* Camera permission and MediaPipe startup are the same shape: poll
         * until both say ready. On desktop both usually resolve instantly;
         * on platforms with a permission prompt this loop simply waits. */
        int perm = SDL_GetCameraPermissionState(cam);
        if (perm < 0) {
            fprintf(stderr, "camera permission denied\n");
            break;
        }
        if (perm == 0 || ngh_face_state(face) != NGH_READY) {
            if (ngh_face_state(face) == NGH_FAILED) {
                fprintf(stderr, "create failed: %s\n", ngh_face_error(face));
                break;
            }
            SDL_Delay(50);
            continue;
        }
        if (!announced) {
            SDL_CameraSpec got;
            announced = 1;
            if (SDL_GetCameraFormat(cam, &got))
                printf("streaming %dx%d @ %g fps (%s)\n", got.width, got.height,
                       (double)got.framerate_numerator /
                           (double)got.framerate_denominator,
                       SDL_GetPixelFormatName(got.format));
        }

        frame = SDL_AcquireCameraFrame(cam, &ts_ns);
        if (!frame) {
            SDL_Delay(2); /* no new frame yet */
            continue;
        }

        use = frame;
        if (frame->format != SDL_PIXELFORMAT_RGBA32) {
            converted = SDL_ConvertSurface(frame, SDL_PIXELFORMAT_RGBA32);
            if (!converted) {
                SDL_ReleaseCameraFrame(cam, frame);
                continue;
            }
            use = converted;
        }

        ts_ms = ts_ns / 1000000u;
        if (ts_ms <= last_ts) ts_ms = last_ts + 1;
        last_ts = ts_ms;

        {
            ngh_image img =
                ngh_image_rgba(use->pixels, use->w, use->h, use->pitch);
            count = ngh_face_detect(face, &img, ts_ms, &result);
        }
        SDL_DestroySurface(converted);
        SDL_ReleaseCameraFrame(cam, frame);

        if (count < 0) {
            fprintf(stderr, "detect failed: %s\n", ngh_strerror(count));
            break;
        }
        ++processed;

        if (processed % 30 == 0) {
            if (result.count > 0) {
                const float *lm = result.landmarks;
                printf("[%3d] nose (%.3f, %.3f)  jawOpen %.2f  lag %dms\n",
                       processed, lm[1 * 3], lm[1 * 3 + 1],
                       result.blendshapes[25],
                       (int)(ts_ms - result.timestamp_ms));
            } else {
                printf("[%3d] no face\n", processed);
            }
        }
    }

    ngh_face_result_free(&result);
    ngh_face_destroy(face);
    free(model);
    SDL_CloseCamera(cam);
    SDL_Quit();
    return 0;
}
