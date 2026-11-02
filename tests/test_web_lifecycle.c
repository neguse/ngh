/* Web task lifecycle against a fake tasks-vision bundle, run under node.
 *
 * tests/web_fake_tasks_vision.mjs stands in for MediaPipe: creation is
 * asynchronous, the GPU delegate always fails, and it counts the tasks it
 * makes and the ones closed. That covers what test_web cannot reach with a
 * bundle that fails to load: a task destroyed while its create is still in
 * flight must be released when the create lands, and the GPU -> CPU retry
 * must report CPU.
 */

#define NGH_MEDIAPIPE_IMPLEMENTATION
#include "ngh_mediapipe.h"

#include "ngh_test.h"

#ifndef __EMSCRIPTEN__
int main(void) {
    printf("test_web_lifecycle: skipped (not an Emscripten build)\n");
    return 0;
}
#else

#include <emscripten.h>

#ifndef NGH_FAKE_BUNDLE
#error "NGH_FAKE_BUNDLE must name tests/web_fake_tasks_vision.mjs"
#endif

static unsigned char g_model[64];
static ngh_face *g_kept;

EM_JS(int, fake_created, (void), { return globalThis.__nghFake.created; })
EM_JS(int, fake_closed, (void), { return globalThis.__nghFake.closed; })

static void finish(void *unused) {
    int rc;
    (void)unused;

    NGH_TEST_CASE("a task destroyed while PENDING is closed when it lands");
    /* The CPU task destroyed early, plus the kept task's CPU retry. The AUTO
     * task destroyed early must not retry on the CPU at all. */
    NGH_CHECK_EQ_INT(fake_created(), 2);
    NGH_CHECK_EQ_INT(fake_closed(), 1);
    NGH_TEST_DONE();

    NGH_TEST_CASE("GPU failure retries on the CPU and reports it");
    NGH_CHECK_EQ_INT(ngh_face_state(g_kept), NGH_READY);
    NGH_CHECK_EQ_INT(ngh_face_accel(g_kept), NGH_ACCEL_CPU);
    NGH_TEST_DONE();

    NGH_TEST_CASE("destroying a READY task closes it");
    ngh_face_destroy(g_kept);
    NGH_CHECK_EQ_INT(fake_closed(), 2);
    NGH_TEST_DONE();

    rc = ngh_test_report("test_web_lifecycle");
    emscripten_force_exit(rc);
}

int main(void) {
    ngh_face_options cpu = ngh_face_options_default();
    ngh_face_options automatic = ngh_face_options_default();
    ngh_face *early_cpu, *early_auto;

    printf("test_web_lifecycle\n");
    cpu.accel = NGH_ACCEL_CPU;
    ngh_web_set_bundle_url(NGH_FAKE_BUNDLE);

    early_cpu = ngh_face_create(g_model, sizeof(g_model), &cpu);
    early_auto = ngh_face_create(g_model, sizeof(g_model), &automatic);
    g_kept = ngh_face_create(g_model, sizeof(g_model), &automatic);

    NGH_TEST_CASE("handles start PENDING");
    NGH_CHECK_EQ_INT(ngh_face_state(early_cpu), NGH_PENDING);
    NGH_CHECK_EQ_INT(ngh_face_state(g_kept), NGH_PENDING);
    NGH_TEST_DONE();

    ngh_face_destroy(early_cpu);
    ngh_face_destroy(early_auto);

    /* The fake takes 20ms per create attempt; the kept task needs two. */
    emscripten_async_call(finish, NULL, 300);
    return 0;
}
#endif /* __EMSCRIPTEN__ */
