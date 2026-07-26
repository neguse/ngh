/* Web backend wiring, run under node.
 *
 * The real thing needs a browser (MediaPipe's web runtime wants WebGL and a
 * DOM), so this covers the parts that do not: the EM_JS registry, handle
 * allocation, the pending -> failed transition and error propagation, and the
 * argument checks that run before anything reaches JavaScript.
 *
 * The bundle URL is pointed at a path that cannot resolve, so the failure is
 * deterministic and needs no network. Getting NGH_FAILED with a message back
 * proves the whole promise chain and the C/JS error handoff work.
 */

#define NGH_MEDIAPIPE_IMPLEMENTATION
#include "ngh_mediapipe.h"

#include "ngh_test.h"

#ifndef __EMSCRIPTEN__
int main(void) {
    printf("test_web: skipped (not an Emscripten build)\n");
    return 0;
}
#else

#include <emscripten.h>

static ngh_face *g_face;
static ngh_pose *g_pose;
static unsigned char g_model[64];

static void test_sync_contracts(void) {
    ngh_face_result r;
    ngh_image img;
    unsigned char px[4 * 4 * 4];

    NGH_TEST_CASE("validation runs before reaching JS");
    NGH_CHECK_EQ_INT(ngh_face_result_alloc(&r, 1, 0, 0), NGH_OK);
    img = ngh_image_rgba(px, 4, 4, 0);

    NGH_CHECK_EQ_INT(ngh_face_detect(NULL, &img, 1, &r), NGH_ERR_INVALID_ARG);
    NGH_CHECK_EQ_INT(ngh_face_detect(g_face, NULL, 1, &r), NGH_ERR_INVALID_ARG);
    NGH_CHECK_EQ_INT(ngh_face_detect(g_face, &img, 1, NULL),
                     NGH_ERR_INVALID_ARG);
    {
        ngh_image bad = img;
        bad.rotation_degrees = 45;
        NGH_CHECK_EQ_INT(ngh_face_detect(g_face, &bad, 1, &r),
                         NGH_ERR_INVALID_ARG);
    }
    {
        ngh_face_result small = r;
        small.capacity = 0;
        NGH_CHECK_EQ_INT(ngh_face_detect(g_face, &img, 1, &small),
                         NGH_ERR_CAPACITY);
    }
    ngh_face_result_free(&r);
    NGH_TEST_DONE();
}

static void test_source_selector(void) {
    NGH_TEST_CASE("web source selector rejects misses");
    /* No DOM under node, so querySelector cannot find anything. */
    NGH_CHECK_EQ_INT(ngh_web_source_from_selector("#nothing-here"),
                     NGH_ERR_INVALID_ARG);
    NGH_CHECK_EQ_INT(ngh_web_source_from_selector(""), NGH_ERR_INVALID_ARG);
    NGH_CHECK_EQ_INT(ngh_web_source_from_selector(NULL), NGH_ERR_INVALID_ARG);
    NGH_TEST_DONE();
}

static void finish(void *unused) {
    int rc;
    (void)unused;

    NGH_TEST_CASE("failed boot surfaces as FAILED + message");
    NGH_CHECK_EQ_INT(ngh_face_state(g_face), NGH_FAILED);
    NGH_CHECK(ngh_face_error(g_face) != NULL);
    NGH_CHECK_EQ_INT(ngh_pose_state(g_pose), NGH_FAILED);
    NGH_TEST_DONE();

    NGH_TEST_CASE("detect on a failed task reports NOT_READY");
    {
        ngh_face_result r;
        unsigned char px[4 * 4 * 4];
        ngh_image img = ngh_image_rgba(px, 4, 4, 0);
        NGH_CHECK_EQ_INT(ngh_face_result_alloc(&r, 1, 0, 0), NGH_OK);
        NGH_CHECK_EQ_INT(ngh_face_detect(g_face, &img, 1, &r),
                         NGH_ERR_NOT_READY);
        ngh_face_result_free(&r);
    }
    NGH_TEST_DONE();

    ngh_face_destroy(g_face);
    ngh_pose_destroy(g_pose);

    rc = ngh_test_report("test_web");
    /* Not exit(): main() already returned, so the runtime is still alive
     * waiting on the callback we are in. */
    emscripten_force_exit(rc);
}

int main(void) {
    ngh_face_options fopt = ngh_face_options_default();
    ngh_pose_options popt = ngh_pose_options_default();

    printf("test_web\n");

    ngh_web_set_bundle_url("./ngh-no-such-bundle.mjs");
    ngh_web_set_wasm_base("./ngh-no-such-wasm");

    NGH_TEST_CASE("runtime load is a no-op that succeeds");
    NGH_CHECK_EQ_INT(ngh_runtime_load(NULL), NGH_OK);
    NGH_CHECK(ngh_runtime_loaded());
    NGH_CHECK(strstr(ngh_runtime_path(), "ngh-no-such-bundle") != NULL);
    NGH_TEST_DONE();

    g_face = ngh_face_create(g_model, sizeof(g_model), &fopt);
    g_pose = ngh_pose_create(g_model, sizeof(g_model), &popt);

    NGH_TEST_CASE("create returns a handle, state starts PENDING");
    NGH_CHECK(g_face != NULL);
    NGH_CHECK(g_pose != NULL);
    NGH_CHECK_EQ_INT(ngh_face_state(g_face), NGH_PENDING);
    NGH_CHECK_EQ_INT(ngh_pose_state(g_pose), NGH_PENDING);
    NGH_TEST_DONE();

    NGH_TEST_CASE("bad arguments to create return NULL");
    NGH_CHECK(ngh_face_create(NULL, 8, &fopt) == NULL);
    NGH_CHECK(ngh_face_create(g_model, 0, &fopt) == NULL);
    {
        ngh_face_options zero = fopt;
        zero.max_faces = 0;
        NGH_CHECK(ngh_face_create(g_model, sizeof(g_model), &zero) == NULL);
    }
    NGH_TEST_DONE();

    test_sync_contracts();
    test_source_selector();

    /* Let the rejected import settle before checking the terminal state. */
    emscripten_async_call(finish, NULL, 50);
    return 0;
}
#endif /* __EMSCRIPTEN__ */
