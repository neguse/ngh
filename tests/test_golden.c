/* End-to-end check against MediaPipe's own golden files.
 *
 * Uses the same test bundles, the same images and the same tolerances as the
 * upstream C++ tests (face_landmarker_test.cc, pose_landmarker_test.cc):
 *
 *   landmarks   0.03   (kLandmarksDiffMargin / kLandmarksOnVideoAbsMargin)
 *   blendshapes 0.12   (kBlendshapesDiffMargin)
 *
 * Upstream compares x and y only and clears z, because z drifts noticeably
 * between frames while tracking. We do the same.
 *
 * Run from the repository root after scripts/fetch_*.py, or pass the directory
 * holding vendor/ as argv[1].
 */

#define NGH_MEDIAPIPE_IMPLEMENTATION
#include "ngh_mediapipe.h"

#include "ngh_test.h"
#include "stb_image.h" /* implementation lives in tests/stb_image_impl.c */

#define LANDMARK_MARGIN 0.03f
#define BLENDSHAPE_MARGIN 0.12f

static char g_root[512] = ".";

static void path_of(char *out, size_t cap, const char *rel) {
    snprintf(out, cap, "%s/%s", g_root, rel);
}

static unsigned char *load_rgb(const char *rel, int *w, int *h) {
    char path[768];
    unsigned char *px;
    int channels;
    path_of(path, sizeof(path), rel);
    px = stbi_load(path, w, h, &channels, 3);
    if (!px) printf("\n    cannot read %s: %s\n", path, stbi_failure_reason());
    return px;
}

static char *load_text(const char *rel) {
    char path[768];
    char *text;
    path_of(path, sizeof(path), rel);
    text = ngh_test_read_file(path, NULL);
    if (!text) printf("\n    cannot read %s\n", path);
    return text;
}

static void *load_blob(const char *rel, size_t *size) {
    char path[768];
    void *blob;
    path_of(path, sizeof(path), rel);
    blob = ngh_test_read_file(path, size);
    if (!blob) printf("\n    cannot read %s\n", path);
    return blob;
}

/* Reports the largest absolute deviation so a near-miss shows how near. */
static void compare(const char *what, const float *got, const float *want,
                    int n, float margin) {
    float worst = 0.0f;
    int worst_at = -1;
    int i;
    for (i = 0; i < n; ++i) {
        float d = got[i] - want[i];
        if (d < 0.0f) d = -d;
        if (d > worst) {
            worst = d;
            worst_at = i;
        }
    }
    ++ngh_test_checks;
    if (worst > margin) {
        ngh_test_fail(__FILE__, __LINE__,
                      "%s: max deviation %.5f at index %d exceeds %.5f "
                      "(got %.5f, want %.5f)",
                      what, worst, worst_at, margin, got[worst_at],
                      want[worst_at]);
    } else {
        printf("[max %.5f] ", worst);
    }
}

static int test_face(void) {
    static float want[NGH_FACE_LANDMARKS * 2];
    static float want_bs[NGH_FACE_BLENDSHAPES];
    static float got_xy[NGH_FACE_LANDMARKS * 2];
    ngh_face_options opt = ngh_face_options_default();
    ngh_face_result res;
    ngh_face *face;
    unsigned char *px;
    char *golden;
    void *model;
    size_t model_size;
    int w = 0, h = 0, n, i, count;

    NGH_TEST_CASE("face: portrait.jpg vs golden");

    px = load_rgb("vendor/testdata/portrait.jpg", &w, &h);
    model = load_blob("vendor/testdata/face_landmarker_v2_with_blendshapes.task",
                      &model_size);
    if (!px || !model) {
        ngh_test_fail(__FILE__, __LINE__, "missing test data");
        NGH_TEST_DONE();
        return 1;
    }

    /* Synchronous so the result belongs to the frame we just submitted. */
    opt.threaded = 0;
    opt.blendshapes = 1;
    face = ngh_face_create(model, model_size, &opt);
    if (!face || ngh_face_state(face) != NGH_READY) {
        ngh_test_fail(__FILE__, __LINE__, "create failed: %s",
                      face ? ngh_face_error(face) : ngh_runtime_error());
        NGH_TEST_DONE();
        return 1;
    }

    NGH_CHECK_EQ_INT(ngh_face_result_alloc(&res, 1, 1, 0), NGH_OK);
    {
        ngh_image img = ngh_image_rgb(px, w, h, 0);
        count = ngh_face_detect(face, &img, 1, &res);
    }
    if (count != 1) {
        ngh_test_fail(__FILE__, __LINE__, "detect returned %d (%s)", count,
                      count < 0 ? ngh_strerror(count) : "expected 1 face");
        goto done;
    }
    NGH_CHECK_EQ_INT(res.timestamp_ms, 1);

    golden = load_text("vendor/testdata/portrait_expected_face_landmarks.pbtxt");
    if (!golden) goto done;
    {
        static float xs[NGH_FACE_LANDMARKS], ys[NGH_FACE_LANDMARKS];
        int nx = ngh_test_scan_floats(golden, "x", xs, NGH_FACE_LANDMARKS);
        int ny = ngh_test_scan_floats(golden, "y", ys, NGH_FACE_LANDMARKS);
        NGH_CHECK_EQ_INT(nx, NGH_FACE_LANDMARKS);
        NGH_CHECK_EQ_INT(ny, NGH_FACE_LANDMARKS);
        for (i = 0; i < NGH_FACE_LANDMARKS; ++i) {
            want[i * 2 + 0] = xs[i];
            want[i * 2 + 1] = ys[i];
            got_xy[i * 2 + 0] = res.landmarks[i * 3 + 0];
            got_xy[i * 2 + 1] = res.landmarks[i * 3 + 1];
        }
    }
    free(golden);
    compare("landmarks x/y", got_xy, want, NGH_FACE_LANDMARKS * 2,
            LANDMARK_MARGIN);

    golden = load_text("vendor/testdata/portrait_expected_blendshapes.pbtxt");
    if (!golden) goto done;
    n = ngh_test_scan_floats(golden, "score", want_bs, NGH_FACE_BLENDSHAPES);
    free(golden);
    NGH_CHECK_EQ_INT(n, NGH_FACE_BLENDSHAPES);
    compare("blendshapes", res.blendshapes, want_bs, NGH_FACE_BLENDSHAPES,
            BLENDSHAPE_MARGIN);

done:
    ngh_face_result_free(&res);
    ngh_face_destroy(face);
    free(model);
    stbi_image_free(px);
    NGH_TEST_DONE();
    return 0;
}

