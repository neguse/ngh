/* Pins the MediaPipe binary layout ngh reproduces.
 *
 * The compile-time NGH__ASSERT_LAYOUT checks inside the header already fail the
 * build on a mismatch; this test exists so a wrong layout is also visible as a
 * readable table when porting to a new target, and so CI reports which field
 * moved rather than just "static assertion failed".
 *
 * Upstream master has already diverged from 0.10.35 here: three extra
 * MpBaseOptions fields push every option struct out by 16 bytes. Loading a
 * newer libmediapipe against these offsets does not crash -- it silently
 * misreads running_mode and reports "Unsupported running mode: unknown mode".
 */

#define NGH_MEDIAPIPE_IMPLEMENTATION
#include "ngh_mediapipe.h"
#include "ngh_test.h"

#ifdef __EMSCRIPTEN__
int main(void) {
    printf("test_abi: skipped (web backend has no MediaPipe ABI)\n");
    return 0;
}
#else

#define FIELD(type, member) \
    { #type "." #member, offsetof(type, member) }

int main(void) {
    struct {
        const char *name;
        size_t got;
        size_t want;
    } sizes[] = {
        {"MpBaseOptions", sizeof(ngh__mp_base_options), 56},
        {"MpImageProcessingOptions", sizeof(ngh__mp_ipo), 24},
        {"MpRectF", sizeof(ngh__mp_rect_f), 16},
        {"MpNormalizedLandmark", sizeof(ngh__mp_landmark), 40},
        {"MpNormalizedLandmarks", sizeof(ngh__mp_landmarks), 16},
        {"MpCategory", sizeof(ngh__mp_category), 24},
        {"MpCategories", sizeof(ngh__mp_categories), 16},
        {"MpMatrix", sizeof(ngh__mp_matrix), 16},
        {"MpFaceLandmarkerOptions", sizeof(ngh__mp_face_options), 88},
        {"MpFaceLandmarkerResult", sizeof(ngh__mp_face_result), 48},
        {"MpPoseLandmarkerOptions", sizeof(ngh__mp_pose_options), 88},
        {"MpPoseLandmarkerResult", sizeof(ngh__mp_pose_result), 48},
    };
    struct {
        const char *name;
        size_t got;
        size_t want;
    } offsets[] = {
        {"MpBaseOptions.model_asset_buffer_count",
         offsetof(ngh__mp_base_options, model_asset_buffer_count), 8},
        {"MpBaseOptions.model_asset_path",
         offsetof(ngh__mp_base_options, model_asset_path), 16},
        {"MpBaseOptions.delegate", offsetof(ngh__mp_base_options, delegate),
         24},
        {"MpBaseOptions.host_version",
         offsetof(ngh__mp_base_options, host_version), 40},
        {"MpBaseOptions.ca_bundle_path",
         offsetof(ngh__mp_base_options, ca_bundle_path), 48},
        {"MpImageProcessingOptions.rotation_degrees",
         offsetof(ngh__mp_ipo, rotation_degrees), 20},
        {"MpNormalizedLandmark.visibility",
         offsetof(ngh__mp_landmark, visibility), 16},
        {"MpNormalizedLandmark.presence", offsetof(ngh__mp_landmark, presence),
         24},
        {"MpNormalizedLandmark.name", offsetof(ngh__mp_landmark, name), 32},
        {"MpCategory.category_name",
         offsetof(ngh__mp_category, category_name), 8},
        {"MpCategory.display_name", offsetof(ngh__mp_category, display_name),
         16},
        {"MpMatrix.data", offsetof(ngh__mp_matrix, data), 8},
        {"MpFaceLandmarkerOptions.running_mode",
         offsetof(ngh__mp_face_options, running_mode), 56},
        {"MpFaceLandmarkerOptions.output_face_blendshapes",
         offsetof(ngh__mp_face_options, output_face_blendshapes), 76},
        {"MpFaceLandmarkerOptions.result_callback",
         offsetof(ngh__mp_face_options, result_callback), 80},
        {"MpFaceLandmarkerResult.face_blendshapes",
         offsetof(ngh__mp_face_result, face_blendshapes), 16},
        {"MpFaceLandmarkerResult.facial_transformation_matrixes",
         offsetof(ngh__mp_face_result, facial_transformation_matrixes), 32},
        {"MpPoseLandmarkerOptions.running_mode",
         offsetof(ngh__mp_pose_options, running_mode), 56},
        {"MpPoseLandmarkerOptions.result_callback",
         offsetof(ngh__mp_pose_options, result_callback), 80},
        {"MpPoseLandmarkerResult.pose_landmarks",
         offsetof(ngh__mp_pose_result, pose_landmarks), 16},
        {"MpPoseLandmarkerResult.pose_world_landmarks",
         offsetof(ngh__mp_pose_result, pose_world_landmarks), 32},
    };
    size_t i;

    printf("test_abi (MediaPipe " NGH_MEDIAPIPE_ABI ")\n");

    NGH_TEST_CASE("sizeof(void*) == 8");
    NGH_CHECK_EQ_INT(sizeof(void *), 8);
    NGH_TEST_DONE();

    NGH_TEST_CASE("struct sizes");
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        if (sizes[i].got != sizes[i].want)
            ngh_test_fail(__FILE__, __LINE__, "sizeof(%s) = %u, want %u",
                          sizes[i].name, (unsigned)sizes[i].got,
                          (unsigned)sizes[i].want);
        ++ngh_test_checks;
    }
    NGH_TEST_DONE();

    NGH_TEST_CASE("field offsets");
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        if (offsets[i].got != offsets[i].want)
            ngh_test_fail(__FILE__, __LINE__, "offsetof(%s) = %u, want %u",
                          offsets[i].name, (unsigned)offsets[i].got,
                          (unsigned)offsets[i].want);
        ++ngh_test_checks;
    }
    NGH_TEST_DONE();

    /* MpRectF is declared left/top/bottom/right upstream, not the
     * left/top/right/bottom that every reader assumes. */
    NGH_TEST_CASE("MpRectF field order is l/t/b/r");
    NGH_CHECK_EQ_INT(offsetof(ngh__mp_rect_f, bottom), 8);
    NGH_CHECK_EQ_INT(offsetof(ngh__mp_rect_f, right), 12);
    NGH_TEST_DONE();

    /* MediaPipe's C++ default member initialisers do not exist in the binary,
     * so a zeroed options struct produces "max_vec_size should be >= 1". */
    NGH_TEST_CASE("options defaults are non-zero");
    {
        ngh_face_options f = ngh_face_options_default();
        ngh_pose_options p = ngh_pose_options_default();
        NGH_CHECK_EQ_INT(f.max_faces, 1);
        NGH_CHECK_NEAR(f.min_detection_confidence, 0.5, 1e-6);
        NGH_CHECK_EQ_INT(p.max_poses, 1);
        NGH_CHECK_NEAR(p.min_tracking_confidence, 0.5, 1e-6);
    }
    NGH_TEST_DONE();

    return ngh_test_report("test_abi");
}
#endif /* !__EMSCRIPTEN__ */
