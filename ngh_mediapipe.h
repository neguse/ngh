/* ngh_mediapipe.h -- MediaPipe face / body tracking from C.
 *
 * Single-header, C99. Define NGH_MEDIAPIPE_IMPLEMENTATION in exactly one
 * translation unit before including this file:
 *
 *     #define NGH_MEDIAPIPE_IMPLEMENTATION
 *     #include "ngh_mediapipe.h"
 *
 * Backends
 *   Desktop (Windows / Linux / macOS)
 *       Resolves the official MediaPipe Tasks C API out of libmediapipe.{dll,so,dylib}
 *       at run time. Nothing is needed at link time. Fetch the library with
 *       scripts/fetch_libmediapipe.py.
 *   Web (Emscripten)
 *       Bridges to @mediapipe/tasks-vision through EM_JS. No extra link flags:
 *       ASYNCIFY is not required because MediaPipe's detect calls are synchronous.
 *
 * The same source builds against both. Task creation is asynchronous on the web,
 * so creation is always a non-blocking call followed by polling ngh_face_state().
 * On desktop the state is NGH_READY as soon as ngh_face_create() returns.
 *
 * Configuration macros (define before including the implementation)
 *   NGH_NO_THREADS          Drop the worker-thread code path entirely.
 *   NGH_WEB_BUNDLE_URL      Default @mediapipe/tasks-vision ESM bundle URL.
 *   NGH_WEB_WASM_BASE       Default directory holding vision_wasm_internal.*
 *
 * See README.md for the platform matrix and measured latencies.
 *
 * License: MIT (see LICENSE.txt). MediaPipe itself is Apache-2.0 and is not
 * redistributed here; see THIRDPARTY.md.
 */

#ifndef NGH_MEDIAPIPE_H_INCLUDED
#define NGH_MEDIAPIPE_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NGH_VERSION_MAJOR 0
#define NGH_VERSION_MINOR 1
#define NGH_VERSION_PATCH 0

/* The MediaPipe release whose binary ABI this header targets.
 *
 * The Tasks C API makes no ABI stability promise and has already changed:
 * upstream master grew three MpBaseOptions fields after 0.10.35, which moves
 * every option struct by 16 bytes. Loading a different build will not fail
 * loudly -- it produces garbage or a confusing "unknown mode" error. Pin this. */
#define NGH_MEDIAPIPE_ABI "0.10.35"

#if defined(__cplusplus) || \
    (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L)
#define NGH_INLINE static inline
#elif defined(_MSC_VER)
#define NGH_INLINE static __inline
#else
#define NGH_INLINE static
#endif

/* ------------------------------------------------------------------ status */

typedef enum ngh_state {
    NGH_PENDING = 0, /* creation in flight (web only) */
    NGH_READY = 1,
    NGH_FAILED = 2,
    NGH_LOST = 3 /* WebGL context lost; recreate the task */
} ngh_state;

typedef enum ngh_accel {
    NGH_ACCEL_AUTO = 0, /* GPU where it helps, CPU otherwise */
    NGH_ACCEL_CPU = 1,
    NGH_ACCEL_GPU = 2
} ngh_accel;

enum {
    NGH_OK = 0,
    NGH_ERR_INVALID_ARG = -1,
    NGH_ERR_NOT_READY = -2,
    NGH_ERR_TIMESTAMP = -3, /* timestamps must strictly increase */
    NGH_ERR_BACKEND = -4,
    NGH_ERR_CAPACITY = -5, /* result buffers too small */
    NGH_ERR_NO_RUNTIME = -6,
    NGH_ERR_OOM = -7
};

const char *ngh_strerror(int code);

/* ----------------------------------------------------------------- runtime */

/* Desktop only; a no-op that returns NGH_OK on the web.
 *
 * Pass NULL to search, in order: $NGH_MEDIAPIPE_PATH, the executable's own
 * directory, then the platform loader's default search path. Calling this is
 * optional -- the first ngh_*_create() loads the library lazily -- but doing it
 * up front lets you report a missing runtime before any other setup. */
int ngh_runtime_load(const char *path);
int ngh_runtime_loaded(void);
const char *ngh_runtime_path(void);  /* NULL until loaded */
const char *ngh_runtime_error(void); /* last load failure, else NULL */
void ngh_runtime_unload(void);

/* ------------------------------------------------------------------- image */

typedef enum ngh_pixfmt {
    NGH_PIXFMT_RGB8 = 1,
    NGH_PIXFMT_RGBA8 = 2
} ngh_pixfmt;

typedef enum ngh_image_kind {
    NGH_IMAGE_PIXELS = 0,
    NGH_IMAGE_WEB = 1 /* handle from ngh_web_source_from_selector() */
} ngh_image_kind;

/* A borrowed view of one frame. Plain data, no ownership, no allocation: the
 * pixels only have to stay valid for the duration of the ngh_*_detect() call. */
typedef struct ngh_image {
    ngh_image_kind kind;
    const uint8_t *pixels;
    int width;
    int height;
    int stride; /* bytes per row; 0 means tightly packed */
    ngh_pixfmt format;
    int web_source;       /* NGH_IMAGE_WEB only */
    int rotation_degrees; /* clockwise, must be a multiple of 90 */
} ngh_image;

NGH_INLINE ngh_image ngh_image_rgba(const uint8_t *pixels, int width,
                                    int height, int stride) {
    ngh_image img;
    img.kind = NGH_IMAGE_PIXELS;
    img.pixels = pixels;
    img.width = width;
    img.height = height;
    img.stride = stride;
    img.format = NGH_PIXFMT_RGBA8;
    img.web_source = -1;
    img.rotation_degrees = 0;
    return img;
}

NGH_INLINE ngh_image ngh_image_rgb(const uint8_t *pixels, int width, int height,
                                   int stride) {
    ngh_image img = ngh_image_rgba(pixels, width, height, stride);
    img.format = NGH_PIXFMT_RGB8;
    return img;
}

/* -------------------------------------------------------------------- face */

#define NGH_FACE_LANDMARKS 478
#define NGH_FACE_BLENDSHAPES 52
#define NGH_FACE_MATRIX 16

typedef struct ngh_face_options {
    ngh_accel accel;
    int max_faces;
    float min_detection_confidence;
    float min_presence_confidence;
    float min_tracking_confidence;
    int blendshapes; /* costs <0.1ms; on by default */
    int transform;   /* 4x4 facial transformation matrix, column-major */
    /* Run inference on a worker thread and make detect() non-blocking. Inference
     * takes ~9ms of a single core, which does not fit a 60fps frame budget.
     * Web ignores this: browsers give us no thread to spare here. */
    int threaded;
} ngh_face_options;

ngh_face_options ngh_face_options_default(void);

/* Caller-owned output buffers. Fill in capacity and the pointers, or call
 * ngh_face_result_alloc(). Unwanted outputs may be NULL. */
typedef struct ngh_face_result {
    int capacity;
    int count;             /* faces written */
    uint64_t timestamp_ms; /* frame these results came from */
    float *landmarks;      /* capacity * 478 * 3 (x, y, z), normalized */
    float *blendshapes;    /* capacity * 52, or NULL */
    float *transform;      /* capacity * 16, or NULL */
    int owned_;            /* set by ngh_face_result_alloc() */
} ngh_face_result;

int ngh_face_result_alloc(ngh_face_result *out, int capacity, int blendshapes,
                          int transform);
void ngh_face_result_free(ngh_face_result *r);

typedef struct ngh_face ngh_face;

/* The model bytes are copied; the caller keeps ownership. */
ngh_face *ngh_face_create(const void *model, size_t model_size,
                          const ngh_face_options *options);
ngh_state ngh_face_state(const ngh_face *f);
ngh_accel ngh_face_accel(const ngh_face *f); /* what is actually in use */

/* The message behind a non-READY state, else NULL.
 *
 * Detection failures come back through ngh_face_detect()'s return code, not
 * through this. In threaded mode the worker owns this buffer once detection is
 * running, so treat it as diagnostic only after the first detect() call. */
const char *ngh_face_error(const ngh_face *f);

/* Returns the number of faces written, or a negative NGH_ERR_*.
 *
 * Timestamps must strictly increase. In threaded mode this submits the frame
 * and returns the most recent finished result, so out->timestamp_ms may lag
 * ts_ms and frames may be dropped while the worker is busy -- that is the point.
 * Before the first result lands it returns 0 with count == 0. */
int ngh_face_detect(ngh_face *f, const ngh_image *image, uint64_t ts_ms,
                    ngh_face_result *out);
void ngh_face_destroy(ngh_face *f);

/* -------------------------------------------------------------------- pose */

#define NGH_POSE_LANDMARKS 33

typedef struct ngh_pose_options {
    ngh_accel accel;
    int max_poses;
    float min_detection_confidence;
    float min_presence_confidence;
    float min_tracking_confidence;
    int world_landmarks; /* metric coordinates, origin at the hip centre */
    int threaded;
} ngh_pose_options;

ngh_pose_options ngh_pose_options_default(void);

typedef struct ngh_pose_result {
    int capacity;
    int count;
    uint64_t timestamp_ms;
    float *landmarks;       /* capacity * 33 * 4 (x, y, z, visibility) */
    float *world_landmarks; /* capacity * 33 * 4, metres, or NULL */
    int owned_;
} ngh_pose_result;

int ngh_pose_result_alloc(ngh_pose_result *out, int capacity,
                          int world_landmarks);
void ngh_pose_result_free(ngh_pose_result *r);

typedef struct ngh_pose ngh_pose;

ngh_pose *ngh_pose_create(const void *model, size_t model_size,
                          const ngh_pose_options *options);
ngh_state ngh_pose_state(const ngh_pose *p);
const char *ngh_pose_error(const ngh_pose *p);
ngh_accel ngh_pose_accel(const ngh_pose *p);
int ngh_pose_detect(ngh_pose *p, const ngh_image *image, uint64_t ts_ms,
                    ngh_pose_result *out);
void ngh_pose_destroy(ngh_pose *p);

/* --------------------------------------------------------------------- web */

#ifdef __EMSCRIPTEN__
/* Override before the first create(). Both default to a pinned CDN URL, which
 * is fine for prototyping; ship the ~11.5MB wasm alongside your app instead of
 * depending on a third-party CDN in production. */
