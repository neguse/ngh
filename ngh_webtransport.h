/* ngh_webtransport.h -- WebTransport client from C.
 *
 * One header, one API, across Windows, Linux and the web. This comment is
 * the documentation; the header is the deliverable. Copy the file, then in
 * exactly one translation unit:
 *
 *     #define NGH_WEBTRANSPORT_IMPLEMENTATION
 *     #include "ngh_webtransport.h"
 *
 * There is nothing to link. On desktop the implementation resolves a small
 * backend library (ngh_wt_backend, built on picoquic) at run time; on the
 * web (Emscripten) it bridges to the browser's WebTransport API through
 * EM_JS, with no extra emcc flags and no ASYNCIFY.
 *
 * ==========================================================================
 * Quick start
 * ==========================================================================
 *
 *     ngh_wt_options opt = ngh_wt_options_default();
 *     opt.cert_hashes = hash;      // SHA-256 of the server cert, see below
 *     opt.cert_hash_count = 1;
 *     ngh_wt *wt = ngh_wt_connect("https://example.com:4433/play", &opt);
 *
 *     // Per frame.
 *     switch (ngh_wt_state(wt)) {
 *     case NGH_WT_PENDING: break;              // still connecting
 *     case NGH_WT_READY: {
 *         ngh_wt_dgram_send(wt, input, input_len);
 *         uint8_t buf[1500];
 *         int n;
 *         while ((n = ngh_wt_dgram_recv(wt, buf, sizeof buf)) > 0)
 *             apply_state(buf, n);
 *         break;
 *     }
 *     default:                                  // NGH_WT_CLOSED / NGH_WT_ERROR
 *         printf("gone: %s\n", ngh_wt_error(wt));
 *     }
 *
 * Streams mirror the browser API: reliable, ordered, with backpressure.
 *
 *     ngh_wt_stream *s = ngh_wt_stream_open(wt, NGH_WT_BIDI);
 *     ngh_wt_stream_write(s, msg, msg_len);     // returns bytes accepted
 *     ngh_wt_stream_finish(s);                  // FIN after queued bytes
 *     int n = ngh_wt_stream_read(s, buf, cap);  // >0 data, 0 none,
 *                                               // NGH_WT_FIN when done
 *     ngh_wt_stream *in = ngh_wt_stream_accept(wt); // peer-initiated, or NULL
 *
 * EVERYTHING IS POLLED, NOTHING BLOCKS, NO CALLBACKS. connect() returns
 * immediately and the session reports NGH_WT_PENDING until the handshake
 * finishes. The same loop is correct on every platform; ASYNCIFY (a link
 * flag a header cannot impose) is never needed. Poll from one thread at a
 * time; the desktop backend runs its own network thread internally.
 *
 * Two semantics to understand before shipping:
 *
 * DATAGRAMS DROP, STREAMS PUSH BACK. Datagrams are unreliable by contract:
 * the receive queue is bounded and drops the oldest when the application
 * stops polling -- exactly what the browser and every UDP stack underneath
 * already do. Streams never drop: when the application stops reading, QUIC
 * flow control makes the sender wait, and when the send queue is full,
 * ngh_wt_stream_write() accepts 0 bytes -- retry next frame.
 *
 * CERTIFICATES ARE PINNED, LIKE THE BROWSER'S DEV FLOW. cert_hashes is the
 * serverCertificateHashes model: SHA-256 over the server's end-entity
 * certificate in DER form, any match passes. Browsers additionally require
 * such certificates to be ECDSA with a validity of at most 14 days. On the
 * web, leaving cert_hashes NULL uses normal CA validation; on desktop CA
 * validation is not implemented yet, so cert_hashes is required and
 * connect() fails loudly without it.
 *
 * ==========================================================================
 * Getting the desktop backend
 * ==========================================================================
 *
 * The desktop implementation lives in a small shared library
 * (ngh_wt_backend) that statically links picoquic, so this header needs
 * nothing at link time. Prebuilt libraries ship with ngh releases, or build
 * one yourself:
 *
 *     python3 scripts/fetch_picoquic.py
 *     cmake -B build && cmake --build build --target ngh_wt_backend
 *
 * ngh_wt_runtime_load(NULL) searches $NGH_WT_BACKEND_PATH, then the
 * directory of the running executable, then the loader's default path. The
 * first connect() loads lazily; calling ngh_wt_runtime_load() up front just
 * lets you report a missing backend before any other setup.
 *
 * ==========================================================================
 * Sizes, orderings, edge semantics
 * ==========================================================================
 *
 * - ngh_wt_dgram_max() is the largest payload ngh_wt_dgram_send accepts;
 *   0 while not READY. Receiving into a buffer smaller than the datagram
 *   returns NGH_WT_ERR_SIZE and drops that datagram (datagram semantics).
 * - ngh_wt_stream_read() interleaves nothing: bytes arrive in order, FIN
 *   only after all data was consumed. A peer reset surfaces as
 *   NGH_WT_ERR_RESET instead.
 * - Handles stay valid until their *_destroy call, whatever the state.
 *   Destroying the session does not destroy its stream handles; destroy
 *   them independently.
 * - The web backend needs a browser with WebTransport (all evergreen
 *   browsers since 2026); under plain node the session reports
 *   NGH_WT_ERROR.
 */

