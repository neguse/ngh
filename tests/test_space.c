/* Coordinate conventions and the helpers that normalize them.
 *
 * MediaPipe's spaces are barely documented upstream; these tests pin what ngh
 * claims about them against MediaPipe's own golden data, so a runtime update
 * that changes a convention fails here instead of in someone's scene graph.
 *
 * Verified facts (see the header's coordinate-helpers section):
 *   face transform  metric space, right-handed, y UP, -z forward, cm.
 *                   Golden translation for portrait.jpg: (-0.37, +22.8, -65.8)
 *                   -- face 66cm in front of the camera, above centre.
 *   pose world      metres, hip origin, x right, y DOWN, +z away.
 *                   nose y = -0.62 (above hips => y down), nose has the
 *                   most-negative z (nearest the camera => +z away).
 *   normalized      z more negative = closer (nose z < 0 on a frontal face).
 */

#define NGH_MEDIAPIPE_IMPLEMENTATION
#include "ngh_mediapipe.h"

#include "ngh_test.h"
#ifndef __EMSCRIPTEN__
#include "stb_image.h"
#endif

/* q must be (x,y,z,w); rebuild a column-major TRS and compare with m. */
static void recompose(const float pos[3], const float q[4], float s,
                      float out[16]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    out[0] = s * (1 - 2 * (y * y + z * z));
    out[1] = s * (2 * (x * y + z * w));
    out[2] = s * (2 * (x * z - y * w));
    out[3] = 0;
    out[4] = s * (2 * (x * y - z * w));
    out[5] = s * (1 - 2 * (x * x + z * z));
    out[6] = s * (2 * (y * z + x * w));
    out[7] = 0;
    out[8] = s * (2 * (x * z + y * w));
    out[9] = s * (2 * (y * z - x * w));
    out[10] = s * (1 - 2 * (x * x + y * y));
    out[11] = 0;
    out[12] = pos[0] * 100.0f; /* helper converts cm->m; undo for compare */
    out[13] = pos[1] * 100.0f;
    out[14] = pos[2] * 100.0f;
    out[15] = 1;
}

static void test_decompose_pure(void) {
    /* Identity: frontal face at the origin. */
    static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                    0, 0, 1, 0, 0, 0, 0, 1};
    /* 90 degrees about +y with scale 2 and a translation, column-major:
     * columns are the images of the basis vectors. */
    static const float turned[16] = {0, 0, -2, 0, 0, 2, 0,   0,
                                     2, 0, 0,  0, 3, 4, -50, 1};
    float pos[3], q[4], s, back[16];
    int i;

    NGH_TEST_CASE("decompose: identity");
    NGH_CHECK_EQ_INT(ngh_face_transform_decompose(ident, pos, q, &s), NGH_OK);
    NGH_CHECK_NEAR(s, 1.0f, 1e-6);
    NGH_CHECK_NEAR(q[3], 1.0f, 1e-6);
    NGH_CHECK_NEAR(q[0] * q[0] + q[1] * q[1] + q[2] * q[2], 0.0f, 1e-10);
    NGH_TEST_DONE();

    NGH_TEST_CASE("decompose: rotation + scale round-trips");
    NGH_CHECK_EQ_INT(ngh_face_transform_decompose(turned, pos, q, &s), NGH_OK);
    NGH_CHECK_NEAR(s, 2.0f, 1e-5);
    NGH_CHECK_NEAR(pos[2], -0.5f, 1e-6); /* -50cm -> -0.5m */
    recompose(pos, q, s, back);
    for (i = 0; i < 16; ++i) NGH_CHECK_NEAR(back[i], turned[i], 1e-4);
    NGH_TEST_DONE();

    NGH_TEST_CASE("decompose: rejects degenerate input");
    {
        static const float zero[16] = {0};
        NGH_CHECK_EQ_INT(ngh_face_transform_decompose(zero, pos, q, &s),
                         NGH_ERR_INVALID_ARG);
        NGH_CHECK_EQ_INT(ngh_face_transform_decompose(NULL, pos, q, &s),
                         NGH_ERR_INVALID_ARG);
    }
    NGH_TEST_DONE();
}