void ngh_web_set_bundle_url(const char *url);
void ngh_web_set_wasm_base(const char *base);

/* Wrap a live <video> or <canvas> so frames never round-trip through wasm
 * memory. This is the fast path on the web; ngh_image_rgba() also works and
 * keeps your code portable.
 *
 * A WebGL canvas created with preserveDrawingBuffer:false is cleared after
 * compositing, so pass it inside the same requestAnimationFrame callback that
 * drew it. Returns a handle, or a negative NGH_ERR_*. */
int ngh_web_source_from_selector(const char *css_selector);
void ngh_web_source_release(int source);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NGH_MEDIAPIPE_H_INCLUDED */

/* ==========================================================================
 *  Implementation
 * ========================================================================== */

#ifdef NGH_MEDIAPIPE_IMPLEMENTATION
#ifndef NGH_MEDIAPIPE_IMPLEMENTED
#define NGH_MEDIAPIPE_IMPLEMENTED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NGH__CAT2(a, b) a##b
#define NGH__CAT(a, b) NGH__CAT2(a, b)
#if defined(__cplusplus)
#define NGH__ASSERT_LAYOUT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define NGH__ASSERT_LAYOUT(cond, msg) _Static_assert(cond, msg)
#else
#define NGH__ASSERT_LAYOUT(cond, msg) \
    typedef char NGH__CAT(ngh__layout_, __LINE__)[(cond) ? 1 : -1]
#endif

const char *ngh_strerror(int code) {
    switch (code) {
        case NGH_OK:
            return "ok";
        case NGH_ERR_INVALID_ARG:
            return "invalid argument";
        case NGH_ERR_NOT_READY:
            return "task is not ready";
        case NGH_ERR_TIMESTAMP:
            return "timestamp must strictly increase";
        case NGH_ERR_BACKEND:
            return "backend error";
        case NGH_ERR_CAPACITY:
            return "result buffer too small";
        case NGH_ERR_NO_RUNTIME:
            return "mediapipe runtime not found";
        case NGH_ERR_OOM:
            return "out of memory";
        default:
            return "unknown error";
    }
}

ngh_face_options ngh_face_options_default(void) {
    ngh_face_options o;
    o.accel = NGH_ACCEL_AUTO;
    o.max_faces = 1;
    o.min_detection_confidence = 0.5f;
    o.min_presence_confidence = 0.5f;
    o.min_tracking_confidence = 0.5f;
    o.blendshapes = 1;
    o.transform = 0;
    o.threaded = 1;
    return o;
}

ngh_pose_options ngh_pose_options_default(void) {
    ngh_pose_options o;
    o.accel = NGH_ACCEL_AUTO;
    o.max_poses = 1;
    o.min_detection_confidence = 0.5f;
    o.min_presence_confidence = 0.5f;
    o.min_tracking_confidence = 0.5f;
    o.world_landmarks = 0;
    o.threaded = 1;
    return o;
}

int ngh_face_result_alloc(ngh_face_result *out, int capacity, int blendshapes,
                          int transform) {
    if (!out || capacity <= 0) return NGH_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->capacity = capacity;
    out->landmarks = (float *)calloc((size_t)capacity * NGH_FACE_LANDMARKS * 3,
                                     sizeof(float));
    if (!out->landmarks) return NGH_ERR_OOM;
    if (blendshapes) {
        out->blendshapes = (float *)calloc(
            (size_t)capacity * NGH_FACE_BLENDSHAPES, sizeof(float));
        if (!out->blendshapes) {
            ngh_face_result_free(out);
            return NGH_ERR_OOM;
        }
    }
    if (transform) {
        out->transform =
            (float *)calloc((size_t)capacity * NGH_FACE_MATRIX, sizeof(float));
        if (!out->transform) {
            ngh_face_result_free(out);
            return NGH_ERR_OOM;
        }
    }
    out->owned_ = 1;
    return NGH_OK;
}

void ngh_face_result_free(ngh_face_result *r) {
    if (!r || !r->owned_) return;
    free(r->landmarks);
    free(r->blendshapes);
    free(r->transform);
    memset(r, 0, sizeof(*r));
}

int ngh_pose_result_alloc(ngh_pose_result *out, int capacity,
                          int world_landmarks) {
    if (!out || capacity <= 0) return NGH_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->capacity = capacity;
    out->landmarks = (float *)calloc((size_t)capacity * NGH_POSE_LANDMARKS * 4,
                                     sizeof(float));
    if (!out->landmarks) return NGH_ERR_OOM;
    if (world_landmarks) {
        out->world_landmarks = (float *)calloc(
            (size_t)capacity * NGH_POSE_LANDMARKS * 4, sizeof(float));
        if (!out->world_landmarks) {
            ngh_pose_result_free(out);
            return NGH_ERR_OOM;
        }
    }
    out->owned_ = 1;
    return NGH_OK;
}

void ngh_pose_result_free(ngh_pose_result *r) {
    if (!r || !r->owned_) return;
    free(r->landmarks);
    free(r->world_landmarks);
    memset(r, 0, sizeof(*r));
}

/* Rows are copied into a tightly packed scratch buffer when the source is
 * strided; MediaPipe's MpImageCreateFromUint8Data wants contiguous pixels. */
static int ngh__pack_rows(const ngh_image *img, uint8_t **buf, size_t *cap,
                          const uint8_t **out_pixels, size_t *out_size) {
    int bpp = (img->format == NGH_PIXFMT_RGBA8) ? 4 : 3;
    size_t row = (size_t)img->width * (size_t)bpp;
    size_t need = row * (size_t)img->height;
    int stride = img->stride > 0 ? img->stride : (int)row;
    int y;

    if ((size_t)stride == row) {
        *out_pixels = img->pixels;
        *out_size = need;
        return NGH_OK;
    }
    if (*cap < need) {
        uint8_t *grown = (uint8_t *)realloc(*buf, need);
        if (!grown) return NGH_ERR_OOM;
        *buf = grown;
        *cap = need;
    }
    for (y = 0; y < img->height; ++y) {
        memcpy(*buf + row * (size_t)y, img->pixels + (size_t)stride * (size_t)y,
               row);
    }
    *out_pixels = *buf;
    *out_size = need;
    return NGH_OK;
}

static int ngh__image_valid(const ngh_image *img) {
    if (!img) return 0;
    if (img->rotation_degrees % 90 != 0) return 0;
    if (img->kind == NGH_IMAGE_WEB) return img->web_source >= 0;
    return img->pixels && img->width > 0 && img->height > 0;
}

#ifndef __EMSCRIPTEN__

/* ==========================================================================
 *  Desktop backend -- MediaPipe Tasks C API, resolved at run time
 * ========================================================================== */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if !defined(NGH_NO_THREADS) && !defined(_WIN32)
#include <pthread.h>
#endif

/* --------------------------------------------------------- MediaPipe ABI --
 *
 * Redeclared rather than #included: the upstream headers are not valid C.
 * They pull in <cstdint>, use C++ default member initialisers and declare a
 * typedef inside a struct. Every layout below is pinned by NGH__ASSERT_LAYOUT
 * so a mismatched build fails to compile instead of silently misbehaving. */

NGH__ASSERT_LAYOUT(sizeof(void *) == 8, "the MediaPipe C API ships 64-bit only");

enum { NGH__MP_DELEGATE_CPU = 0, NGH__MP_DELEGATE_GPU = 1 };
enum { NGH__MP_MODE_IMAGE = 1, NGH__MP_MODE_VIDEO = 2, NGH__MP_MODE_STREAM = 3 };
enum { NGH__MP_FMT_SRGB = 1, NGH__MP_FMT_SRGBA = 2 };
enum { NGH__MP_STATUS_OK = 0 };

typedef struct ngh__mp_base_options {
    const char *model_asset_buffer;
    unsigned int model_asset_buffer_count;
    const char *model_asset_path;
    int delegate;
    int host_environment;
    int host_system;
    const char *host_version;
    const char *ca_bundle_path;
} ngh__mp_base_options;

NGH__ASSERT_LAYOUT(sizeof(ngh__mp_base_options) == 56, "MpBaseOptions 0.10.35");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_base_options, model_asset_path) == 16, "");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_base_options, delegate) == 24, "");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_base_options, ca_bundle_path) == 48, "");

typedef struct ngh__mp_rect_f {
    float left, top, bottom, right; /* note: not left/top/right/bottom */
} ngh__mp_rect_f;

typedef struct ngh__mp_ipo {
    int has_region_of_interest; /* int, not bool -- the binary reads 4 bytes */
    ngh__mp_rect_f region_of_interest;
    int rotation_degrees;
} ngh__mp_ipo;

NGH__ASSERT_LAYOUT(sizeof(ngh__mp_ipo) == 24, "MpImageProcessingOptions");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_ipo, rotation_degrees) == 20, "");

/* MpNormalizedLandmark and MpLandmark share a layout. */
typedef struct ngh__mp_landmark {
    float x, y, z;
    unsigned char has_visibility; /* C++ bool: 1 byte on every target we ship */
    float visibility;
    unsigned char has_presence;
    float presence;
    char *name;
} ngh__mp_landmark;

NGH__ASSERT_LAYOUT(sizeof(ngh__mp_landmark) == 40, "MpLandmark");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_landmark, visibility) == 16, "");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_landmark, presence) == 24, "");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_landmark, name) == 32, "");

typedef struct ngh__mp_landmarks {
    ngh__mp_landmark *landmarks;
    uint32_t landmarks_count;
} ngh__mp_landmarks;

NGH__ASSERT_LAYOUT(sizeof(ngh__mp_landmarks) == 16, "MpLandmarks");

typedef struct ngh__mp_category {
    int index;
    float score;
    char *category_name;
    char *display_name;
} ngh__mp_category;

NGH__ASSERT_LAYOUT(sizeof(ngh__mp_category) == 24, "MpCategory");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_category, category_name) == 8, "");

typedef struct ngh__mp_categories {
    ngh__mp_category *categories;
    uint32_t categories_count;
} ngh__mp_categories;