#ifndef NGH_WEBTRANSPORT_H_INCLUDED
#define NGH_WEBTRANSPORT_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NGH_WT_VERSION_MAJOR 0
#define NGH_WT_VERSION_MINOR 1
#define NGH_WT_VERSION_PATCH 0

/* Session states. */
#define NGH_WT_PENDING 0
#define NGH_WT_READY 1
#define NGH_WT_CLOSED 2
#define NGH_WT_ERROR 3

/* Stream directions for ngh_wt_stream_open. */
#define NGH_WT_UNI 0
#define NGH_WT_BIDI 1

/* Negative returns; positive returns are byte counts, 0 means "nothing
 * right now" unless documented otherwise. */
#define NGH_WT_FIN (-1)
#define NGH_WT_ERR (-2)
#define NGH_WT_ERR_SIZE (-3)
#define NGH_WT_ERR_RESET (-4)

typedef struct ngh_wt ngh_wt;
typedef struct ngh_wt_stream ngh_wt_stream;

typedef struct ngh_wt_options {
    /* cert_hash_count concatenated 32-byte SHA-256 digests of acceptable
     * end-entity certificates (DER). NULL = CA validation (web only for
     * now; see the manual above). */
    const uint8_t *cert_hashes;
    size_t cert_hash_count;
    uint32_t idle_timeout_ms;    /* 0 = default */
    uint32_t connect_timeout_ms; /* 0 = default (10s) */
} ngh_wt_options;

ngh_wt_options ngh_wt_options_default(void);

/* Desktop only; a no-op that returns 0 on the web. Loads the backend from
 * `path`, or from the default search order when NULL. Returns 0 on
 * success; on failure ngh_wt_last_error() explains. */
int ngh_wt_runtime_load(const char *path);

/* Explains the most recent NULL return from ngh_wt_connect or nonzero
 * return from ngh_wt_runtime_load. */
const char *ngh_wt_last_error(void);

ngh_wt *ngh_wt_connect(const char *url, const ngh_wt_options *opt);
int ngh_wt_state(ngh_wt *wt);
/* Non-NULL once the session left PENDING/READY abnormally, or CLOSED with
 * a peer-supplied code/reason. Owned by the session. */
const char *ngh_wt_error(ngh_wt *wt);
void ngh_wt_close(ngh_wt *wt, uint32_t error_code, const char *reason);
void ngh_wt_destroy(ngh_wt *wt);

int ngh_wt_dgram_send(ngh_wt *wt, const void *data, size_t len);
int ngh_wt_dgram_recv(ngh_wt *wt, void *buf, size_t cap);
size_t ngh_wt_dgram_max(ngh_wt *wt);

ngh_wt_stream *ngh_wt_stream_open(ngh_wt *wt, int bidi);
ngh_wt_stream *ngh_wt_stream_accept(ngh_wt *wt);
int ngh_wt_stream_bidi(ngh_wt_stream *s);
int ngh_wt_stream_write(ngh_wt_stream *s, const void *data, size_t len);
int ngh_wt_stream_finish(ngh_wt_stream *s);
int ngh_wt_stream_read(ngh_wt_stream *s, void *buf, size_t cap);
void ngh_wt_stream_reset(ngh_wt_stream *s, uint32_t error_code);
void ngh_wt_stream_destroy(ngh_wt_stream *s);

#ifdef __cplusplus
}
#endif

/* ==========================================================================
 * Implementation
 * ========================================================================== */
#ifdef NGH_WEBTRANSPORT_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char ngh_wt__last_error[512];

const char *ngh_wt_last_error(void) {
    return ngh_wt__last_error[0] ? ngh_wt__last_error : NULL;
}

ngh_wt_options ngh_wt_options_default(void) {
    ngh_wt_options o;
    memset(&o, 0, sizeof(o));
    return o;
}

#if !defined(__EMSCRIPTEN__)

/* ==========================================================================
 *  Desktop backend -- the ngh_wt_backend shared library, loaded at run time
 * ==========================================================================
 *
 * The declarations below mirror backend/wt_backend.h (ABI version 1); the
 * backend hides picoquic entirely, so this header only moves bytes and
 * polls states through a dozen C calls.
 */

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif
#endif

