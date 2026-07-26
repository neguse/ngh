/* Smallest useful ngh program: one image in, landmarks out.
 *
 *   python3 scripts/fetch_libmediapipe.py
 *   python3 scripts/fetch_models.py
 *   python3 scripts/fetch_testdata.py
 *   cmake -B build && cmake --build build
 *   NGH_MEDIAPIPE_PATH=vendor/libmediapipe.so ./build/ngh_still_image \
 *       vendor/face_landmarker.task vendor/testdata/portrait.jpg
 *
 * Synchronous mode, because there is only one frame and we want its result
 * before the program ends. A real-time loop wants threaded mode instead --
 * see game_loop.c.
 */

#define NGH_MEDIAPIPE_IMPLEMENTATION
#include "ngh_mediapipe.h"

#include <stdio.h>
#include <stdlib.h>

#include "stb_image.h"

/* The 52 blendshape scores come back in the order MediaPipe's model card
 * documents; these are the ones a face rig usually reads first. */
static const struct {
    int index;
    const char *name;
} kInterestingBlendshapes[] = {
    {9, "eyeBlinkLeft"}, {10, "eyeBlinkRight"}, {25, "jawOpen"},
    {44, "mouthSmileLeft"}, {45, "mouthSmileRight"},
};

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
    const char *image_path =
        argc > 2 ? argv[2] : "vendor/testdata/portrait.jpg";
    ngh_face_options opt = ngh_face_options_default();
    ngh_face_result result;
    ngh_face *face;
    unsigned char *pixels;
    void *model;
    size_t model_size = 0;
    int width, height, channels, count, i;

    model = read_file(model_path, &model_size);
    if (!model) {
        fprintf(stderr, "cannot read model: %s\n", model_path);
        return 1;
    }

    pixels = stbi_load(image_path, &width, &height, &channels, 3);
    if (!pixels) {
        fprintf(stderr, "cannot read image %s: %s\n", image_path,
                stbi_failure_reason());
        return 1;
    }

    opt.threaded = 0;
    opt.blendshapes = 1;
    face = ngh_face_create(model, model_size, &opt);
    if (!face || ngh_face_state(face) != NGH_READY) {
        fprintf(stderr, "create failed: %s\n",
                face ? ngh_face_error(face) : ngh_runtime_error());
        return 1;
    }
    printf("runtime : %s\n", ngh_runtime_path());
    printf("accel   : %s\n",
           ngh_face_accel(face) == NGH_ACCEL_GPU ? "GPU" : "CPU");
    printf("image   : %dx%d\n", width, height);

    if (ngh_face_result_alloc(&result, opt.max_faces, 1, 0) != NGH_OK) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    {
        ngh_image image = ngh_image_rgb(pixels, width, height, 0);
        count = ngh_face_detect(face, &image, 1, &result);
    }
    if (count < 0) {
        fprintf(stderr, "detect failed: %s\n", ngh_strerror(count));
        return 1;
    }
    printf("faces   : %d\n\n", count);

    for (i = 0; i < count; ++i) {
        const float *lm = result.landmarks + (size_t)i * NGH_FACE_LANDMARKS * 3;
        const float *bs = result.blendshapes + (size_t)i * NGH_FACE_BLENDSHAPES;
        size_t k;
        printf("face %d\n", i);
        /* 1 is the nose tip and 33/263 the outer eye corners: the three points
         * MediaPipe itself uses to orient the face. */
        printf("  nose tip     (%.4f, %.4f, %.4f)\n", lm[1 * 3], lm[1 * 3 + 1],
               lm[1 * 3 + 2]);
        printf("  eye outer L  (%.4f, %.4f)\n", lm[33 * 3], lm[33 * 3 + 1]);
        printf("  eye outer R  (%.4f, %.4f)\n", lm[263 * 3], lm[263 * 3 + 1]);
        for (k = 0; k < sizeof(kInterestingBlendshapes) /
                            sizeof(kInterestingBlendshapes[0]);
             ++k) {
            printf("  %-12s %.4f\n", kInterestingBlendshapes[k].name,
                   bs[kInterestingBlendshapes[k].index]);
        }
    }

    ngh_face_result_free(&result);
    ngh_face_destroy(face);
    stbi_image_free(pixels);
    free(model);
    return 0;
}