typedef struct ngh__mp_matrix {
    uint32_t rows, cols;
    float *data; /* column-major */
} ngh__mp_matrix;

NGH__ASSERT_LAYOUT(sizeof(ngh__mp_matrix) == 16, "MpMatrix");

typedef struct ngh__mp_face_options {
    ngh__mp_base_options base_options;
    int running_mode;
    int num_faces;
    float min_face_detection_confidence;
    float min_face_presence_confidence;
    float min_tracking_confidence;
    unsigned char output_face_blendshapes;
    unsigned char output_facial_transformation_matrixes;
    void (*result_callback)(int, const void *, void *, int64_t);
} ngh__mp_face_options;

NGH__ASSERT_LAYOUT(sizeof(ngh__mp_face_options) == 88,
                   "MpFaceLandmarkerOptions 0.10.35");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_face_options, running_mode) == 56, "");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_face_options, result_callback) == 80, "");

typedef struct ngh__mp_face_result {
    ngh__mp_landmarks *face_landmarks;
    uint32_t face_landmarks_count;
    ngh__mp_categories *face_blendshapes;
    uint32_t face_blendshapes_count;
    ngh__mp_matrix *facial_transformation_matrixes;
    uint32_t facial_transformation_matrixes_count;
} ngh__mp_face_result;

NGH__ASSERT_LAYOUT(sizeof(ngh__mp_face_result) == 48, "MpFaceLandmarkerResult");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_face_result, face_blendshapes) == 16, "");

typedef struct ngh__mp_pose_options {
    ngh__mp_base_options base_options;
    int running_mode;
    int num_poses;
    float min_pose_detection_confidence;
    float min_pose_presence_confidence;
    float min_tracking_confidence;
    unsigned char output_segmentation_masks;
    void (*result_callback)(int, const void *, void *, int64_t);
} ngh__mp_pose_options;

NGH__ASSERT_LAYOUT(sizeof(ngh__mp_pose_options) == 88,
                   "MpPoseLandmarkerOptions 0.10.35");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_pose_options, running_mode) == 56, "");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_pose_options, result_callback) == 80, "");

typedef struct ngh__mp_pose_result {
    void **segmentation_masks;
    uint32_t segmentation_masks_count;
    ngh__mp_landmarks *pose_landmarks;
    uint32_t pose_landmarks_count;
    ngh__mp_landmarks *pose_world_landmarks;
    uint32_t pose_world_landmarks_count;
} ngh__mp_pose_result;

NGH__ASSERT_LAYOUT(sizeof(ngh__mp_pose_result) == 48, "MpPoseLandmarkerResult");
NGH__ASSERT_LAYOUT(offsetof(ngh__mp_pose_result, pose_landmarks) == 16, "");

/* ------------------------------------------------------ dynamic loading -- */

typedef struct ngh__runtime {
    void *handle;
    char path[1024];
    char error[1280]; /* has to hold a full path plus the message */
    int loaded;

    int (*image_from_u8)(int fmt, int w, int h, const uint8_t *px, int size,
                         void **out, char **err);
    void (*image_free)(void *img);
    void (*error_free)(char *msg);

    int (*face_create)(ngh__mp_face_options *opts, void **out, char **err);
    int (*face_video)(void *lm, void *img, const ngh__mp_ipo *ipo, int64_t ts,
                      ngh__mp_face_result *res, char **err);
    void (*face_close_result)(ngh__mp_face_result *res);
    int (*face_close)(void *lm, char **err);

    int (*pose_create)(ngh__mp_pose_options *opts, void **out, char **err);
    int (*pose_video)(void *lm, void *img, const ngh__mp_ipo *ipo, int64_t ts,
                      ngh__mp_pose_result *res, char **err);
    void (*pose_close_result)(ngh__mp_pose_result *res);
    int (*pose_close)(void *lm, char **err);
} ngh__runtime;

static ngh__runtime ngh__rt;

#if defined(_WIN32)
#define NGH__LIB_NAME "libmediapipe.dll"
#elif defined(__APPLE__)
#define NGH__LIB_NAME "libmediapipe.dylib"
#else
#define NGH__LIB_NAME "libmediapipe.so"
#endif

/* MSVC deprecates getenv (C4996); the CRT race it warns about cannot happen
 * here since nothing in this process rewrites the environment. */
static const char *ngh__getenv(const char *name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

static void *ngh__dlopen(const char *utf8_path) {
#if defined(_WIN32)
    wchar_t wide[1024];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wide,
                                (int)(sizeof(wide) / sizeof(wide[0])));
    if (n <= 0) return NULL;
    return (void *)LoadLibraryW(wide);
#else
    return dlopen(utf8_path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void *ngh__dlsym(void *handle, const char *name) {
#if defined(_WIN32)
    return (void *)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

static void ngh__dlclose(void *handle) {
#if defined(_WIN32)
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

/* Writes the directory containing the running executable, with a trailing
 * separator. Returns 0 when it cannot be determined. */
static int ngh__exe_dir(char *out, size_t cap) {
    size_t len = 0;
#if defined(_WIN32)
    wchar_t wide[1024];
    DWORD n = GetModuleFileNameW(NULL, wide, 1024);
    if (n == 0 || n >= 1024) return 0;
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL) ==
        0)
        return 0;
    len = strlen(out);
#elif defined(__APPLE__)
    uint32_t n = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &n) != 0) return 0;
    len = strlen(out);
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    len = (size_t)n;
#endif
    while (len > 0 && out[len - 1] != '/' && out[len - 1] != '\\') --len;
    out[len] = '\0';
    return len > 0;
}

static int ngh__bind(void) {
    ngh__runtime *r = &ngh__rt;
    struct {
        const char *name;
        void **slot;
    } table[] = {
        {"MpImageCreateFromUint8Data", (void **)&r->image_from_u8},
        {"MpImageFree", (void **)&r->image_free},
        {"MpErrorFree", (void **)&r->error_free},
        {"MpFaceLandmarkerCreate", (void **)&r->face_create},
        {"MpFaceLandmarkerDetectForVideo", (void **)&r->face_video},
        {"MpFaceLandmarkerCloseResult", (void **)&r->face_close_result},
        {"MpFaceLandmarkerClose", (void **)&r->face_close},
        {"MpPoseLandmarkerCreate", (void **)&r->pose_create},
        {"MpPoseLandmarkerDetectForVideo", (void **)&r->pose_video},
        {"MpPoseLandmarkerCloseResult", (void **)&r->pose_close_result},
        {"MpPoseLandmarkerClose", (void **)&r->pose_close},
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        *table[i].slot = ngh__dlsym(r->handle, table[i].name);
        if (!*table[i].slot) {
            snprintf(r->error, sizeof(r->error),
                     "%.1000s does not export %.64s (wrong or too old "
                     "MediaPipe? ngh targets " NGH_MEDIAPIPE_ABI ")",
                     r->path, table[i].name);
            return 0;
        }
    }
    return 1;
}

static int ngh__try_load(const char *path) {
    ngh__runtime *r = &ngh__rt;
    void *handle = ngh__dlopen(path);
    if (!handle) return 0;
    r->handle = handle;
    snprintf(r->path, sizeof(r->path), "%s", path);
    if (!ngh__bind()) {
        ngh__dlclose(handle);
        r->handle = NULL;
        r->path[0] = '\0';
        return 0;
    }
    r->loaded = 1;
    r->error[0] = '\0';
    return 1;
}

int ngh_runtime_load(const char *path) {
    char buf[1024];
    const char *env;

    if (ngh__rt.loaded) return NGH_OK;
    ngh__rt.error[0] = '\0';

    if (path && *path) {
        if (ngh__try_load(path)) return NGH_OK;
        if (!ngh__rt.error[0])
            snprintf(ngh__rt.error, sizeof(ngh__rt.error), "cannot load %s",
                     path);
        return NGH_ERR_NO_RUNTIME;
    }

    env = ngh__getenv("NGH_MEDIAPIPE_PATH");
    if (env && *env && ngh__try_load(env)) return NGH_OK;

    if (ngh__exe_dir(buf, sizeof(buf))) {
        size_t len = strlen(buf);
        snprintf(buf + len, sizeof(buf) - len, "%s", NGH__LIB_NAME);
        if (ngh__try_load(buf)) return NGH_OK;
    }

    if (ngh__try_load(NGH__LIB_NAME)) return NGH_OK;

    if (!ngh__rt.error[0]) {
        snprintf(ngh__rt.error, sizeof(ngh__rt.error),
                 "%s not found; set NGH_MEDIAPIPE_PATH or place it next to the "
                 "executable (scripts/fetch_libmediapipe.py fetches it)",
                 NGH__LIB_NAME);
    }
    return NGH_ERR_NO_RUNTIME;
}

int ngh_runtime_loaded(void) { return ngh__rt.loaded; }

const char *ngh_runtime_path(void) {
    return ngh__rt.loaded ? ngh__rt.path : NULL;
}

const char *ngh_runtime_error(void) {
    return ngh__rt.error[0] ? ngh__rt.error : NULL;
}

void ngh_runtime_unload(void) {
    if (!ngh__rt.handle) return;
    ngh__dlclose(ngh__rt.handle);
    memset(&ngh__rt, 0, sizeof(ngh__rt));
}

/* ------------------------------------------------------------- threading -- */

#ifndef NGH_NO_THREADS
#if defined(_WIN32)
typedef CRITICAL_SECTION ngh__mutex;
typedef CONDITION_VARIABLE ngh__cond;
typedef HANDLE ngh__thread;
#define ngh__mutex_init(m) InitializeCriticalSection(m)
#define ngh__mutex_destroy(m) DeleteCriticalSection(m)
#define ngh__lock(m) EnterCriticalSection(m)
#define ngh__unlock(m) LeaveCriticalSection(m)
#define ngh__cond_init(c) InitializeConditionVariable(c)
#define ngh__cond_destroy(c) ((void)(c))
#define ngh__cond_wait(c, m) SleepConditionVariableCS(c, m, INFINITE)
#define ngh__cond_signal(c) WakeConditionVariable(c)
#else
typedef pthread_mutex_t ngh__mutex;
typedef pthread_cond_t ngh__cond;
typedef pthread_t ngh__thread;
#define ngh__mutex_init(m) pthread_mutex_init(m, NULL)
#define ngh__mutex_destroy(m) pthread_mutex_destroy(m)
#define ngh__lock(m) pthread_mutex_lock(m)
#define ngh__unlock(m) pthread_mutex_unlock(m)
#define ngh__cond_init(c) pthread_cond_init(c, NULL)
#define ngh__cond_destroy(c) pthread_cond_destroy(c)
#define ngh__cond_wait(c, m) pthread_cond_wait(c, m)
#define ngh__cond_signal(c) pthread_cond_signal(c)
#endif

