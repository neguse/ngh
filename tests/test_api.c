/* API contract: argument validation, buffer ownership, timestamp rules, and
 * the equivalences callers are entitled to rely on (strided == packed input,
 * threaded == synchronous results). */

#define NGH_MEDIAPIPE_IMPLEMENTATION
#include "ngh_mediapipe.h"

#include "ngh_test.h"
#include "stb_image.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static char g_root[512] = ".";
static void *g_model = NULL;
static size_t g_model_size = 0;
static unsigned char *g_portrait = NULL;
static int g_w = 0, g_h = 0;

static void path_of(char *out, size_t cap, const char *rel) {
    snprintf(out, cap, "%s/%s", g_root, rel);
}

/* ------------------------------------------------- runtime-free contracts -- */

static void test_strerror(void) {
    int codes[] = {NGH_OK,          NGH_ERR_INVALID_ARG, NGH_ERR_NOT_READY,
                   NGH_ERR_TIMESTAMP, NGH_ERR_BACKEND,   NGH_ERR_CAPACITY,
                   NGH_ERR_NO_RUNTIME, NGH_ERR_OOM};
    size_t i;
    NGH_TEST_CASE("strerror covers every code");
    for (i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        const char *s = ngh_strerror(codes[i]);
        NGH_CHECK(s != NULL);
        NGH_CHECK(strcmp(s, "unknown error") != 0);
    }
    NGH_CHECK(strcmp(ngh_strerror(-9999), "unknown error") == 0);
    NGH_TEST_DONE();
}

static void test_image_helpers(void) {
    unsigned char px[16] = {0};
    ngh_image rgba = ngh_image_rgba(px, 2, 2, 8);
    ngh_image rgb = ngh_image_rgb(px, 2, 2, 6);
    NGH_TEST_CASE("image helpers");
    NGH_CHECK_EQ_INT(rgba.format, NGH_PIXFMT_RGBA8);
    NGH_CHECK_EQ_INT(rgb.format, NGH_PIXFMT_RGB8);
    NGH_CHECK_EQ_INT(rgba.kind, NGH_IMAGE_PIXELS);
    NGH_CHECK_EQ_INT(rgba.rotation_degrees, 0);
    NGH_CHECK(rgba.pixels == px);
    NGH_TEST_DONE();
}

static void test_result_alloc(void) {
    ngh_face_result f;
    ngh_pose_result p;
    ngh_face_result borrowed;
    float stack_buf[NGH_FACE_LANDMARKS * 3];

    NGH_TEST_CASE("result alloc / free");
    NGH_CHECK_EQ_INT(ngh_face_result_alloc(&f, 2, 1, 1), NGH_OK);
    NGH_CHECK_EQ_INT(f.capacity, 2);
    NGH_CHECK(f.landmarks && f.blendshapes && f.transform);
    ngh_face_result_free(&f);
    NGH_CHECK(f.landmarks == NULL);
    ngh_face_result_free(&f); /* freeing twice must be harmless */

    NGH_CHECK_EQ_INT(ngh_face_result_alloc(&f, 1, 0, 0), NGH_OK);
    NGH_CHECK(f.blendshapes == NULL && f.transform == NULL);
    ngh_face_result_free(&f);

    NGH_CHECK_EQ_INT(ngh_face_result_alloc(&f, 0, 0, 0), NGH_ERR_INVALID_ARG);
    NGH_CHECK_EQ_INT(ngh_face_result_alloc(NULL, 1, 0, 0),
                     NGH_ERR_INVALID_ARG);

    /* A caller-provided buffer must not be freed by ngh. */
    memset(&borrowed, 0, sizeof(borrowed));
    borrowed.capacity = 1;
    borrowed.landmarks = stack_buf;
    ngh_face_result_free(&borrowed);
    NGH_CHECK(borrowed.landmarks == stack_buf);

    NGH_CHECK_EQ_INT(ngh_pose_result_alloc(&p, 1, 1), NGH_OK);
    NGH_CHECK(p.landmarks && p.world_landmarks);
    ngh_pose_result_free(&p);
    NGH_TEST_DONE();
}