#define NGH_WT__ABI_VERSION 1u

typedef struct ngh_wt__abi_options {
    size_t struct_size;
    const uint8_t *cert_hashes;
    size_t cert_hash_count;
    uint32_t idle_timeout_ms;
    uint32_t connect_timeout_ms;
} ngh_wt__abi_options;

typedef struct ngh_wt__abi {
    void *handle;
    char path[1024];
    int loaded;
    uint32_t (*abi_version)(void);
    void *(*ctx_create)(void);
    void (*ctx_destroy)(void *);
    const char *(*ctx_error)(void *);
    void *(*connect)(void *, const char *, const ngh_wt__abi_options *);
    int (*session_state)(void *);
    const char *(*session_error)(void *);
    void (*session_close)(void *, uint32_t, const char *);
    void (*session_destroy)(void *);
    int (*dgram_send)(void *, const uint8_t *, size_t);
    int (*dgram_recv)(void *, uint8_t *, size_t);
    size_t (*dgram_max)(void *);
    void *(*stream_open)(void *, int);
    void *(*stream_accept)(void *);
    int (*stream_bidi)(void *);
    int (*stream_write)(void *, const uint8_t *, size_t);
    int (*stream_finish)(void *);
    int (*stream_read)(void *, uint8_t *, size_t);
    void (*stream_reset)(void *, uint32_t);
    void (*stream_destroy)(void *);
} ngh_wt__abi;

static ngh_wt__abi ngh_wt__rt;
static void *ngh_wt__ctx;

#if defined(_WIN32)
#define NGH_WT__LIB_NAME "ngh_wt_backend.dll"
#elif defined(__APPLE__)
#define NGH_WT__LIB_NAME "libngh_wt_backend.dylib"
#else
#define NGH_WT__LIB_NAME "libngh_wt_backend.so"
#endif