static void test_view_helper(void) {
    float out[3];
    NGH_TEST_CASE("landmark_to_view: axes and aspect");
    /* Centre of a 16:9 image maps to the origin. */
    ngh_landmark_to_view(0.5f, 0.5f, 0.0f, 1920, 1080, out);
    NGH_CHECK_NEAR(out[0], 0.0f, 1e-6);
    NGH_CHECK_NEAR(out[1], 0.0f, 1e-6);
    /* Top of the image is +y (up), and x carries the aspect ratio. */
    ngh_landmark_to_view(1.0f, 0.0f, 0.0f, 1920, 1080, out);
    NGH_CHECK_NEAR(out[0], 0.5f * (1920.0f / 1080.0f), 1e-4);
    NGH_CHECK_NEAR(out[1], 0.5f, 1e-6);
    /* MediaPipe's "more negative = closer" becomes +z toward the viewer. */
    ngh_landmark_to_view(0.5f, 0.5f, -0.1f, 1000, 1000, out);
    NGH_CHECK(out[2] > 0.0f);
    NGH_TEST_DONE();
}

static void test_world_helper(void) {
    float out[3];
    static const float nose_world[3] = {-0.086f, -0.615f, -0.147f};
    NGH_TEST_CASE("pose_world_to_camera: y flips up, z flips toward");
    ngh_pose_world_to_camera(nose_world, out);
    NGH_CHECK_NEAR(out[0], -0.086f, 1e-6);
    NGH_CHECK(out[1] > 0.5f); /* the nose sits above the hips */
    NGH_CHECK(out[2] > 0.0f); /* and toward the viewer */
    NGH_TEST_DONE();
}

#ifdef __EMSCRIPTEN__
int main(void) {
    printf("test_space (pure only under emscripten)\n");
    test_decompose_pure();
    test_view_helper();
    test_world_helper();
    return ngh_test_report("test_space");
}
#else

static char g_root[512] = ".";

static void path_of(char *out, size_t cap, const char *rel) {
    snprintf(out, cap, "%s/%s", g_root, rel);
}

static void test_face_space_live(void) {
    ngh_face_options opt = ngh_face_options_default();
    ngh_face_result res;
    ngh_face *face;
    unsigned char *px;
    void *model;
    size_t model_size;
    char path[768];
    int w = 0, h = 0, ch, n, i;
    float pos[3], q[4], s, back[16], qlen;

    NGH_TEST_CASE("live: face metric space is y-up, -z fwd, cm");
    path_of(path, sizeof(path), "vendor/testdata/portrait.jpg");
    px = stbi_load(path, &w, &h, &ch, 3);
    path_of(path, sizeof(path),
            "vendor/testdata/face_landmarker_v2_with_blendshapes.task");
    model = ngh_test_read_file(path, &model_size);
    if (!px || !model) {
        ngh_test_fail(__FILE__, __LINE__, "missing test data");
        NGH_TEST_DONE();
        return;
    }
    opt.threaded = 0;
    opt.blendshapes = 0;
    opt.transform = 1;
    face = ngh_face_create(model, model_size, &opt);
    NGH_CHECK_EQ_INT(ngh_face_result_alloc(&res, 1, 0, 1), NGH_OK);
    {
        ngh_image img = ngh_image_rgb(px, w, h, 0);
        n = ngh_face_detect(face, &img, 1, &res);
    }
    NGH_CHECK_EQ_INT(n, 1);
    if (n == 1) {
        /* The portrait face: slightly left of centre, above centre, about
         * two thirds of a metre from the camera. Golden translation is
         * (-0.37, +22.8, -65.8) in centimetres. */
        NGH_CHECK_EQ_INT(
            ngh_face_transform_decompose(res.transform, pos, q, &s), NGH_OK);
        NGH_CHECK_NEAR(pos[0], 0.0f, 0.05);
        NGH_CHECK_NEAR(pos[1], 0.23f, 0.05);
        NGH_CHECK_NEAR(pos[2], -0.66f, 0.05);
        NGH_CHECK_NEAR(s, 1.0f, 0.02); /* scale is absorbed upstream */
        qlen = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        NGH_CHECK_NEAR(qlen, 1.0f, 1e-4);
        /* A frontal portrait decomposes to a near-identity rotation. */
        NGH_CHECK(q[3] > 0.99f);
        /* And the decomposition must actually reproduce the matrix. */
        recompose(pos, q, s, back);
        for (i = 0; i < 16; ++i) NGH_CHECK_NEAR(back[i], res.transform[i], 5e-3);
        /* Normalized z: the nose is the closest point on a frontal face. */
        NGH_CHECK(res.landmarks[1 * 3 + 2] < 0.0f);
    }
    ngh_face_result_free(&res);
    ngh_face_destroy(face);
    free(model);
    stbi_image_free(px);
    NGH_TEST_DONE();
}