static void test_null_handles(void) {
    ngh_face_result r;
    ngh_image img;
    unsigned char px[4] = {0, 0, 0, 255};
    memset(&r, 0, sizeof(r));
    img = ngh_image_rgba(px, 1, 1, 0);

    NGH_TEST_CASE("NULL handles do not crash");
    NGH_CHECK_EQ_INT(ngh_face_state(NULL), NGH_FAILED);
    NGH_CHECK_EQ_INT(ngh_pose_state(NULL), NGH_FAILED);
    NGH_CHECK_EQ_INT(ngh_face_accel(NULL), NGH_ACCEL_CPU);
    NGH_CHECK_EQ_INT(ngh_face_detect(NULL, &img, 1, &r), NGH_ERR_INVALID_ARG);
    ngh_face_destroy(NULL);
    ngh_pose_destroy(NULL);
    NGH_TEST_DONE();
}

static void test_bad_runtime_path(void) {
    NGH_TEST_CASE("explicit bad runtime path reports an error");
    if (ngh_runtime_loaded()) {
        /* Already loaded: load() is idempotent and must not tear it down. */
        NGH_CHECK_EQ_INT(ngh_runtime_load("/nonexistent/libmediapipe.so"),
                         NGH_OK);
        NGH_CHECK(ngh_runtime_loaded());
    } else {
        NGH_CHECK_EQ_INT(ngh_runtime_load("/nonexistent/libmediapipe.so"),
                         NGH_ERR_NO_RUNTIME);
        NGH_CHECK(ngh_runtime_error() != NULL);
    }
    NGH_TEST_DONE();
}

/* ------------------------------------------------------ runtime contracts -- */

static ngh_face *make_face(int threaded) {
    ngh_face_options opt = ngh_face_options_default();
    opt.threaded = threaded;
    opt.blendshapes = 0;
    return ngh_face_create(g_model, g_model_size, &opt);
}

static void test_detect_validation(void) {
    ngh_face *f = make_face(0);
    ngh_face_result r;
    ngh_image img;
    ngh_image bad;

    NGH_TEST_CASE("detect argument validation");
    if (!f || ngh_face_state(f) != NGH_READY) {
        ngh_test_fail(__FILE__, __LINE__, "create failed");
        ngh_face_destroy(f);
        NGH_TEST_DONE();
        return;
    }
    NGH_CHECK_EQ_INT(ngh_face_result_alloc(&r, 1, 0, 0), NGH_OK);
    img = ngh_image_rgb(g_portrait, g_w, g_h, 0);

    NGH_CHECK_EQ_INT(ngh_face_detect(f, NULL, 1, &r), NGH_ERR_INVALID_ARG);
    NGH_CHECK_EQ_INT(ngh_face_detect(f, &img, 1, NULL), NGH_ERR_INVALID_ARG);

    bad = img;
    bad.rotation_degrees = 45; /* MediaPipe only accepts multiples of 90 */
    NGH_CHECK_EQ_INT(ngh_face_detect(f, &bad, 1, &r), NGH_ERR_INVALID_ARG);

    bad = img;
    bad.pixels = NULL;
    NGH_CHECK_EQ_INT(ngh_face_detect(f, &bad, 1, &r), NGH_ERR_INVALID_ARG);

    {
        ngh_face_result small;
        memset(&small, 0, sizeof(small));
        small.capacity = 0;
        small.landmarks = r.landmarks;
        NGH_CHECK_EQ_INT(ngh_face_detect(f, &img, 1, &small),
                         NGH_ERR_CAPACITY);
    }

    ngh_face_result_free(&r);
    ngh_face_destroy(f);
    NGH_TEST_DONE();
}

static void test_timestamp_monotonic(void) {
    ngh_face *f = make_face(0);
    ngh_face_result r;
    ngh_image img;

    NGH_TEST_CASE("timestamps must strictly increase");
    if (!f || ngh_face_state(f) != NGH_READY) {
        ngh_test_fail(__FILE__, __LINE__, "create failed");
        ngh_face_destroy(f);
        NGH_TEST_DONE();
        return;
    }
    NGH_CHECK_EQ_INT(ngh_face_result_alloc(&r, 1, 0, 0), NGH_OK);
    img = ngh_image_rgb(g_portrait, g_w, g_h, 0);

    NGH_CHECK(ngh_face_detect(f, &img, 100, &r) >= 0);
    /* Rejected by ngh before MediaPipe can abort the whole graph over it. */
    NGH_CHECK_EQ_INT(ngh_face_detect(f, &img, 100, &r), NGH_ERR_TIMESTAMP);
    NGH_CHECK_EQ_INT(ngh_face_detect(f, &img, 99, &r), NGH_ERR_TIMESTAMP);
    NGH_CHECK(ngh_face_detect(f, &img, 101, &r) >= 0);

    ngh_face_result_free(&r);
    ngh_face_destroy(f);
    NGH_TEST_DONE();
}