/* One in-flight frame and one published result. A frame submitted while the
 * worker is busy replaces the previous pending frame: for a game loop the
 * newest frame is the only one worth having. */
typedef struct ngh__worker {
    ngh__thread thread;
    ngh__mutex mu;
    ngh__cond cv;
    int started;
    int quit;

    uint8_t *pending;
    size_t pending_cap;
    size_t pending_size;
    int pending_valid;
    int w, h, fmt, rot;
    uint64_t ts;

    uint8_t *scratch; /* swapped with `pending` under the lock */
    size_t scratch_cap;
} ngh__worker;

static void ngh__worker_stop(ngh__worker *wk) {
    if (!wk->started) return;
    ngh__lock(&wk->mu);
    wk->quit = 1;
    ngh__cond_signal(&wk->cv);
    ngh__unlock(&wk->mu);
#if defined(_WIN32)
    WaitForSingleObject(wk->thread, INFINITE);
    CloseHandle(wk->thread);
#else
    pthread_join(wk->thread, NULL);
#endif
    wk->started = 0;
}

/* Only call this when the worker was successfully started -- the mutex and
 * condition variable are initialised on that same path. */
static void ngh__worker_destroy(ngh__worker *wk) {
    ngh__worker_stop(wk);
    ngh__cond_destroy(&wk->cv);
    ngh__mutex_destroy(&wk->mu);
    free(wk->pending);
    free(wk->scratch);
    wk->pending = NULL;
    wk->scratch = NULL;
}

/* Caller must hold wk->mu. */
static int ngh__worker_submit(ngh__worker *wk, const uint8_t *px, size_t size,
                              int w, int h, int fmt, int rot, uint64_t ts) {
    if (wk->pending_cap < size) {
        uint8_t *grown = (uint8_t *)realloc(wk->pending, size);
        if (!grown) return NGH_ERR_OOM;
        wk->pending = grown;
        wk->pending_cap = size;
    }
    memcpy(wk->pending, px, size);
    wk->pending_size = size;
    wk->w = w;
    wk->h = h;
    wk->fmt = fmt;
    wk->rot = rot;
    wk->ts = ts;
    wk->pending_valid = 1;
    ngh__cond_signal(&wk->cv);
    return NGH_OK;
}

/* Caller must hold wk->mu. Returns 0 when the worker should exit. */
static int ngh__worker_take(ngh__worker *wk, size_t *size, int *w, int *h,
                            int *fmt, int *rot, uint64_t *ts) {
    uint8_t *tmp;
    size_t tmp_cap;
    while (!wk->pending_valid && !wk->quit) ngh__cond_wait(&wk->cv, &wk->mu);
    if (wk->quit) return 0;
    tmp = wk->scratch;
    tmp_cap = wk->scratch_cap;
    wk->scratch = wk->pending;
    wk->scratch_cap = wk->pending_cap;
    wk->pending = tmp;
    wk->pending_cap = tmp_cap;
    wk->pending_valid = 0;
    *size = wk->pending_size;
    *w = wk->w;
    *h = wk->h;
    *fmt = wk->fmt;
    *rot = wk->rot;
    *ts = wk->ts;
    return 1;
}
#endif /* NGH_NO_THREADS */

/* ----------------------------------------------------------------- shared -- */

static int ngh__mp_delegate(ngh_accel accel) {
    /* AUTO means CPU on desktop. GPU loses on face landmarks even where it is
     * available (measured 12.0ms vs 9.2ms on a Vega iGPU), Windows builds have
     * no GPU path at all, and Linux needs EGL_PLATFORM=surfaceless. Opting in
     * is a deliberate choice, so we do not make it for the caller. */
    return accel == NGH_ACCEL_GPU ? NGH__MP_DELEGATE_GPU : NGH__MP_DELEGATE_CPU;
}

static void ngh__set_err(char *dst, size_t cap, const char *fallback,
                         char *mp_err) {
    if (mp_err) {
        snprintf(dst, cap, "%s", mp_err);
        if (ngh__rt.error_free) ngh__rt.error_free(mp_err);
    } else {
        snprintf(dst, cap, "%s", fallback);
    }
}

static int ngh__make_image(const uint8_t *px, size_t size, int w, int h,
                           int fmt, void **out, char *err, size_t err_cap) {
    char *mp_err = NULL;
    int st = ngh__rt.image_from_u8(fmt, w, h, px, (int)size, out, &mp_err);
    if (st != NGH__MP_STATUS_OK) {
        ngh__set_err(err, err_cap, "MpImageCreateFromUint8Data failed", mp_err);
        return NGH_ERR_BACKEND;
    }
    return NGH_OK;
}

/* ------------------------------------------------------------------ face -- */

struct ngh_face {
    void *lm;
    ngh_state state;
    ngh_accel accel;
    char error[512];

    int max_faces;
    int want_blendshapes;
    int want_transform;

    uint8_t *pack; /* row-packing scratch for strided input */
    size_t pack_cap;

    /* Filled by inference; copied out by ngh_face_detect(). */
    float *lm_buf;
    float *bs_buf;
    float *tf_buf;
    int count;
    uint64_t result_ts;
    int infer_err;

    uint64_t last_ts;
    int have_ts;

#ifndef NGH_NO_THREADS
    int threaded;
    ngh__worker wk;
    float *pub_lm;
    float *pub_bs;
    float *pub_tf;
    int pub_count;
    uint64_t pub_ts;
    int pub_valid;
    int pub_err;
#endif
};

static void ngh__face_read_result(ngh_face *f, const ngh__mp_face_result *res) {
    uint32_t n = res->face_landmarks_count;
    uint32_t i, j;
    if ((int)n > f->max_faces) n = (uint32_t)f->max_faces;
    for (i = 0; i < n; ++i) {
        const ngh__mp_landmarks *src = &res->face_landmarks[i];
        uint32_t cnt = src->landmarks_count;
        float *dst = f->lm_buf + (size_t)i * NGH_FACE_LANDMARKS * 3;
        if (cnt > NGH_FACE_LANDMARKS) cnt = NGH_FACE_LANDMARKS;
        for (j = 0; j < cnt; ++j) {
            dst[j * 3 + 0] = src->landmarks[j].x;
            dst[j * 3 + 1] = src->landmarks[j].y;
            dst[j * 3 + 2] = src->landmarks[j].z;
        }
    }
    if (f->bs_buf && res->face_blendshapes) {
        uint32_t bn = res->face_blendshapes_count;
        if (bn > n) bn = n;
        for (i = 0; i < bn; ++i) {
            const ngh__mp_categories *src = &res->face_blendshapes[i];
            uint32_t cnt = src->categories_count;
            float *dst = f->bs_buf + (size_t)i * NGH_FACE_BLENDSHAPES;
            if (cnt > NGH_FACE_BLENDSHAPES) cnt = NGH_FACE_BLENDSHAPES;
            for (j = 0; j < cnt; ++j) dst[j] = src->categories[j].score;
        }
    }
    if (f->tf_buf && res->facial_transformation_matrixes) {
        uint32_t mn = res->facial_transformation_matrixes_count;
        if (mn > n) mn = n;
        for (i = 0; i < mn; ++i) {
            const ngh__mp_matrix *src = &res->facial_transformation_matrixes[i];
            uint32_t cnt = src->rows * src->cols;
            float *dst = f->tf_buf + (size_t)i * NGH_FACE_MATRIX;
            if (cnt > NGH_FACE_MATRIX) cnt = NGH_FACE_MATRIX;
            for (j = 0; j < cnt; ++j) dst[j] = src->data[j];
        }
    }
    f->count = (int)n;
}

static void ngh__face_infer(ngh_face *f, const uint8_t *px, size_t size, int w,
                            int h, int fmt, int rot, uint64_t ts) {
    ngh__mp_face_result res;
    ngh__mp_ipo ipo;
    void *img = NULL;
    char *mp_err = NULL;
    int st;

    f->infer_err = NGH_OK;
    f->count = 0;
    f->result_ts = ts;

    if (ngh__make_image(px, size, w, h, fmt, &img, f->error, sizeof(f->error)) !=
        NGH_OK) {
        f->infer_err = NGH_ERR_BACKEND;
        return;
    }

    memset(&ipo, 0, sizeof(ipo));
    ipo.rotation_degrees = rot;
    memset(&res, 0, sizeof(res));

    st = ngh__rt.face_video(f->lm, img, &ipo, (int64_t)ts, &res, &mp_err);
    if (st != NGH__MP_STATUS_OK) {
        ngh__set_err(f->error, sizeof(f->error), "face detect failed", mp_err);
        f->infer_err = NGH_ERR_BACKEND;
    } else {
        ngh__face_read_result(f, &res);
        ngh__rt.face_close_result(&res);
    }
    ngh__rt.image_free(img);
}

#ifndef NGH_NO_THREADS
static void ngh__face_publish(ngh_face *f) {
    size_t lm_n = (size_t)f->max_faces * NGH_FACE_LANDMARKS * 3;
    memcpy(f->pub_lm, f->lm_buf, lm_n * sizeof(float));
    if (f->pub_bs)
        memcpy(f->pub_bs, f->bs_buf,
               (size_t)f->max_faces * NGH_FACE_BLENDSHAPES * sizeof(float));
    if (f->pub_tf)
        memcpy(f->pub_tf, f->tf_buf,
               (size_t)f->max_faces * NGH_FACE_MATRIX * sizeof(float));
    f->pub_count = f->count;
    f->pub_ts = f->result_ts;
    f->pub_err = f->infer_err;
    f->pub_valid = 1;
}