static const char *ngh_wt__getenv(const char *name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

static void *ngh_wt__dlopen(const char *utf8_path) {
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

static void *ngh_wt__dlsym(void *handle, const char *name) {
#if defined(_WIN32)
    return (void *)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

static void ngh_wt__dlclose(void *handle) {
#if defined(_WIN32)
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

static int ngh_wt__exe_dir(char *out, size_t cap) {
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

static int ngh_wt__bind(void) {
    ngh_wt__abi *r = &ngh_wt__rt;
    struct {
        const char *name;
        void **slot;
    } table[] = {
        {"ngh_wtb_abi_version", (void **)&r->abi_version},
        {"ngh_wtb_ctx_create", (void **)&r->ctx_create},
        {"ngh_wtb_ctx_destroy", (void **)&r->ctx_destroy},
        {"ngh_wtb_ctx_error", (void **)&r->ctx_error},
        {"ngh_wtb_connect", (void **)&r->connect},
        {"ngh_wtb_session_state", (void **)&r->session_state},
        {"ngh_wtb_session_error", (void **)&r->session_error},
        {"ngh_wtb_session_close", (void **)&r->session_close},
        {"ngh_wtb_session_destroy", (void **)&r->session_destroy},
        {"ngh_wtb_dgram_send", (void **)&r->dgram_send},
        {"ngh_wtb_dgram_recv", (void **)&r->dgram_recv},
        {"ngh_wtb_dgram_max", (void **)&r->dgram_max},
        {"ngh_wtb_stream_open", (void **)&r->stream_open},
        {"ngh_wtb_stream_accept", (void **)&r->stream_accept},
        {"ngh_wtb_stream_bidi", (void **)&r->stream_bidi},
        {"ngh_wtb_stream_write", (void **)&r->stream_write},
        {"ngh_wtb_stream_finish", (void **)&r->stream_finish},
        {"ngh_wtb_stream_read", (void **)&r->stream_read},
        {"ngh_wtb_stream_reset", (void **)&r->stream_reset},
        {"ngh_wtb_stream_destroy", (void **)&r->stream_destroy},
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        *table[i].slot = ngh_wt__dlsym(r->handle, table[i].name);
        if (!*table[i].slot) {
            snprintf(ngh_wt__last_error, sizeof(ngh_wt__last_error),
                     "%.300s does not export %.64s (wrong or too old "
                     "ngh_wt_backend?)",
                     r->path, table[i].name);
            return 0;
        }
    }
    if (r->abi_version() != NGH_WT__ABI_VERSION) {
        snprintf(ngh_wt__last_error, sizeof(ngh_wt__last_error),
                 "%.300s speaks ABI %u, this header needs %u", r->path,
                 (unsigned)r->abi_version(), (unsigned)NGH_WT__ABI_VERSION);
        return 0;
    }
    return 1;
}

static int ngh_wt__try_load(const char *path) {
    ngh_wt__abi *r = &ngh_wt__rt;
    void *handle = ngh_wt__dlopen(path);
    if (!handle) return 0;
    r->handle = handle;
    snprintf(r->path, sizeof(r->path), "%s", path);
    if (!ngh_wt__bind()) {
        ngh_wt__dlclose(handle);
        r->handle = NULL;
        r->path[0] = '\0';
        return 0;
    }
    r->loaded = 1;
    ngh_wt__last_error[0] = '\0';
    return 1;
}

int ngh_wt_runtime_load(const char *path) {
    char buf[1024];
    const char *env;

    if (ngh_wt__rt.loaded) return 0;
    ngh_wt__last_error[0] = '\0';

    if (path && *path) {
        if (ngh_wt__try_load(path)) return 0;
        if (!ngh_wt__last_error[0])
            snprintf(ngh_wt__last_error, sizeof(ngh_wt__last_error),
                     "cannot load %s", path);
        return NGH_WT_ERR;
    }

    env = ngh_wt__getenv("NGH_WT_BACKEND_PATH");
    if (env && *env && ngh_wt__try_load(env)) return 0;

    if (ngh_wt__exe_dir(buf, sizeof(buf))) {
        size_t len = strlen(buf);
        snprintf(buf + len, sizeof(buf) - len, "%s", NGH_WT__LIB_NAME);
        if (ngh_wt__try_load(buf)) return 0;
    }

    if (ngh_wt__try_load(NGH_WT__LIB_NAME)) return 0;

    if (!ngh_wt__last_error[0])
        snprintf(ngh_wt__last_error, sizeof(ngh_wt__last_error),
                 "cannot find " NGH_WT__LIB_NAME
                 " (set NGH_WT_BACKEND_PATH or place it next to the "
                 "executable)");
    return NGH_WT_ERR;
}

static int ngh_wt__ensure(void) {
    if (!ngh_wt__rt.loaded && ngh_wt_runtime_load(NULL) != 0) return 0;
    if (ngh_wt__ctx == NULL) {
        ngh_wt__ctx = ngh_wt__rt.ctx_create();
        if (ngh_wt__ctx == NULL) {
            snprintf(ngh_wt__last_error, sizeof(ngh_wt__last_error),
                     "backend context creation failed");
            return 0;
        }
    }
    return 1;
}

ngh_wt *ngh_wt_connect(const char *url, const ngh_wt_options *opt) {
    ngh_wt__abi_options abi_opt;
    void *s;
    if (!url || !*url) {
        snprintf(ngh_wt__last_error, sizeof(ngh_wt__last_error),
                 "url is required");
        return NULL;
    }
    if (!ngh_wt__ensure()) return NULL;
    memset(&abi_opt, 0, sizeof(abi_opt));
    abi_opt.struct_size = sizeof(abi_opt);
    if (opt) {
        abi_opt.cert_hashes = opt->cert_hashes;
        abi_opt.cert_hash_count = opt->cert_hash_count;
        abi_opt.idle_timeout_ms = opt->idle_timeout_ms;
        abi_opt.connect_timeout_ms = opt->connect_timeout_ms;
    }
    s = ngh_wt__rt.connect(ngh_wt__ctx, url, &abi_opt);
    if (s == NULL) {
        const char *msg = ngh_wt__rt.ctx_error(ngh_wt__ctx);
        snprintf(ngh_wt__last_error, sizeof(ngh_wt__last_error), "%s",
                 msg ? msg : "connect failed");
        return NULL;
    }
    return (ngh_wt *)s;
}

int ngh_wt_state(ngh_wt *wt) {
    if (!wt) return NGH_WT_ERROR;
    return ngh_wt__rt.session_state(wt);
}

const char *ngh_wt_error(ngh_wt *wt) {
    if (!wt) return "invalid handle";
    return ngh_wt__rt.session_error(wt);
}

void ngh_wt_close(ngh_wt *wt, uint32_t error_code, const char *reason) {
    if (wt) ngh_wt__rt.session_close(wt, error_code, reason);
}

void ngh_wt_destroy(ngh_wt *wt) {
    if (wt) ngh_wt__rt.session_destroy(wt);
}

int ngh_wt_dgram_send(ngh_wt *wt, const void *data, size_t len) {
    if (!wt || (!data && len > 0)) return NGH_WT_ERR;
    return ngh_wt__rt.dgram_send(wt, (const uint8_t *)data, len);
}

int ngh_wt_dgram_recv(ngh_wt *wt, void *buf, size_t cap) {
    if (!wt || !buf) return NGH_WT_ERR;
    return ngh_wt__rt.dgram_recv(wt, (uint8_t *)buf, cap);
}

size_t ngh_wt_dgram_max(ngh_wt *wt) {
    if (!wt) return 0;
    return ngh_wt__rt.dgram_max(wt);
}

ngh_wt_stream *ngh_wt_stream_open(ngh_wt *wt, int bidi) {
    if (!wt) return NULL;
    return (ngh_wt_stream *)ngh_wt__rt.stream_open(wt, bidi);
}

ngh_wt_stream *ngh_wt_stream_accept(ngh_wt *wt) {
    if (!wt) return NULL;
    return (ngh_wt_stream *)ngh_wt__rt.stream_accept(wt);
}

int ngh_wt_stream_bidi(ngh_wt_stream *s) {
    if (!s) return 0;
    return ngh_wt__rt.stream_bidi(s);
}

int ngh_wt_stream_write(ngh_wt_stream *s, const void *data, size_t len) {
    if (!s || (!data && len > 0)) return NGH_WT_ERR;
    return ngh_wt__rt.stream_write(s, (const uint8_t *)data, len);
}

int ngh_wt_stream_finish(ngh_wt_stream *s) {
    if (!s) return NGH_WT_ERR;
    return ngh_wt__rt.stream_finish(s);
}

int ngh_wt_stream_read(ngh_wt_stream *s, void *buf, size_t cap) {
    if (!s || !buf) return NGH_WT_ERR;
    return ngh_wt__rt.stream_read(s, (uint8_t *)buf, cap);
}

void ngh_wt_stream_reset(ngh_wt_stream *s, uint32_t error_code) {
    if (s) ngh_wt__rt.stream_reset(s, error_code);
}

void ngh_wt_stream_destroy(ngh_wt_stream *s) {
    if (s) ngh_wt__rt.stream_destroy(s);
}

#else /* __EMSCRIPTEN__ */

/* ==========================================================================
 *  Web backend -- the browser's WebTransport API through EM_JS
 * ==========================================================================
 *
 * Every EM_JS body is emitted as its own function, so they cannot share
 * top-level state. Shared state lives on globalThis.__NGH.wt and sessions
 * and streams are referenced by integer handle. The async pumps translate
 * the promise world into bounded queues the C side polls:
 *
 * - datagrams: pump always reads; the queue drops oldest past 64 entries
 *   (datagram semantics, the browser does the same internally).
 * - streams: the pump pauses above a byte watermark and resumes when the
 *   application reads, so QUIC flow control reaches the sender.
 * - writes: bytes are counted until their write() promise settles; past a
 *   watermark ngh_wt_stream_write accepts 0 bytes (backpressure).
 */

#include <emscripten.h>

EM_JS_DEPS(ngh_wt, "$UTF8ToString,$stringToUTF8")

/* Installs the registry and helpers. Idempotent. */
EM_JS(void, ngh_wt__js_boot, (void), {
    const S = (globalThis.__NGH = globalThis.__NGH || {});
    if (S.wt) return;
    const W = (S.wt = {sessions : [null], streams : [null]});
    W.DGRAM_QUEUE_MAX = 64;
    W.RECV_WATERMARK = 1 << 20;
    W.SEND_WATERMARK = 1 << 20;

    W.fail = function(e, err) {
        if (e.state <= 1) {
            e.state = 3;
            e.error = String(err);
        }
    };

    W.makeStream = function(bidi, canRead, canWrite) {
        const st = {
            bidi : bidi,
            writer : null,
            canRead : canRead,
            canWrite : canWrite,
            pend : [],       /* writes queued before the writer exists */
            pendClose : false,
            sendBytes : 0,
            rq : [],
            rqBytes : 0,
            rqOff : 0,
            fin : false,
            reset : false,
            resume : null,
            dead : false,
        };
        W.streams.push(st);
        st.sid = W.streams.length - 1;
        return st;
    };

    W.attachWriter = function(st, writable) {
        st.writer = writable.getWriter();
        for (const chunk of st.pend) {
            st.writer.write(chunk).then(
                () => { st.sendBytes -= chunk.length; },
                () => {});
        }
        st.pend = [];
        if (st.pendClose) {
            st.writer.close().catch(() => {});
        }
    };

    W.pumpRead = async function(st, readable) {
        try {
            const reader = readable.getReader();
            for (;;) {
                if (st.rqBytes >= W.RECV_WATERMARK) {
                    await new Promise((r) => { st.resume = r; });
                    if (st.dead) break;
                }
                const {done, value} = await reader.read();
                if (done) {
                    st.fin = true;
                    break;
                }
                st.rq.push(value);
                st.rqBytes += value.length;
            }
        } catch (err) {
            st.reset = true;
        }
    };

    W.pumpDgram = async function(e) {
        try {
            const reader = e.t.datagrams.readable.getReader();
            for (;;) {
                const {done, value} = await reader.read();
                if (done) break;
                if (e.dq.length >= W.DGRAM_QUEUE_MAX) e.dq.shift();
                e.dq.push(value);
            }
        } catch (err) { /* session teardown reports elsewhere */
        }
    };

    W.pumpAccept = async function(e, incoming, bidi) {
        try {
            const reader = incoming.getReader();
            for (;;) {
                const {done, value} = await reader.read();
                if (done) break;
                let st;
                if (bidi) {
                    st = W.makeStream(1, true, true);
                    W.attachWriter(st, value.writable);
                    W.pumpRead(st, value.readable);
                } else {
                    st = W.makeStream(0, true, false);
                    W.pumpRead(st, value);
                }
                if (e.acceptq.length >= 64) {
                    st.dead = true;
                    W.streams[st.sid] = null;
                    continue;
                }
                e.acceptq.push(st.sid);
            }
        } catch (err) {
        }
    };
})

EM_JS(int, ngh_wt__js_connect,
      (const char *url, const uint8_t *hashes, int hash_count, int idle_ms,
       int connect_ms), {
    const W = globalThis.__NGH.wt;
    const e = {
        t : null,
        state : 0,
        error : null,
        dmax : 0,
        dq : [],
        dwriter : null,
        acceptq : [],
    };
    W.sessions.push(e);
    const h = W.sessions.length - 1;
    const opts = {};
    if (hash_count > 0) {
        opts.serverCertificateHashes = [];
        for (let i = 0; i < hash_count; ++i) {
            opts.serverCertificateHashes.push({
                algorithm : "sha-256",
                value : new Uint8Array(
                    HEAPU8.subarray(hashes + 32 * i, hashes + 32 * i + 32))
                    .slice().buffer
            });
        }
    }
    try {
        e.t = new WebTransport(UTF8ToString(url), opts);
    } catch (err) {
        W.fail(e, err);
        return h;
    }
    let timer = null;
    if (connect_ms > 0) {
        timer = setTimeout(() => {
            if (e.state === 0) {
                W.fail(e, "connect timeout");
                try { e.t.close(); } catch (err) {}
            }
        }, connect_ms);
    }
    e.t.ready.then(
        () => {
            if (timer) clearTimeout(timer);
            if (e.state !== 0) return;
            e.state = 1;
            e.dmax = e.t.datagrams.maxDatagramSize | 0;
            e.dwriter = e.t.datagrams.writable.getWriter();
            W.pumpDgram(e);
            W.pumpAccept(e, e.t.incomingBidirectionalStreams, true);
            W.pumpAccept(e, e.t.incomingUnidirectionalStreams, false);
        },
        (err) => {
            if (timer) clearTimeout(timer);
            W.fail(e, err);
        });
    e.t.closed.then(
        (info) => {
            if (e.state <= 1) {
                e.state = 2;
                e.error = "closed by peer: code " + info.closeCode +
                    (info.reason ? ", " + info.reason : "");
            }
        },
        (err) => { W.fail(e, err); });
    return h;
})

EM_JS(int, ngh_wt__js_state, (int h), {
    const e = globalThis.__NGH.wt.sessions[h];
    return e ? e.state : 3;
})

EM_JS(int, ngh_wt__js_error, (int h, char *buf, int cap), {
    const e = globalThis.__NGH.wt.sessions[h];
    if (!e || !e.error) return 0;
    stringToUTF8(e.error, buf, cap);
    return 1;
})

EM_JS(void, ngh_wt__js_close, (int h, int code, const char *reason), {
    const e = globalThis.__NGH.wt.sessions[h];
    if (!e || !e.t) return;
    try {
        e.t.close({
            closeCode : code >>> 0,
            reason : reason ? UTF8ToString(reason) : ""
        });
    } catch (err) {}
    if (e.state <= 1) e.state = 2;
})

EM_JS(void, ngh_wt__js_session_destroy, (int h), {
    const W = globalThis.__NGH.wt;
    const e = W.sessions[h];
    if (!e) return;
    if (e.t && e.state <= 1) {
        try { e.t.close(); } catch (err) {}
    }
    W.sessions[h] = null;
})

EM_JS(int, ngh_wt__js_dgram_send, (int h, const uint8_t *p, int len), {
    const e = globalThis.__NGH.wt.sessions[h];
    if (!e || e.state !== 1 || !e.dwriter || len > e.dmax) return -2;
    const bytes = new Uint8Array(HEAPU8.subarray(p, p + len)).slice();
    e.dwriter.write(bytes).catch(() => {});
    return 0;
})

EM_JS(int, ngh_wt__js_dgram_recv, (int h, uint8_t *p, int cap), {
    const e = globalThis.__NGH.wt.sessions[h];
    if (!e) return -2;
    if (e.dq.length === 0) return 0;
    const d = e.dq.shift();
    if (d.length > cap) return -3;
    HEAPU8.set(d, p);
    return d.length;
})

EM_JS(int, ngh_wt__js_dgram_max, (int h), {
    const e = globalThis.__NGH.wt.sessions[h];
    return e && e.state === 1 ? e.dmax : 0;
})

EM_JS(int, ngh_wt__js_stream_open, (int h, int bidi), {
    const W = globalThis.__NGH.wt;
    const e = W.sessions[h];
    if (!e || e.state !== 1) return 0;
    const st = W.makeStream(bidi ? 1 : 0, !!bidi, true);
    if (bidi) {
        e.t.createBidirectionalStream().then(
            (s) => {
                if (st.dead) return;
                W.attachWriter(st, s.writable);
                W.pumpRead(st, s.readable);
            },
            () => { st.reset = true; });
    } else {
        e.t.createUnidirectionalStream().then(
            (s) => {
                if (st.dead) return;
                W.attachWriter(st, s);
            },
            () => { st.reset = true; });
    }
    return st.sid;
})

EM_JS(int, ngh_wt__js_stream_accept, (int h), {
    const e = globalThis.__NGH.wt.sessions[h];
    if (!e || e.acceptq.length === 0) return 0;
    return e.acceptq.shift();
})

EM_JS(int, ngh_wt__js_stream_bidi, (int sid), {
    const st = globalThis.__NGH.wt.streams[sid];
    return st ? st.bidi : 0;
})

EM_JS(int, ngh_wt__js_stream_write, (int sid, const uint8_t *p, int len), {
    const W = globalThis.__NGH.wt;
    const st = W.streams[sid];
    if (!st || !st.canWrite || st.pendClose || st.reset) return -2;
    const room = W.SEND_WATERMARK - st.sendBytes;
    if (room <= 0) return 0;
    const n = len < room ? len : room;
    const bytes = new Uint8Array(HEAPU8.subarray(p, p + n)).slice();
    st.sendBytes += n;
    if (st.writer) {
        st.writer.write(bytes).then(
            () => { st.sendBytes -= n; },
            () => {});
    } else {
        st.pend.push(bytes);
    }
    return n;
})

EM_JS(int, ngh_wt__js_stream_finish, (int sid), {
    const st = globalThis.__NGH.wt.streams[sid];
    if (!st || !st.canWrite || st.pendClose) return -2;
    st.pendClose = true;
    if (st.writer) st.writer.close().catch(() => {});
    return 0;
})

EM_JS(int, ngh_wt__js_stream_read, (int sid, uint8_t *p, int cap), {
    const W = globalThis.__NGH.wt;
    const st = W.streams[sid];
    if (!st) return -2;
    if (st.rq.length === 0) {
        if (st.reset) return -4;
        if (st.fin) return -1;
        return 0;
    }
    let got = 0;
    while (st.rq.length > 0 && got < cap) {
        const chunk = st.rq[0];
        const avail = chunk.length - st.rqOff;
        const take = avail < cap - got ? avail : cap - got;
        HEAPU8.set(chunk.subarray(st.rqOff, st.rqOff + take), p + got);
        got += take;
        st.rqOff += take;
        st.rqBytes -= take;
        if (st.rqOff === chunk.length) {
            st.rq.shift();
            st.rqOff = 0;
        }
    }
    if (st.resume && st.rqBytes < W.RECV_WATERMARK) {
        const r = st.resume;
        st.resume = null;
        r();
    }
    return got;
})

EM_JS(void, ngh_wt__js_stream_reset, (int sid, int code), {
    const st = globalThis.__NGH.wt.streams[sid];
    if (!st) return;
    if (st.writer) st.writer.abort().catch(() => {});
    st.pendClose = true;
})

EM_JS(void, ngh_wt__js_stream_destroy, (int sid), {
    const W = globalThis.__NGH.wt;
    const st = W.streams[sid];
    if (!st) return;
    st.dead = true;
    if (st.resume) {
        const r = st.resume;
        st.resume = null;
        r();
    }
    if (st.writer && !st.pendClose) st.writer.abort().catch(() => {});
    W.streams[sid] = null;
})

/* Handles are ints on the JS side; the public opaque pointers wrap them in
 * tiny heap structs so the API is identical on every platform. */
struct ngh_wt {
    int h;
    char err[512];
};
struct ngh_wt_stream {
    int sid;
};

int ngh_wt_runtime_load(const char *path) {
    (void)path;
    return 0;
}

ngh_wt *ngh_wt_connect(const char *url, const ngh_wt_options *opt) {
    ngh_wt *wt;
    if (!url || !*url) {
        snprintf(ngh_wt__last_error, sizeof(ngh_wt__last_error),
                 "url is required");
        return NULL;
    }
    wt = (ngh_wt *)calloc(1, sizeof(ngh_wt));
    if (!wt) {
        snprintf(ngh_wt__last_error, sizeof(ngh_wt__last_error),
                 "out of memory");
        return NULL;
    }
    ngh_wt__js_boot();
    wt->h = ngh_wt__js_connect(
        url, opt ? opt->cert_hashes : NULL,
        opt ? (int)opt->cert_hash_count : 0,
        opt ? (int)opt->idle_timeout_ms : 0,
        opt && opt->connect_timeout_ms ? (int)opt->connect_timeout_ms : 10000);
    return wt;
}

int ngh_wt_state(ngh_wt *wt) {
    if (!wt) return NGH_WT_ERROR;
    return ngh_wt__js_state(wt->h);
}

const char *ngh_wt_error(ngh_wt *wt) {
    if (!wt) return "invalid handle";
    if (!ngh_wt__js_error(wt->h, wt->err, (int)sizeof(wt->err))) return NULL;
    return wt->err;
}

void ngh_wt_close(ngh_wt *wt, uint32_t error_code, const char *reason) {
    if (wt) ngh_wt__js_close(wt->h, (int)error_code, reason);
}

void ngh_wt_destroy(ngh_wt *wt) {
    if (!wt) return;
    ngh_wt__js_session_destroy(wt->h);
    free(wt);
}

int ngh_wt_dgram_send(ngh_wt *wt, const void *data, size_t len) {
    if (!wt || (!data && len > 0)) return NGH_WT_ERR;
    return ngh_wt__js_dgram_send(wt->h, (const uint8_t *)data, (int)len);
}

int ngh_wt_dgram_recv(ngh_wt *wt, void *buf, size_t cap) {
    if (!wt || !buf) return NGH_WT_ERR;
    return ngh_wt__js_dgram_recv(wt->h, (uint8_t *)buf, (int)cap);
}

size_t ngh_wt_dgram_max(ngh_wt *wt) {
    if (!wt) return 0;
    return (size_t)ngh_wt__js_dgram_max(wt->h);
}

ngh_wt_stream *ngh_wt_stream_open(ngh_wt *wt, int bidi) {
    ngh_wt_stream *s;
    int sid;
    if (!wt) return NULL;
    sid = ngh_wt__js_stream_open(wt->h, bidi);
    if (sid == 0) return NULL;
    s = (ngh_wt_stream *)calloc(1, sizeof(ngh_wt_stream));
    if (!s) {
        ngh_wt__js_stream_destroy(sid);
        return NULL;
    }
    s->sid = sid;
    return s;
}

ngh_wt_stream *ngh_wt_stream_accept(ngh_wt *wt) {
    ngh_wt_stream *s;
    int sid;
    if (!wt) return NULL;
    sid = ngh_wt__js_stream_accept(wt->h);
    if (sid == 0) return NULL;
    s = (ngh_wt_stream *)calloc(1, sizeof(ngh_wt_stream));
    if (!s) {
        ngh_wt__js_stream_destroy(sid);
        return NULL;
    }
    s->sid = sid;
    return s;
}

int ngh_wt_stream_bidi(ngh_wt_stream *s) {
    if (!s) return 0;
    return ngh_wt__js_stream_bidi(s->sid);
}

int ngh_wt_stream_write(ngh_wt_stream *s, const void *data, size_t len) {
    if (!s || (!data && len > 0)) return NGH_WT_ERR;
    return ngh_wt__js_stream_write(s->sid, (const uint8_t *)data, (int)len);
}

int ngh_wt_stream_finish(ngh_wt_stream *s) {
    if (!s) return NGH_WT_ERR;
    return ngh_wt__js_stream_finish(s->sid);
}

int ngh_wt_stream_read(ngh_wt_stream *s, void *buf, size_t cap) {
    if (!s || !buf) return NGH_WT_ERR;
    return ngh_wt__js_stream_read(s->sid, (uint8_t *)buf, (int)cap);
}

void ngh_wt_stream_reset(ngh_wt_stream *s, uint32_t error_code) {
    if (s) ngh_wt__js_stream_reset(s->sid, (int)error_code);
}

void ngh_wt_stream_destroy(ngh_wt_stream *s) {
    if (!s) return;
    ngh_wt__js_stream_destroy(s->sid);
    free(s);
}

#endif /* __EMSCRIPTEN__ */

#endif /* NGH_WEBTRANSPORT_IMPLEMENTATION */
#endif /* NGH_WEBTRANSPORT_H_INCLUDED */