/* A padded copy of the image must give bit-identical results to the packed one:
 * this is the row-packing path in ngh__pack_rows(). */
static void test_strided_input(void) {
    ngh_face *f;
    ngh_face_result packed, strided;
    int stride = g_w * 3 + 37;
    unsigned char *buf = (unsigned char *)malloc((size_t)stride * (size_t)g_h);
    int i, n1, n2;

    NGH_TEST_CASE("strided input == packed input");
    if (!buf) {
        ngh_test_fail(__FILE__, __LINE__, "oom");
        NGH_TEST_DONE();
        return;
    }
    memset(buf, 0xAB, (size_t)stride * (size_t)g_h);
    for (i = 0; i < g_h; ++i)
        memcpy(buf + (size_t)stride * (size_t)i,
               g_portrait + (size_t)g_w * 3 * (size_t)i, (size_t)g_w * 3);

    NGH_CHECK_EQ_INT(ngh_face_result_alloc(&packed, 1, 0, 0), NGH_OK);
    NGH_CHECK_EQ_INT(ngh_face_result_alloc(&strided, 1, 0, 0), NGH_OK);

    f = make_face(0);
    {
        ngh_image img = ngh_image_rgb(g_portrait, g_w, g_h, 0);
        n1 = ngh_face_detect(f, &img, 1, &packed);
    }
    ngh_face_destroy(f);

    f = make_face(0);
    {
        ngh_image img = ngh_image_rgb(buf, g_w, g_h, stride);
        n2 = ngh_face_detect(f, &img, 1, &strided);
    }
    ngh_face_destroy(f);

    NGH_CHECK_EQ_INT(n1, 1);
    NGH_CHECK_EQ_INT(n2, 1);
    if (n1 == 1 && n2 == 1) {
        for (i = 0; i < NGH_FACE_LANDMARKS * 3; ++i)
            NGH_CHECK_NEAR(strided.landmarks[i], packed.landmarks[i], 1e-6);
    }

    ngh_face_result_free(&packed);
    ngh_face_result_free(&strided);
    free(buf);
    NGH_TEST_DONE();
}

/* Threading must not change what the model sees, only when we see it back. */
static void test_threaded_matches_sync(void) {
    ngh_face *sync_face, *thr_face;
    ngh_face_result sync_r, thr_r;
    ngh_image img;
    int i, n, tries;

    NGH_TEST_CASE("threaded result matches synchronous");
    NGH_CHECK_EQ_INT(ngh_face_result_alloc(&sync_r, 1, 0, 0), NGH_OK);
    NGH_CHECK_EQ_INT(ngh_face_result_alloc(&thr_r, 1, 0, 0), NGH_OK);
    img = ngh_image_rgb(g_portrait, g_w, g_h, 0);

    sync_face = make_face(0);
    n = ngh_face_detect(sync_face, &img, 1, &sync_r);
    ngh_face_destroy(sync_face);
    NGH_CHECK_EQ_INT(n, 1);

    thr_face = make_face(1);
    if (!thr_face || ngh_face_state(thr_face) != NGH_READY) {
        ngh_test_fail(__FILE__, __LINE__, "create failed");
        goto done;
    }
    /* The first submission has nothing to return yet. Built with
     * NGH_NO_THREADS it falls back to the synchronous path and answers
     * immediately, which is also correct. */
    n = ngh_face_detect(thr_face, &img, 1, &thr_r);
    NGH_CHECK(n == 0 || n == 1);

    for (tries = 0; n == 0 && tries < 200; ++tries) {
        sleep_ms(10);
        n = ngh_face_detect(thr_face, &img, (uint64_t)(2 + tries), &thr_r);
        if (n < 0) {
            ngh_test_fail(__FILE__, __LINE__, "detect: %s", ngh_strerror(n));
            goto done;
        }
    }
    NGH_CHECK_EQ_INT(n, 1);
    NGH_CHECK(thr_r.timestamp_ms >= 1);
    if (n == 1) {
        /* The worker sees the same pixels, so the first frame it processes
         * lands on the same landmarks the synchronous path produced. */
        for (i = 0; i < NGH_FACE_LANDMARKS * 3; ++i)
            NGH_CHECK_NEAR(thr_r.landmarks[i], sync_r.landmarks[i], 0.02);
    }

done:
    ngh_face_destroy(thr_face);
    ngh_face_result_free(&sync_r);
    ngh_face_result_free(&thr_r);
    NGH_TEST_DONE();
}