static void test_pose_space_live(void) {
    ngh_pose_options opt = ngh_pose_options_default();
    ngh_pose_result res;
    ngh_pose *pose;
    unsigned char *px;
    void *model;
    size_t model_size;
    char path[768];
    int w = 0, h = 0, ch, n;

    NGH_TEST_CASE("live: pose world is y-down, +z away, hip origin");
    path_of(path, sizeof(path), "vendor/testdata/pose.jpg");
    px = stbi_load(path, &w, &h, &ch, 3);
    path_of(path, sizeof(path), "vendor/testdata/pose_landmarker.task");
    model = ngh_test_read_file(path, &model_size);
    if (!px || !model) {
        ngh_test_fail(__FILE__, __LINE__, "missing test data");
        NGH_TEST_DONE();
        return;
    }
    opt.threaded = 0;
    opt.world_landmarks = 1;
    pose = ngh_pose_create(model, model_size, &opt);
    NGH_CHECK_EQ_INT(ngh_pose_result_alloc(&res, 1, 1), NGH_OK);
    {
        ngh_image img = ngh_image_rgb(px, w, h, 0);
        n = ngh_pose_detect(pose, &img, 1, &res);
    }
    NGH_CHECK_EQ_INT(n, 1);
    if (n == 1) {
        const float *nose = res.world_landmarks + 0 * 4;
        const float *lhip = res.world_landmarks + 23 * 4;
        const float *rhip = res.world_landmarks + 24 * 4;
        float cam[3];
        /* Hip centre is the origin. */
        NGH_CHECK_NEAR((lhip[0] + rhip[0]) * 0.5f, 0.0f, 0.05);
        NGH_CHECK_NEAR((lhip[1] + rhip[1]) * 0.5f, 0.0f, 0.05);
        /* Raw: the nose is above the hips, so raw y must be NEGATIVE (y
         * points down). If this fires, MediaPipe changed conventions and
         * ngh_pose_world_to_camera needs revisiting. */
        NGH_CHECK(nose[1] < -0.2f);
        /* Converted: up is up. */
        ngh_pose_world_to_camera(nose, cam);
        NGH_CHECK(cam[1] > 0.2f);
        /* Human proportions in metres, not centimetres or normalized units. */
        NGH_CHECK(cam[1] > 0.3f && cam[1] < 1.2f);
    }
    ngh_pose_result_free(&res);
    ngh_pose_destroy(pose);
    free(model);
    stbi_image_free(px);
    NGH_TEST_DONE();
}

int main(int argc, char **argv) {
    if (argc > 1) snprintf(g_root, sizeof(g_root), "%s", argv[1]);
    printf("test_space (root=%s)\n", g_root);

    test_decompose_pure();
    test_view_helper();
    test_world_helper();

    if (ngh_runtime_load(NULL) != NGH_OK) {
        printf("  SKIP live checks: %s\n", ngh_runtime_error());
        return ngh_test_report("test_space") ? 1 : 77;
    }
    test_face_space_live();
    test_pose_space_live();
    return ngh_test_report("test_space");
}
#endif /* __EMSCRIPTEN__ */