static int test_pose(void) {
    static float want[NGH_POSE_LANDMARKS * 2];
    static float got_xy[NGH_POSE_LANDMARKS * 2];
    ngh_pose_options opt = ngh_pose_options_default();
    ngh_pose_result res;
    ngh_pose *pose;
    unsigned char *px;
    char *golden;
    void *model;
    size_t model_size;
    int w = 0, h = 0, i, count;

    NGH_TEST_CASE("pose: pose.jpg vs golden");

    px = load_rgb("vendor/testdata/pose.jpg", &w, &h);
    model = load_blob("vendor/testdata/pose_landmarker.task", &model_size);
    if (!px || !model) {
        ngh_test_fail(__FILE__, __LINE__, "missing test data");
        NGH_TEST_DONE();
        return 1;
    }

    opt.threaded = 0;
    pose = ngh_pose_create(model, model_size, &opt);
    if (!pose || ngh_pose_state(pose) != NGH_READY) {
        ngh_test_fail(__FILE__, __LINE__, "create failed: %s",
                      pose ? ngh_pose_error(pose) : ngh_runtime_error());
        NGH_TEST_DONE();
        return 1;
    }

    NGH_CHECK_EQ_INT(ngh_pose_result_alloc(&res, 1, 0), NGH_OK);
    {
        ngh_image img = ngh_image_rgb(px, w, h, 0);
        count = ngh_pose_detect(pose, &img, 1, &res);
    }
    if (count != 1) {
        ngh_test_fail(__FILE__, __LINE__, "detect returned %d (%s)", count,
                      count < 0 ? ngh_strerror(count) : "expected 1 pose");
        goto done;
    }

    golden = load_text("vendor/testdata/pose_landmarks.pbtxt");
    if (!golden) goto done;
    {
        static float xs[NGH_POSE_LANDMARKS], ys[NGH_POSE_LANDMARKS];
        int nx = ngh_test_scan_floats(golden, "x", xs, NGH_POSE_LANDMARKS);
        int ny = ngh_test_scan_floats(golden, "y", ys, NGH_POSE_LANDMARKS);
        NGH_CHECK_EQ_INT(nx, NGH_POSE_LANDMARKS);
        NGH_CHECK_EQ_INT(ny, NGH_POSE_LANDMARKS);
        for (i = 0; i < NGH_POSE_LANDMARKS; ++i) {
            want[i * 2 + 0] = xs[i];
            want[i * 2 + 1] = ys[i];
            got_xy[i * 2 + 0] = res.landmarks[i * 4 + 0];
            got_xy[i * 2 + 1] = res.landmarks[i * 4 + 1];
        }
    }
    free(golden);
    compare("landmarks x/y", got_xy, want, NGH_POSE_LANDMARKS * 2,
            LANDMARK_MARGIN);

    /* Visibility must survive the copy: it is the fourth float per landmark and
     * the only per-point confidence pose exposes. */
    NGH_CHECK(res.landmarks[3] > 0.5f);

done:
    ngh_pose_result_free(&res);
    ngh_pose_destroy(pose);
    free(model);
    stbi_image_free(px);
    NGH_TEST_DONE();
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) snprintf(g_root, sizeof(g_root), "%s", argv[1]);
    printf("test_golden (root=%s)\n", g_root);

    if (ngh_runtime_load(NULL) != NGH_OK) {
        printf("  SKIP: %s\n", ngh_runtime_error());
        return 77; /* automake convention for "skipped" */
    }
    printf("  runtime: %s\n", ngh_runtime_path());

    test_face();
    test_pose();
    return ngh_test_report("test_golden");
}