static void test_pose_smoke(void) {
    ngh_pose_options opt = ngh_pose_options_default();
    ngh_pose_result r;
    ngh_pose *p;
    unsigned char *px;
    void *model;
    size_t model_size;
    char path[768];
    int w = 0, h = 0, channels, n;

    NGH_TEST_CASE("pose world landmarks are metric");
    path_of(path, sizeof(path), "vendor/testdata/pose.jpg");
    px = stbi_load(path, &w, &h, &channels, 3);
    path_of(path, sizeof(path), "vendor/testdata/pose_landmarker.task");
    model = ngh_test_read_file(path, &model_size);
    if (!px || !model) {
        ngh_test_fail(__FILE__, __LINE__, "missing test data");
        NGH_TEST_DONE();
        return;
    }

    opt.threaded = 0;
    opt.world_landmarks = 1;
    p = ngh_pose_create(model, model_size, &opt);
    NGH_CHECK_EQ_INT(ngh_pose_result_alloc(&r, 1, 1), NGH_OK);
    {
        ngh_image img = ngh_image_rgb(px, w, h, 0);
        n = ngh_pose_detect(p, &img, 1, &r);
    }
    NGH_CHECK_EQ_INT(n, 1);
    if (n == 1) {
        int i;
        float max_abs = 0.0f;
        /* Normalized landmarks sit in [0,1]; world landmarks are metres around
         * the hip centre, so they must look nothing like each other. */
        for (i = 0; i < NGH_POSE_LANDMARKS; ++i) {
            float v = r.world_landmarks[i * 4];
            if (v < 0.0f) v = -v;
            if (v > max_abs) max_abs = v;
        }
        NGH_CHECK(max_abs > 0.05f);
        NGH_CHECK(max_abs < 5.0f);
        NGH_CHECK(r.landmarks[0] >= -0.5f && r.landmarks[0] <= 1.5f);
    }

    ngh_pose_result_free(&r);
    ngh_pose_destroy(p);
    free(model);
    stbi_image_free(px);
    NGH_TEST_DONE();
}

int main(int argc, char **argv) {
    char path[768];
    int channels;

    if (argc > 1) snprintf(g_root, sizeof(g_root), "%s", argv[1]);
    printf("test_api (root=%s)\n", g_root);

    test_strerror();
    test_image_helpers();
    test_result_alloc();
    test_null_handles();
    test_bad_runtime_path();

    if (ngh_runtime_load(NULL) != NGH_OK) {
        printf("  SKIP rest: %s\n", ngh_runtime_error());
        return ngh_test_report("test_api");
    }

    path_of(path, sizeof(path),
            "vendor/testdata/face_landmarker_v2_with_blendshapes.task");
    g_model = ngh_test_read_file(path, &g_model_size);
    path_of(path, sizeof(path), "vendor/testdata/portrait.jpg");
    g_portrait = stbi_load(path, &g_w, &g_h, &channels, 3);
    if (!g_model || !g_portrait) {
        printf("  SKIP rest: run scripts/fetch_testdata.py first\n");
        return ngh_test_report("test_api");
    }

    test_detect_validation();
    test_timestamp_monotonic();
    test_strided_input();
    test_threaded_matches_sync();
    test_pose_smoke();

    free(g_model);
    stbi_image_free(g_portrait);
    return ngh_test_report("test_api");
}