#if defined(_WIN32)
static DWORD WINAPI ngh__face_thread(LPVOID arg)
#else
static void *ngh__face_thread(void *arg)
#endif
{
    ngh_face *f = (ngh_face *)arg;
    for (;;) {
        size_t size;
        int w, h, fmt, rot;
        uint64_t ts;
        ngh__lock(&f->wk.mu);
        if (!ngh__worker_take(&f->wk, &size, &w, &h, &fmt, &rot, &ts)) {
            ngh__unlock(&f->wk.mu);
            break;
        }
        ngh__unlock(&f->wk.mu);

        ngh__face_infer(f, f->wk.scratch, size, w, h, fmt, rot, ts);

        ngh__lock(&f->wk.mu);
        ngh__face_publish(f);
        ngh__unlock(&f->wk.mu);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}
#endif /* NGH_NO_THREADS */

ngh_face *ngh_face_create(const void *model, size_t model_size,
                          const ngh_face_options *options) {
    ngh_face_options opt = options ? *options : ngh_face_options_default();
    ngh__mp_face_options mp;
    ngh_face *f;
    char *mp_err = NULL;
    int st;

    if (!model || model_size == 0 || opt.max_faces <= 0) return NULL;
    if (ngh_runtime_load(NULL) != NGH_OK) return NULL;

    f = (ngh_face *)calloc(1, sizeof(ngh_face));
    if (!f) return NULL;
    f->max_faces = opt.max_faces;
    f->want_blendshapes = opt.blendshapes ? 1 : 0;
    f->want_transform = opt.transform ? 1 : 0;
    f->accel = (opt.accel == NGH_ACCEL_GPU) ? NGH_ACCEL_GPU : NGH_ACCEL_CPU;

    f->lm_buf = (float *)calloc((size_t)opt.max_faces * NGH_FACE_LANDMARKS * 3,
                                sizeof(float));
    if (f->want_blendshapes)
        f->bs_buf = (float *)calloc(
            (size_t)opt.max_faces * NGH_FACE_BLENDSHAPES, sizeof(float));
    if (f->want_transform)
        f->tf_buf = (float *)calloc((size_t)opt.max_faces * NGH_FACE_MATRIX,
                                    sizeof(float));
    if (!f->lm_buf || (f->want_blendshapes && !f->bs_buf) ||
        (f->want_transform && !f->tf_buf)) {
        ngh_face_destroy(f);
        return NULL;
    }

    memset(&mp, 0, sizeof(mp));
    mp.base_options.model_asset_buffer = (const char *)model;
    mp.base_options.model_asset_buffer_count = (unsigned int)model_size;
    mp.base_options.delegate = ngh__mp_delegate(opt.accel);
    mp.running_mode = NGH__MP_MODE_VIDEO;
    mp.num_faces = opt.max_faces;
    mp.min_face_detection_confidence = opt.min_detection_confidence;
    mp.min_face_presence_confidence = opt.min_presence_confidence;
    mp.min_tracking_confidence = opt.min_tracking_confidence;
    mp.output_face_blendshapes = (unsigned char)f->want_blendshapes;
    mp.output_facial_transformation_matrixes = (unsigned char)f->want_transform;

    st = ngh__rt.face_create(&mp, &f->lm, &mp_err);
    if (st != NGH__MP_STATUS_OK || !f->lm) {
        f->state = NGH_FAILED;
        ngh__set_err(f->error, sizeof(f->error), "MpFaceLandmarkerCreate failed",
                     mp_err);
        return f; /* keep the handle so the caller can read the message */
    }
    f->state = NGH_READY;

#ifndef NGH_NO_THREADS
    if (opt.threaded) {
        f->pub_lm = (float *)calloc(
            (size_t)opt.max_faces * NGH_FACE_LANDMARKS * 3, sizeof(float));
        if (f->want_blendshapes)
            f->pub_bs = (float *)calloc(
                (size_t)opt.max_faces * NGH_FACE_BLENDSHAPES, sizeof(float));
        if (f->want_transform)
            f->pub_tf = (float *)calloc(
                (size_t)opt.max_faces * NGH_FACE_MATRIX, sizeof(float));
        if (!f->pub_lm || (f->want_blendshapes && !f->pub_bs) ||
            (f->want_transform && !f->pub_tf)) {
            ngh_face_destroy(f);
            return NULL;
        }
        ngh__mutex_init(&f->wk.mu);
        ngh__cond_init(&f->wk.cv);
#if defined(_WIN32)
        f->wk.thread = CreateThread(NULL, 0, ngh__face_thread, f, 0, NULL);
        f->wk.started = f->wk.thread != NULL;
#else
        f->wk.started =
            pthread_create(&f->wk.thread, NULL, ngh__face_thread, f) == 0;
#endif
        if (!f->wk.started) {
            ngh__cond_destroy(&f->wk.cv);
            ngh__mutex_destroy(&f->wk.mu);
        }
        f->threaded = f->wk.started;
    }
#endif
    return f;
}

ngh_state ngh_face_state(const ngh_face *f) {
    return f ? f->state : NGH_FAILED;
}

const char *ngh_face_error(const ngh_face *f) {
    if (!f) return ngh_runtime_error();
    return f->error[0] ? f->error : NULL;
}

ngh_accel ngh_face_accel(const ngh_face *f) {
    return f ? f->accel : NGH_ACCEL_CPU;
}

int ngh_face_detect(ngh_face *f, const ngh_image *image, uint64_t ts_ms,
                    ngh_face_result *out) {
    const uint8_t *px;
    size_t size;
    int fmt, rc;

    if (!f || !out || !out->landmarks) return NGH_ERR_INVALID_ARG;
    if (f->state != NGH_READY) return NGH_ERR_NOT_READY;
    if (!ngh__image_valid(image) || image->kind != NGH_IMAGE_PIXELS)
        return NGH_ERR_INVALID_ARG;
    if (out->capacity < f->max_faces) return NGH_ERR_CAPACITY;
    if (f->have_ts && ts_ms <= f->last_ts) return NGH_ERR_TIMESTAMP;
    f->last_ts = ts_ms;
    f->have_ts = 1;

    rc = ngh__pack_rows(image, &f->pack, &f->pack_cap, &px, &size);
    if (rc != NGH_OK) return rc;
    fmt = (image->format == NGH_PIXFMT_RGBA8) ? NGH__MP_FMT_SRGBA
                                              : NGH__MP_FMT_SRGB;

#ifndef NGH_NO_THREADS
    if (f->threaded) {
        int have;
        ngh__lock(&f->wk.mu);
        rc = ngh__worker_submit(&f->wk, px, size, image->width, image->height,
                                fmt, image->rotation_degrees, ts_ms);
        have = f->pub_valid;
        if (have) {
            int n = f->pub_count;
            memcpy(out->landmarks, f->pub_lm,
                   (size_t)n * NGH_FACE_LANDMARKS * 3 * sizeof(float));
            if (out->blendshapes && f->pub_bs)
                memcpy(out->blendshapes, f->pub_bs,
                       (size_t)n * NGH_FACE_BLENDSHAPES * sizeof(float));
            if (out->transform && f->pub_tf)
                memcpy(out->transform, f->pub_tf,
                       (size_t)n * NGH_FACE_MATRIX * sizeof(float));
            out->count = n;
            out->timestamp_ms = f->pub_ts;
        } else {
            out->count = 0;
            out->timestamp_ms = 0;
        }
        ngh__unlock(&f->wk.mu);
        if (rc != NGH_OK) return rc;
        if (have && f->pub_err != NGH_OK) return f->pub_err;
        return out->count;
    }
#endif

    ngh__face_infer(f, px, size, image->width, image->height, fmt,
                    image->rotation_degrees, ts_ms);
    if (f->infer_err != NGH_OK) return f->infer_err;
    memcpy(out->landmarks, f->lm_buf,
           (size_t)f->count * NGH_FACE_LANDMARKS * 3 * sizeof(float));
    if (out->blendshapes && f->bs_buf)
        memcpy(out->blendshapes, f->bs_buf,
               (size_t)f->count * NGH_FACE_BLENDSHAPES * sizeof(float));
    if (out->transform && f->tf_buf)
        memcpy(out->transform, f->tf_buf,
               (size_t)f->count * NGH_FACE_MATRIX * sizeof(float));
    out->count = f->count;
    out->timestamp_ms = ts_ms;
    return f->count;
}

void ngh_face_destroy(ngh_face *f) {
    if (!f) return;
#ifndef NGH_NO_THREADS
    if (f->threaded) ngh__worker_destroy(&f->wk);
    free(f->pub_lm);
    free(f->pub_bs);
    free(f->pub_tf);
#endif
    if (f->lm && ngh__rt.face_close) {
        char *mp_err = NULL;
        ngh__rt.face_close(f->lm, &mp_err);
        if (mp_err && ngh__rt.error_free) ngh__rt.error_free(mp_err);
    }
    free(f->lm_buf);
    free(f->bs_buf);
    free(f->tf_buf);
    free(f->pack);
    free(f);
}

/* ------------------------------------------------------------------ pose -- */

struct ngh_pose {
    void *lm;
    ngh_state state;
    ngh_accel accel;
    char error[512];

    int max_poses;
    int want_world;

    uint8_t *pack;
    size_t pack_cap;

    float *lm_buf;
    float *world_buf;
    int count;
    uint64_t result_ts;
    int infer_err;

    uint64_t last_ts;
    int have_ts;

#ifndef NGH_NO_THREADS
    int threaded;
    ngh__worker wk;
    float *pub_lm;
    float *pub_world;
    int pub_count;
    uint64_t pub_ts;
    int pub_valid;
    int pub_err;
#endif
};

static void ngh__pose_copy_landmarks(float *dst, const ngh__mp_landmarks *src) {
    uint32_t cnt = src->landmarks_count;
    uint32_t j;
    if (cnt > NGH_POSE_LANDMARKS) cnt = NGH_POSE_LANDMARKS;
    for (j = 0; j < cnt; ++j) {
        dst[j * 4 + 0] = src->landmarks[j].x;
        dst[j * 4 + 1] = src->landmarks[j].y;
        dst[j * 4 + 2] = src->landmarks[j].z;
        dst[j * 4 + 3] = src->landmarks[j].has_visibility
                             ? src->landmarks[j].visibility
                             : 1.0f;
    }
}

static void ngh__pose_read_result(ngh_pose *p, const ngh__mp_pose_result *res) {
    uint32_t n = res->pose_landmarks_count;
    uint32_t i;
    if ((int)n > p->max_poses) n = (uint32_t)p->max_poses;
    for (i = 0; i < n; ++i) {
        ngh__pose_copy_landmarks(p->lm_buf + (size_t)i * NGH_POSE_LANDMARKS * 4,
                                 &res->pose_landmarks[i]);
    }
    if (p->world_buf && res->pose_world_landmarks) {
        uint32_t wn = res->pose_world_landmarks_count;
        if (wn > n) wn = n;
        for (i = 0; i < wn; ++i) {
            ngh__pose_copy_landmarks(
                p->world_buf + (size_t)i * NGH_POSE_LANDMARKS * 4,
                &res->pose_world_landmarks[i]);
        }
    }
    p->count = (int)n;
}

static void ngh__pose_infer(ngh_pose *p, const uint8_t *px, size_t size, int w,
                            int h, int fmt, int rot, uint64_t ts) {
    ngh__mp_pose_result res;
    ngh__mp_ipo ipo;
    void *img = NULL;
    char *mp_err = NULL;
    int st;

    p->infer_err = NGH_OK;
    p->count = 0;
    p->result_ts = ts;

    if (ngh__make_image(px, size, w, h, fmt, &img, p->error, sizeof(p->error)) !=
        NGH_OK) {
        p->infer_err = NGH_ERR_BACKEND;
        return;
    }

    memset(&ipo, 0, sizeof(ipo));
    ipo.rotation_degrees = rot;
    memset(&res, 0, sizeof(res));

    st = ngh__rt.pose_video(p->lm, img, &ipo, (int64_t)ts, &res, &mp_err);
    if (st != NGH__MP_STATUS_OK) {
        ngh__set_err(p->error, sizeof(p->error), "pose detect failed", mp_err);
        p->infer_err = NGH_ERR_BACKEND;
    } else {
        ngh__pose_read_result(p, &res);
        ngh__rt.pose_close_result(&res);
    }
    ngh__rt.image_free(img);
}

#ifndef NGH_NO_THREADS
#if defined(_WIN32)
static DWORD WINAPI ngh__pose_thread(LPVOID arg)
#else
static void *ngh__pose_thread(void *arg)
#endif
{
    ngh_pose *p = (ngh_pose *)arg;
    for (;;) {
        size_t size;
        int w, h, fmt, rot;
        uint64_t ts;
        ngh__lock(&p->wk.mu);
        if (!ngh__worker_take(&p->wk, &size, &w, &h, &fmt, &rot, &ts)) {
            ngh__unlock(&p->wk.mu);
            break;
        }
        ngh__unlock(&p->wk.mu);

        ngh__pose_infer(p, p->wk.scratch, size, w, h, fmt, rot, ts);

        ngh__lock(&p->wk.mu);
        memcpy(p->pub_lm, p->lm_buf,
               (size_t)p->max_poses * NGH_POSE_LANDMARKS * 4 * sizeof(float));
        if (p->pub_world)
            memcpy(
                p->pub_world, p->world_buf,
                (size_t)p->max_poses * NGH_POSE_LANDMARKS * 4 * sizeof(float));
        p->pub_count = p->count;
        p->pub_ts = p->result_ts;
        p->pub_err = p->infer_err;
        p->pub_valid = 1;
        ngh__unlock(&p->wk.mu);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}
#endif /* NGH_NO_THREADS */

ngh_pose *ngh_pose_create(const void *model, size_t model_size,
                          const ngh_pose_options *options) {
    ngh_pose_options opt = options ? *options : ngh_pose_options_default();
    ngh__mp_pose_options mp;
    ngh_pose *p;
    char *mp_err = NULL;
    int st;

    if (!model || model_size == 0 || opt.max_poses <= 0) return NULL;
    if (ngh_runtime_load(NULL) != NGH_OK) return NULL;

    p = (ngh_pose *)calloc(1, sizeof(ngh_pose));
    if (!p) return NULL;
    p->max_poses = opt.max_poses;
    p->want_world = opt.world_landmarks ? 1 : 0;
    p->accel = (opt.accel == NGH_ACCEL_GPU) ? NGH_ACCEL_GPU : NGH_ACCEL_CPU;

    p->lm_buf = (float *)calloc((size_t)opt.max_poses * NGH_POSE_LANDMARKS * 4,
                                sizeof(float));
    if (p->want_world)
        p->world_buf = (float *)calloc(
            (size_t)opt.max_poses * NGH_POSE_LANDMARKS * 4, sizeof(float));
    if (!p->lm_buf || (p->want_world && !p->world_buf)) {
        ngh_pose_destroy(p);
        return NULL;
    }

    memset(&mp, 0, sizeof(mp));
    mp.base_options.model_asset_buffer = (const char *)model;
    mp.base_options.model_asset_buffer_count = (unsigned int)model_size;
    mp.base_options.delegate = ngh__mp_delegate(opt.accel);
    mp.running_mode = NGH__MP_MODE_VIDEO;
    mp.num_poses = opt.max_poses;
    mp.min_pose_detection_confidence = opt.min_detection_confidence;
    mp.min_pose_presence_confidence = opt.min_presence_confidence;
    mp.min_tracking_confidence = opt.min_tracking_confidence;
    mp.output_segmentation_masks = 0; /* large GPU readback for no gain here */

    st = ngh__rt.pose_create(&mp, &p->lm, &mp_err);
    if (st != NGH__MP_STATUS_OK || !p->lm) {
        p->state = NGH_FAILED;
        ngh__set_err(p->error, sizeof(p->error), "MpPoseLandmarkerCreate failed",
                     mp_err);
        return p;
    }
    p->state = NGH_READY;

#ifndef NGH_NO_THREADS
    if (opt.threaded) {
        p->pub_lm = (float *)calloc(
            (size_t)opt.max_poses * NGH_POSE_LANDMARKS * 4, sizeof(float));
        if (p->want_world)
            p->pub_world = (float *)calloc(
                (size_t)opt.max_poses * NGH_POSE_LANDMARKS * 4, sizeof(float));
        if (!p->pub_lm || (p->want_world && !p->pub_world)) {
            ngh_pose_destroy(p);
            return NULL;
        }
        ngh__mutex_init(&p->wk.mu);
        ngh__cond_init(&p->wk.cv);
#if defined(_WIN32)
        p->wk.thread = CreateThread(NULL, 0, ngh__pose_thread, p, 0, NULL);
        p->wk.started = p->wk.thread != NULL;
#else
        p->wk.started =
            pthread_create(&p->wk.thread, NULL, ngh__pose_thread, p) == 0;
#endif
        if (!p->wk.started) {
            ngh__cond_destroy(&p->wk.cv);
            ngh__mutex_destroy(&p->wk.mu);
        }
        p->threaded = p->wk.started;
    }
#endif
    return p;
}

ngh_state ngh_pose_state(const ngh_pose *p) {
    return p ? p->state : NGH_FAILED;
}

const char *ngh_pose_error(const ngh_pose *p) {
    if (!p) return ngh_runtime_error();
    return p->error[0] ? p->error : NULL;
}

ngh_accel ngh_pose_accel(const ngh_pose *p) {
    return p ? p->accel : NGH_ACCEL_CPU;
}

int ngh_pose_detect(ngh_pose *p, const ngh_image *image, uint64_t ts_ms,
                    ngh_pose_result *out) {
    const uint8_t *px;
    size_t size;
    int fmt, rc;

    if (!p || !out || !out->landmarks) return NGH_ERR_INVALID_ARG;
    if (p->state != NGH_READY) return NGH_ERR_NOT_READY;
    if (!ngh__image_valid(image) || image->kind != NGH_IMAGE_PIXELS)
        return NGH_ERR_INVALID_ARG;
    if (out->capacity < p->max_poses) return NGH_ERR_CAPACITY;
    if (p->have_ts && ts_ms <= p->last_ts) return NGH_ERR_TIMESTAMP;
    p->last_ts = ts_ms;
    p->have_ts = 1;

    rc = ngh__pack_rows(image, &p->pack, &p->pack_cap, &px, &size);
    if (rc != NGH_OK) return rc;
    fmt = (image->format == NGH_PIXFMT_RGBA8) ? NGH__MP_FMT_SRGBA
                                              : NGH__MP_FMT_SRGB;

#ifndef NGH_NO_THREADS
    if (p->threaded) {
        int have;
        ngh__lock(&p->wk.mu);
        rc = ngh__worker_submit(&p->wk, px, size, image->width, image->height,
                                fmt, image->rotation_degrees, ts_ms);
        have = p->pub_valid;
        if (have) {
            int n = p->pub_count;
            memcpy(out->landmarks, p->pub_lm,
                   (size_t)n * NGH_POSE_LANDMARKS * 4 * sizeof(float));
            if (out->world_landmarks && p->pub_world)
                memcpy(out->world_landmarks, p->pub_world,
                       (size_t)n * NGH_POSE_LANDMARKS * 4 * sizeof(float));
            out->count = n;
            out->timestamp_ms = p->pub_ts;
        } else {
            out->count = 0;
            out->timestamp_ms = 0;
        }
        ngh__unlock(&p->wk.mu);
        if (rc != NGH_OK) return rc;
        if (have && p->pub_err != NGH_OK) return p->pub_err;
        return out->count;
    }
#endif

    ngh__pose_infer(p, px, size, image->width, image->height, fmt,
                    image->rotation_degrees, ts_ms);
    if (p->infer_err != NGH_OK) return p->infer_err;
    memcpy(out->landmarks, p->lm_buf,
           (size_t)p->count * NGH_POSE_LANDMARKS * 4 * sizeof(float));
    if (out->world_landmarks && p->world_buf)
        memcpy(out->world_landmarks, p->world_buf,
               (size_t)p->count * NGH_POSE_LANDMARKS * 4 * sizeof(float));
    out->count = p->count;
    out->timestamp_ms = ts_ms;
    return p->count;
}

void ngh_pose_destroy(ngh_pose *p) {
    if (!p) return;
#ifndef NGH_NO_THREADS
    if (p->threaded) ngh__worker_destroy(&p->wk);
    free(p->pub_lm);
    free(p->pub_world);
#endif
    if (p->lm && ngh__rt.pose_close) {
        char *mp_err = NULL;
        ngh__rt.pose_close(p->lm, &mp_err);
        if (mp_err && ngh__rt.error_free) ngh__rt.error_free(mp_err);
    }
    free(p->lm_buf);
    free(p->world_buf);
    free(p->pack);
    free(p);
}

#else /* __EMSCRIPTEN__ */

/* ==========================================================================
 *  Web backend -- @mediapipe/tasks-vision through EM_JS
 * ==========================================================================
 *
 * No ASYNCIFY. MediaPipe's detect() and detectForVideo() are synchronous on the
 * web; only createFromOptions() and FilesetResolver.forVisionTasks() return
 * promises. Making creation a poll instead of a blocking call keeps ASYNCIFY --
 * a link flag a header cannot impose, costing roughly 50% in size and speed --
 * out of the caller's build entirely.
 *
 * Every EM_JS body is emitted as its own function, so they cannot share
 * top-level state. Shared state lives on globalThis.__NGH and tasks are
 * referenced by integer handle.
 */

#include <emscripten.h>

/* stringToUTF8 rather than stringToNewUTF8: the latter mallocs, which drags
 * malloc into the link even for callers that never allocate. */
EM_JS_DEPS(ngh, "$UTF8ToString,$stringToUTF8")

#ifndef NGH_WEB_BUNDLE_URL
#define NGH_WEB_BUNDLE_URL                                     \
    "https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@0.10.35/" \
    "vision_bundle.mjs"
#endif
#ifndef NGH_WEB_WASM_BASE
#define NGH_WEB_WASM_BASE \
    "https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@0.10.35/wasm"
#endif

enum { NGH__WEB_FACE = 0, NGH__WEB_POSE = 1 };

/* Installs the shared registry and helpers, and starts loading the bundle.
 * Idempotent: later calls observe S.boot and return. */
EM_JS(void, ngh__web_boot, (const char *bundle_url, const char *wasm_base), {
    if (globalThis.__NGH && globalThis.__NGH.boot) return;
    const S = (globalThis.__NGH = globalThis.__NGH || {});
    S.tasks = [];
    S.sources = [];
    S.mod = null;
    S.fileset = null;
    S.bootError = null;

    /* Wrap wasm memory as an ImageData without copying where the browser lets
     * us. Under -pthread the heap is a SharedArrayBuffer, which ImageData
     * rejects, so fall back to one copy. */
    S.makeImage = function(kind, ptr, w, h, fmt, src) {
        if (kind === 1) return S.sources[src];
        const px = w * h;
        if (fmt === 2) {
            const buf = HEAPU8.buffer;
            const shared = typeof SharedArrayBuffer !== 'undefined' &&
                buf instanceof SharedArrayBuffer;
            const view = shared
                ? new Uint8ClampedArray(HEAPU8.subarray(ptr, ptr + px * 4))
                : new Uint8ClampedArray(buf, ptr, px * 4);
            return new ImageData(view, w, h);
        }
        const out = new Uint8ClampedArray(px * 4);
        const heap = HEAPU8;
        for (let i = 0, s = ptr, d = 0; i < px; ++i) {
            out[d++] = heap[s++];
            out[d++] = heap[s++];
            out[d++] = heap[s++];
            out[d++] = 255;
        }
        return new ImageData(out, w, h);
    };

    S.boot = (async function() {
        try {
            S.mod = await import(UTF8ToString(bundle_url));
            S.fileset =
                await S.mod.FilesetResolver.forVisionTasks(UTF8ToString(wasm_base));
        } catch (e) {
            S.bootError = String(e);
        }
    })();
})

/* Returns a handle immediately; the task itself finishes creating later. */
EM_JS(int, ngh__web_create,
      (int kind, const uint8_t *model, int model_len, int accel, int max_n,
       double c_detect, double c_present, double c_track, int flag_a,
       int flag_b), {
    const S = globalThis.__NGH;
    const handle = S.tasks.length;
    const entry = {task : null, state : 0, error : null, accel : 1};
    S.tasks.push(entry);

    /* MediaPipe keeps its own copy, but the wasm heap can move under us before
     * the async create runs, so snapshot the model here. */
    const bytes = new Uint8Array(HEAPU8.subarray(model, model + model_len));

    S.boot.then(async function() {
        if (S.bootError) {
            entry.state = 2;
            entry.error = S.bootError;
            return;
        }
        const base = {
            baseOptions : {modelAssetBuffer : bytes, delegate : "GPU"},
            runningMode : "VIDEO",
        };
        if (kind === 0) {
            base.numFaces = max_n;
            base.minFaceDetectionConfidence = c_detect;
            base.minFacePresenceConfidence = c_present;
            base.minTrackingConfidence = c_track;
            base.outputFaceBlendshapes = !!flag_a;
            base.outputFacialTransformationMatrixes = !!flag_b;
        } else {
            base.numPoses = max_n;
            base.minPoseDetectionConfidence = c_detect;
            base.minPosePresenceConfidence = c_present;
            base.minTrackingConfidence = c_track;
            base.outputSegmentationMasks = false;
        }
        const make = kind === 0 ? S.mod.FaceLandmarker : S.mod.PoseLandmarker;
        const wanted = accel === 1 ? "CPU" : "GPU";
        base.baseOptions.delegate = wanted;
        try {
            entry.task = await make.createFromOptions(S.fileset, base);
            entry.accel = wanted === "GPU" ? 2 : 1;
        } catch (e) {
            if (wanted === "CPU") {
                entry.state = 2;
                entry.error = String(e);
                return;
            }
            /* GPU creation fails on drivers without the render targets
             * MediaPipe needs. CPU still works there. */
            base.baseOptions.delegate = "CPU";
            try {
                entry.task = await make.createFromOptions(S.fileset, base);
                entry.accel = 1;
            } catch (e2) {
                entry.state = 2;
                entry.error = String(e) + " (CPU retry: " + String(e2) + ")";
                return;
            }
        }
        entry.state = 1;
    });
    return handle;
})

EM_JS(int, ngh__web_state, (int handle), {
    const e = globalThis.__NGH.tasks[handle];
    return e ? e.state : 2;
})

EM_JS(int, ngh__web_accel, (int handle), {
    const e = globalThis.__NGH.tasks[handle];
    return e ? e.accel : 1;
})

/* Copies the message into a caller-owned buffer; returns 1 when there was one. */
EM_JS(int, ngh__web_error, (int handle, char *buf, int cap), {
    const e = globalThis.__NGH.tasks[handle];
    if (!e || !e.error) return 0;
    stringToUTF8(e.error, buf, cap);
    return 1;
})

EM_JS(void, ngh__web_destroy, (int handle), {
    const S = globalThis.__NGH;
    const e = S.tasks[handle];
    if (!e) return;
    if (e.task) e.task.close();
    S.tasks[handle] = null;
})

/* Writes results straight into caller-owned wasm memory. MediaPipe hands back
 * arrays of {x, y, z} objects, so packing into a typed array first would just
 * walk them twice. */
EM_JS(int, ngh__web_detect,
      (int handle, int kind, int img_kind, const uint8_t *ptr, int w, int h,
       int fmt, int src, int rot, double ts, float *out_a, float *out_b,
       float *out_c, int cap), {
    const S = globalThis.__NGH;
    const e = S.tasks[handle];
    if (!e || e.state !== 1) return -2; /* NGH_ERR_NOT_READY */

    let image;
    try {
        image = S.makeImage(img_kind, ptr, w, h, fmt, src);
    } catch (err) {
        e.error = String(err);
        return -1; /* NGH_ERR_INVALID_ARG */
    }
    if (!image) return -1;

    let res;
    try {
        res = rot ? e.task.detectForVideo(image, ts, {rotationDegrees : rot})
                  : e.task.detectForVideo(image, ts);
    } catch (err) {
        const msg = String(err);
        e.error = msg;
        /* A lost WebGL context cannot be recovered in place. */
        if (msg.indexOf("context") >= 0 && msg.indexOf("lost") >= 0) {
            e.state = 3;
            return -4;
        }
        return -4; /* NGH_ERR_BACKEND */
    }

    const H = HEAPF32;
    let n;
    if (kind === 0) {
        const faces = res.faceLandmarks || [];
        n = Math.min(faces.length, cap);
        for (let f = 0; f < n; ++f) {
            const lms = faces[f];
            let o = (out_a >> 2) + f * 478 * 3;
            for (let i = 0; i < lms.length && i < 478; ++i) {
                const l = lms[i];
                H[o++] = l.x;
                H[o++] = l.y;
                H[o++] = l.z;
            }
        }
        if (out_b && res.faceBlendshapes) {
            for (let f = 0; f < n && f < res.faceBlendshapes.length; ++f) {
                const cats = res.faceBlendshapes[f].categories || [];
                let o = (out_b >> 2) + f * 52;
                for (let i = 0; i < cats.length && i < 52; ++i)
                    H[o++] = cats[i].score;
            }
        }
        if (out_c && res.facialTransformationMatrixes) {
            const mats = res.facialTransformationMatrixes;
            for (let f = 0; f < n && f < mats.length; ++f) {
                const d = mats[f].data || [];
                let o = (out_c >> 2) + f * 16;
                for (let i = 0; i < d.length && i < 16; ++i) H[o++] = d[i];
            }
        }
    } else {
        const poses = res.landmarks || [];
        n = Math.min(poses.length, cap);
        for (let p = 0; p < n; ++p) {
            const lms = poses[p];
            let o = (out_a >> 2) + p * 33 * 4;
            for (let i = 0; i < lms.length && i < 33; ++i) {
                const l = lms[i];
                H[o++] = l.x;
                H[o++] = l.y;
                H[o++] = l.z;
                H[o++] = l.visibility === undefined ? 1.0 : l.visibility;
            }
        }
        if (out_b && res.worldLandmarks) {
            const world = res.worldLandmarks;
            for (let p = 0; p < n && p < world.length; ++p) {
                const lms = world[p];
                let o = (out_b >> 2) + p * 33 * 4;
                for (let i = 0; i < lms.length && i < 33; ++i) {
                    const l = lms[i];
                    H[o++] = l.x;
                    H[o++] = l.y;
                    H[o++] = l.z;
                    H[o++] = l.visibility === undefined ? 1.0 : l.visibility;
                }
            }
        }
    }
    return n;
})

/* There is no document in a worker or under node, so this has to be a lookup
 * failure rather than a ReferenceError. */
EM_JS(int, ngh__web_source_create, (const char *selector), {
    if (typeof document === 'undefined') return -1;
    const S = globalThis.__NGH;
    const el = document.querySelector(UTF8ToString(selector));
    if (!el) return -1;
    S.sources.push(el);
    return S.sources.length - 1;
})

EM_JS(void, ngh__web_source_destroy, (int source), {
    const S = globalThis.__NGH;
    if (source >= 0 && source < S.sources.length) S.sources[source] = null;
})

/* ------------------------------------------------------------- C surface -- */

static char ngh__web_bundle[512] = NGH_WEB_BUNDLE_URL;
static char ngh__web_wasm[512] = NGH_WEB_WASM_BASE;
static int ngh__web_booted = 0;
static char ngh__web_runtime_error[512];

void ngh_web_set_bundle_url(const char *url) {
    if (url && *url) snprintf(ngh__web_bundle, sizeof(ngh__web_bundle), "%s", url);
}

void ngh_web_set_wasm_base(const char *base) {
    if (base && *base) snprintf(ngh__web_wasm, sizeof(ngh__web_wasm), "%s", base);
}

static void ngh__web_ensure_boot(void) {
    if (ngh__web_booted) return;
    ngh__web_boot(ngh__web_bundle, ngh__web_wasm);
    ngh__web_booted = 1;
}

int ngh_runtime_load(const char *path) {
    (void)path; /* the web runtime is fetched, not opened from disk */
    ngh__web_ensure_boot();
    return NGH_OK;
}

int ngh_runtime_loaded(void) { return ngh__web_booted; }
const char *ngh_runtime_path(void) { return ngh__web_bundle; }

const char *ngh_runtime_error(void) {
    return ngh__web_runtime_error[0] ? ngh__web_runtime_error : NULL;
}

void ngh_runtime_unload(void) {}

int ngh_web_source_from_selector(const char *css_selector) {
    int src;
    if (!css_selector || !*css_selector) return NGH_ERR_INVALID_ARG;
    ngh__web_ensure_boot();
    src = ngh__web_source_create(css_selector);
    return src < 0 ? NGH_ERR_INVALID_ARG : src;
}

void ngh_web_source_release(int source) {
    if (source >= 0) ngh__web_source_destroy(source);
}

/* Both task types are the same object on the web; the kind field selects the
 * MediaPipe class and the result layout. */
typedef struct ngh__web_task {
    int handle;
    int kind;
    int max_n;
    char error[512];
} ngh__web_task;

struct ngh_face {
    ngh__web_task t;
    int want_blendshapes;
    int want_transform;
    uint8_t *pack;
    size_t pack_cap;
    uint64_t last_ts;
    int have_ts;
};

struct ngh_pose {
    ngh__web_task t;
    int want_world;
    uint8_t *pack;
    size_t pack_cap;
    uint64_t last_ts;
    int have_ts;
};

static ngh_state ngh__web_task_state(ngh__web_task *t) {
    switch (ngh__web_state(t->handle)) {
        case 0:
            return NGH_PENDING;
        case 1:
            return NGH_READY;
        case 3:
            return NGH_LOST;
        default:
            return NGH_FAILED;
    }
}

static const char *ngh__web_task_error(ngh__web_task *t) {
    if (!ngh__web_error(t->handle, t->error, (int)sizeof(t->error)))
        return t->error[0] ? t->error : NULL;
    return t->error;
}

ngh_face *ngh_face_create(const void *model, size_t model_size,
                          const ngh_face_options *options) {
    ngh_face_options opt = options ? *options : ngh_face_options_default();
    ngh_face *f;
    if (!model || model_size == 0 || opt.max_faces <= 0) return NULL;
    ngh__web_ensure_boot();
    f = (ngh_face *)calloc(1, sizeof(ngh_face));
    if (!f) return NULL;
    f->want_blendshapes = opt.blendshapes ? 1 : 0;
    f->want_transform = opt.transform ? 1 : 0;
    f->t.kind = NGH__WEB_FACE;
    f->t.max_n = opt.max_faces;
    f->t.handle = ngh__web_create(
        NGH__WEB_FACE, (const uint8_t *)model, (int)model_size, (int)opt.accel,
        opt.max_faces, opt.min_detection_confidence, opt.min_presence_confidence,
        opt.min_tracking_confidence, f->want_blendshapes, f->want_transform);
    return f;
}

ngh_state ngh_face_state(const ngh_face *f) {
    return f ? ngh__web_task_state((ngh__web_task *)&f->t) : NGH_FAILED;
}

const char *ngh_face_error(const ngh_face *f) {
    return f ? ngh__web_task_error((ngh__web_task *)&f->t) : NULL;
}

ngh_accel ngh_face_accel(const ngh_face *f) {
    return f && ngh__web_accel(f->t.handle) == 2 ? NGH_ACCEL_GPU
                                                 : NGH_ACCEL_CPU;
}

int ngh_face_detect(ngh_face *f, const ngh_image *image, uint64_t ts_ms,
                    ngh_face_result *out) {
    const uint8_t *px = NULL;
    size_t size = 0;
    int n, rc;

    if (!f || !out || !out->landmarks) return NGH_ERR_INVALID_ARG;
    if (!ngh__image_valid(image)) return NGH_ERR_INVALID_ARG;
    if (out->capacity < f->t.max_n) return NGH_ERR_CAPACITY;
    if (ngh_face_state(f) != NGH_READY) return NGH_ERR_NOT_READY;
    if (f->have_ts && ts_ms <= f->last_ts) return NGH_ERR_TIMESTAMP;
    f->last_ts = ts_ms;
    f->have_ts = 1;

    if (image->kind == NGH_IMAGE_PIXELS) {
        rc = ngh__pack_rows(image, &f->pack, &f->pack_cap, &px, &size);
        if (rc != NGH_OK) return rc;
    }

    n = ngh__web_detect(f->t.handle, NGH__WEB_FACE, (int)image->kind, px,
                        image->width, image->height, (int)image->format,
                        image->web_source, image->rotation_degrees,
                        (double)ts_ms, out->landmarks, out->blendshapes,
                        out->transform, out->capacity);
    if (n < 0) {
        out->count = 0;
        return n;
    }
    out->count = n;
    out->timestamp_ms = ts_ms;
    return n;
}

void ngh_face_destroy(ngh_face *f) {
    if (!f) return;
    ngh__web_destroy(f->t.handle);
    free(f->pack);
    free(f);
}

ngh_pose *ngh_pose_create(const void *model, size_t model_size,
                          const ngh_pose_options *options) {
    ngh_pose_options opt = options ? *options : ngh_pose_options_default();
    ngh_pose *p;
    if (!model || model_size == 0 || opt.max_poses <= 0) return NULL;
    ngh__web_ensure_boot();
    p = (ngh_pose *)calloc(1, sizeof(ngh_pose));
    if (!p) return NULL;
    p->want_world = opt.world_landmarks ? 1 : 0;
    p->t.kind = NGH__WEB_POSE;
    p->t.max_n = opt.max_poses;
    p->t.handle = ngh__web_create(
        NGH__WEB_POSE, (const uint8_t *)model, (int)model_size, (int)opt.accel,
        opt.max_poses, opt.min_detection_confidence, opt.min_presence_confidence,
        opt.min_tracking_confidence, 0, 0);
    return p;
}

ngh_state ngh_pose_state(const ngh_pose *p) {
    return p ? ngh__web_task_state((ngh__web_task *)&p->t) : NGH_FAILED;
}

const char *ngh_pose_error(const ngh_pose *p) {
    return p ? ngh__web_task_error((ngh__web_task *)&p->t) : NULL;
}

ngh_accel ngh_pose_accel(const ngh_pose *p) {
    return p && ngh__web_accel(p->t.handle) == 2 ? NGH_ACCEL_GPU
                                                 : NGH_ACCEL_CPU;
}

int ngh_pose_detect(ngh_pose *p, const ngh_image *image, uint64_t ts_ms,
                    ngh_pose_result *out) {
    const uint8_t *px = NULL;
    size_t size = 0;
    int n, rc;

    if (!p || !out || !out->landmarks) return NGH_ERR_INVALID_ARG;
    if (!ngh__image_valid(image)) return NGH_ERR_INVALID_ARG;
    if (out->capacity < p->t.max_n) return NGH_ERR_CAPACITY;
    if (ngh_pose_state(p) != NGH_READY) return NGH_ERR_NOT_READY;
    if (p->have_ts && ts_ms <= p->last_ts) return NGH_ERR_TIMESTAMP;
    p->last_ts = ts_ms;
    p->have_ts = 1;

    if (image->kind == NGH_IMAGE_PIXELS) {
        rc = ngh__pack_rows(image, &p->pack, &p->pack_cap, &px, &size);
        if (rc != NGH_OK) return rc;
    }

    n = ngh__web_detect(p->t.handle, NGH__WEB_POSE, (int)image->kind, px,
                        image->width, image->height, (int)image->format,
                        image->web_source, image->rotation_degrees,
                        (double)ts_ms, out->landmarks, out->world_landmarks,
                        NULL, out->capacity);
    if (n < 0) {
        out->count = 0;
        return n;
    }
    out->count = n;
    out->timestamp_ms = ts_ms;
    return n;
}

void ngh_pose_destroy(ngh_pose *p) {
    if (!p) return;
    ngh__web_destroy(p->t.handle);
    free(p->pack);
    free(p);
}

#endif /* __EMSCRIPTEN__ */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NGH_MEDIAPIPE_IMPLEMENTED */
#endif /* NGH_MEDIAPIPE_IMPLEMENTATION */
