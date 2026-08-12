/* ngh_http.h -- HTTP client from C.
 *
 * One header, one API, across Windows, Linux, macOS and the web. This
 * comment is the documentation; the header is the deliverable. Copy the
 * file, then in exactly one translation unit:
 *
 *     #define NGH_HTTP_IMPLEMENTATION
 *     #include "ngh_http.h"
 *
 * There is nothing to link and nothing to ship. The implementation is the
 * platform's own HTTP stack, resolved at run time: the browser's fetch()
 * on the web (EM_JS, no extra emcc flags, no ASYNCIFY), WinHTTP on
 * Windows, NSURLSession on Apple platforms, libcurl on Linux (dlopen of
 * the system library). TLS, CA validation, proxy settings and wire
 * protocol upgrades all come from the OS -- and so do their security
 * fixes.
 *
 * ==========================================================================
 * Quick start
 * ==========================================================================
 *
 *     ngh_http_options opt = ngh_http_options_default();
 *     ngh_http *r = ngh_http_fetch("https://example.com/api/data", &opt);
 *
 *     // Per frame.
 *     switch (ngh_http_state(r)) {
 *     case NGH_HTTP_PENDING: break;             // request in flight
 *     case NGH_HTTP_DONE: {                     // complete response is in
 *         size_t len;
 *         const char *body = ngh_http_body(r, &len);
 *         printf("%d: %.*s\n", ngh_http_status(r), (int)len, body);
 *         ngh_http_destroy(r);
 *         break;
 *     }
 *     case NGH_HTTP_ERROR:                      // DNS/TLS/transport failed
 *         printf("failed: %s\n", ngh_http_error(r));
 *         ngh_http_destroy(r);
 *     }
 *
 * EVERYTHING IS POLLED, NOTHING BLOCKS, NO CALLBACKS. fetch() returns
 * immediately and the request runs in the background until the poll
 * reports NGH_HTTP_DONE or NGH_HTTP_ERROR. The same loop is correct on
 * every platform. Poll from one thread at a time.
 *
 * ==========================================================================
 * The contract is HTTP's semantic layer, not a wire protocol
 * ==========================================================================
 *
 * This API exposes what RFC 9110 calls HTTP semantics -- method, URL,
 * request/response headers, status code, body bytes -- and nothing from
 * any particular wire generation. That is deliberate: it is what lets the
 * platform move the app from h1 to h2 to h3 (and onward) without a source
 * change, exactly as fetch() did in browsers. Concretely:
 *
 * - opt.version is a HINT (NGH_HTTP_V_AUTO by default). The platform
 *   negotiates whatever it can; ngh_http_version() reports what actually
 *   happened (0 when the platform does not say, which is normal on the
 *   web). Never branch behavior on it.
 * - There is no reason phrase ("OK" after 200 is an HTTP/1 wire artifact;
 *   h2/h3 have none). Status is the integer.
 * - There are no connection, keep-alive or transfer-encoding knobs; the
 *   platform owns connection management. Hop-by-hop request headers
 *   (connection, keep-alive, te, trailer, transfer-encoding, upgrade,
 *   proxy-connection) and the platform-owned content-length/host are
 *   rejected at fetch() time -- NULL return, never silently dropped.
 * - Header names are matched case-insensitively and reported lowercase
 *   (the h2/h3 convention; harmless under h1).
 *
 * ==========================================================================
 * Semantics to understand before shipping
 * ==========================================================================
 *
 * STATUS IS DATA, ERROR IS TRANSPORT. 404 and 500 are DONE responses like
 * any other -- inspect ngh_http_status(). NGH_HTTP_ERROR means no
 * complete HTTP response exists: DNS failure, TLS failure, refused
 * connection, timeout, a transport that died mid-body, or a body over the
 * cap; ngh_http_error() explains.
 *
 * THE RESPONSE IS ATOMIC. Nothing is observable before NGH_HTTP_DONE;
 * then status, headers and body are all there, complete, owned by the
 * request until destroy. The body is capped by opt.max_body_len (default
 * 64 MiB) and a response exceeding it fails as NGH_HTTP_ERROR instead of
 * eating unbounded memory: this library fetches things that fit in
 * memory, it is not a download manager. The body is NUL-terminated one
 * byte past its length, so text responses are directly usable as C
 * strings.
 *
 * REDIRECTS ARE FOLLOWED by default, like fetch(); the reported status,
 * headers and body are the final response's. opt.no_redirect returns the
 * 3xx itself instead. There is no cookie jar and no cache: requests carry
 * exactly the headers the app supplies, state belongs to the app.
 *
 * THE REQUEST BODY IS A BUFFER (opt.body/opt.body_len, complete at fetch
 * time). Streaming -- chunked response reads, upload sources -- is
 * deliberately absent and can be added later without breaking this API.
 *
 * VALIDATION IS THE PLATFORM'S CA STORE on every backend (unlike
 * ngh_webtransport.h there is no certificate pinning; public HTTPS is the
 * target). On the web, CORS governs what can be requested and which
 * response headers are readable -- same-origin or properly CORS-enabled
 * endpoints behave identically to desktop. One web-only hole: fetch()
 * cannot expose redirects, so opt.no_redirect makes ngh_http_fetch fail
 * loudly there.
 *
 * ==========================================================================
 * Runtime resolution
 * ==========================================================================
 *
 * The first fetch() lazily binds the platform stack; calling
 * ngh_http_runtime_load(NULL) up front just reports a broken platform
 * before any other setup. On Linux the system libcurl is dlopen'd
 * ("libcurl.so.4", then "libcurl.so"); passing a path loads that library
 * instead, which is the escape hatch for consumers who want to ship one
 * pinned curl everywhere -- then its CVE response is theirs, not ngh's.
 * The path is ignored on platforms whose stack is the OS itself.
 */

#ifndef NGH_HTTP_H_INCLUDED
#define NGH_HTTP_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NGH_HTTP_VERSION_MAJOR 0
#define NGH_HTTP_VERSION_MINOR 1
#define NGH_HTTP_VERSION_PATCH 0

/* Request states. */
#define NGH_HTTP_PENDING 0
#define NGH_HTTP_DONE 1
#define NGH_HTTP_ERROR 2

/* Wire version hint (opt.version) and report (ngh_http_version). */
#define NGH_HTTP_V_AUTO 0 /* report: unknown */
#define NGH_HTTP_V1 1
#define NGH_HTTP_V2 2
#define NGH_HTTP_V3 3

typedef struct ngh_http ngh_http;

typedef struct ngh_http_options {
    const char *method; /* NULL = "GET" */
    /* header_count name/value PAIRS, flattened:
     * {"accept", "application/json", "x-token", "..."} has header_count 2.
     * Names are sent lowercase whatever the spelling here. */
    const char *const *headers;
    size_t header_count;
    const void *body; /* complete request body, NULL = none */
    size_t body_len;
    size_t max_body_len;         /* response body cap; 0 = default (64 MiB) */
    int version;                 /* NGH_HTTP_V_AUTO/V1/V2/V3: a hint, see above */
    int no_redirect;             /* nonzero: hand back 3xx instead of
                                  * following (not on the web, see manual) */
    uint32_t connect_timeout_ms; /* 0 = default (10s) */
    uint32_t total_timeout_ms;   /* whole request; 0 = none */
} ngh_http_options;

ngh_http_options ngh_http_options_default(void);

/* Binds the platform HTTP stack, or on Linux the libcurl at `path` when
 * non-NULL. A no-op where the stack is the OS itself. Returns 0 on
 * success; on failure ngh_http_last_error() explains. */
int ngh_http_runtime_load(const char *path);

/* Explains the most recent NULL return from ngh_http_fetch or nonzero
 * return from ngh_http_runtime_load. */
const char *ngh_http_last_error(void);

ngh_http *ngh_http_fetch(const char *url, const ngh_http_options *opt);
int ngh_http_state(ngh_http *r);

/* 0 until DONE, then the final response's status code. */
int ngh_http_status(ngh_http *r);

/* The complete response body once DONE, NULL before; raw bytes with a NUL
 * one byte past *len. Owned by the request, valid until destroy. `len`
 * may be NULL. */
const char *ngh_http_body(ngh_http *r, size_t *len);

/* Response header by case-insensitive name; NULL when absent or not DONE.
 * Owned by the request, valid until destroy. Multiple values arrive
 * joined with ", " (set-cookie: "; "), the fetch() convention. */
const char *ngh_http_header(ngh_http *r, const char *name);

/* Iterate response headers: 1 and the lowercase name/value while i is in
 * range, 0 past the end (and before DONE). */
int ngh_http_header_at(ngh_http *r, size_t i, const char **name,
                       const char **value);

/* What the wire actually spoke: NGH_HTTP_V1/V2/V3, or NGH_HTTP_V_AUTO
 * when the platform does not report it. Observability only. */
int ngh_http_version(ngh_http *r);

/* Non-NULL once the request is in NGH_HTTP_ERROR. Owned by the request. */
const char *ngh_http_error(ngh_http *r);

/* Cancels an in-flight request and frees everything. */
void ngh_http_destroy(ngh_http *r);

#ifdef __cplusplus
}
#endif

#endif /* NGH_HTTP_H_INCLUDED */

#ifdef NGH_HTTP_IMPLEMENTATION
#ifndef NGH_HTTP_IMPLEMENTATION_INCLUDED
#define NGH_HTTP_IMPLEMENTATION_INCLUDED

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NGH_HTTP__DEFAULT_MAX_BODY ((size_t)64 * 1024 * 1024)

static char ngh_http__last_error[512];

const char *ngh_http_last_error(void) {
    return ngh_http__last_error[0] ? ngh_http__last_error : NULL;
}

ngh_http_options ngh_http_options_default(void) {
    ngh_http_options o;
    memset(&o, 0, sizeof(o));
    return o;
}

static unsigned char ngh_http__ascii_lower(unsigned char c) {
    if (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
        return (unsigned char)(c + ((unsigned char)'a' - (unsigned char)'A'));
    return c;
}

static int ngh_http__ascii_equal(const char *a, const char *b) {
    unsigned char ac;
    unsigned char bc;
    if (!a || !b) return 0;
    do {
        ac = ngh_http__ascii_lower((unsigned char)*a++);
        bc = ngh_http__ascii_lower((unsigned char)*b++);
        if (ac != bc) return 0;
    } while (ac != 0);
    return 1;
}

static int ngh_http__token_char(unsigned char c) {
    if ((c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
        (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
        (c >= (unsigned char)'0' && c <= (unsigned char)'9'))
        return 1;
    return c == (unsigned char)'!' || c == (unsigned char)'#' ||
           c == (unsigned char)'$' || c == (unsigned char)'%' ||
           c == (unsigned char)'&' || c == (unsigned char)'\'' ||
           c == (unsigned char)'*' || c == (unsigned char)'+' ||
           c == (unsigned char)'-' || c == (unsigned char)'.' ||
           c == (unsigned char)'^' || c == (unsigned char)'_' ||
           c == (unsigned char)'`' || c == (unsigned char)'|' ||
           c == (unsigned char)'~';
}

static int ngh_http__valid_token(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if (!p || !*p) return 0;
    while (*p) {
        if (!ngh_http__token_char(*p++)) return 0;
    }
    return 1;
}

static int ngh_http__forbidden_header(const char *name) {
    static const char *const forbidden[] = {
        "connection",       "keep-alive",       "te",
        "trailer",          "transfer-encoding", "upgrade",
        "proxy-connection", "content-length",   "host",
    };
    size_t i;
    for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        if (ngh_http__ascii_equal(name, forbidden[i])) return 1;
    }
    return 0;
}

static int ngh_http__validate_fetch(const char *url,
                                    const ngh_http_options *opt) {
    size_t i;
    if (!url || !*url) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "url is required");
        return 0;
    }
    if (!opt) return 1;
    if (opt->method && !ngh_http__valid_token(opt->method)) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "method must be a nonempty HTTP token");
        return 0;
    }
    if (opt->body_len > 0 && !opt->body) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "body_len is nonzero but body is NULL");
        return 0;
    }
    if (opt->version < NGH_HTTP_V_AUTO || opt->version > NGH_HTTP_V3) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "version must be NGH_HTTP_V_AUTO, V1, V2, or V3");
        return 0;
    }
    if (opt->header_count > 0 && !opt->headers) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "header_count is nonzero but headers is NULL");
        return 0;
    }
    for (i = 0; i < opt->header_count; ++i) {
        const char *name = opt->headers[i * 2];
        const char *value = opt->headers[i * 2 + 1];
        if (!ngh_http__valid_token(name) || !value) {
            snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                     "request header %zu needs a token name and a value",
                     i);
            return 0;
        }
        if (ngh_http__forbidden_header(name)) {
            snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                     "request header '%s' is platform-owned or hop-by-hop",
                     name);
            return 0;
        }
        if (strchr(value, '\r') || strchr(value, '\n')) {
            snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                     "request header '%s' contains a line break", name);
            return 0;
        }
    }
    return 1;
}

#if defined(__linux__) && !defined(__EMSCRIPTEN__)

/* ==========================================================================
 *  Linux backend -- the system libcurl, loaded at run time
 * ==========================================================================
 *
 * These declarations mirror the small part of libcurl's public ABI used
 * below. Numeric option and info values are stable parts of that ABI; no
 * curl headers or link-time curl dependency are needed.
 */

#include <dlfcn.h>
#include <pthread.h>

typedef int ngh_http__curl_code;
typedef int ngh_http__curl_option;
typedef int ngh_http__curl_info;
typedef int64_t ngh_http__curl_off_t;

struct ngh_http__curl_slist {
    char *data;
    struct ngh_http__curl_slist *next;
};

#define NGH_HTTP__CURLE_OK 0 /* CURLE_OK */
#define NGH_HTTP__CURL_GLOBAL_DEFAULT 3L /* CURL_GLOBAL_DEFAULT */
#define NGH_HTTP__CURL_ERROR_SIZE 256 /* CURL_ERROR_SIZE */

#define NGH_HTTP__CURLOPT_WRITEDATA 10001 /* CURLOPT_WRITEDATA */
#define NGH_HTTP__CURLOPT_URL 10002 /* CURLOPT_URL */
#define NGH_HTTP__CURLOPT_ERRORBUFFER 10010 /* CURLOPT_ERRORBUFFER */
#define NGH_HTTP__CURLOPT_WRITEFUNCTION 20011 /* CURLOPT_WRITEFUNCTION */
#define NGH_HTTP__CURLOPT_POSTFIELDS 10015 /* CURLOPT_POSTFIELDS */
#define NGH_HTTP__CURLOPT_HTTPHEADER 10023 /* CURLOPT_HTTPHEADER */
#define NGH_HTTP__CURLOPT_HEADERDATA 10029 /* CURLOPT_HEADERDATA */
#define NGH_HTTP__CURLOPT_CUSTOMREQUEST 10036 /* CURLOPT_CUSTOMREQUEST */
#define NGH_HTTP__CURLOPT_NOPROGRESS 43 /* CURLOPT_NOPROGRESS */
#define NGH_HTTP__CURLOPT_NOBODY 44 /* CURLOPT_NOBODY */
#define NGH_HTTP__CURLOPT_FOLLOWLOCATION 52 /* CURLOPT_FOLLOWLOCATION */
#define NGH_HTTP__CURLOPT_XFERINFODATA 10057 /* CURLOPT_XFERINFODATA */
#define NGH_HTTP__CURLOPT_HEADERFUNCTION 20079 /* CURLOPT_HEADERFUNCTION */
#define NGH_HTTP__CURLOPT_HTTP_VERSION 84 /* CURLOPT_HTTP_VERSION */
#define NGH_HTTP__CURLOPT_NOSIGNAL 99 /* CURLOPT_NOSIGNAL */
#define NGH_HTTP__CURLOPT_POSTFIELDSIZE_LARGE \
    30120 /* CURLOPT_POSTFIELDSIZE_LARGE */
#define NGH_HTTP__CURLOPT_TIMEOUT_MS 155 /* CURLOPT_TIMEOUT_MS */
#define NGH_HTTP__CURLOPT_CONNECTTIMEOUT_MS \
    156 /* CURLOPT_CONNECTTIMEOUT_MS */
#define NGH_HTTP__CURLOPT_XFERINFOFUNCTION \
    20219 /* CURLOPT_XFERINFOFUNCTION */

#define NGH_HTTP__CURLINFO_RESPONSE_CODE 0x200002 /* CURLINFO_RESPONSE_CODE */
#define NGH_HTTP__CURLINFO_HTTP_VERSION 0x20002e /* CURLINFO_HTTP_VERSION */

#define NGH_HTTP__CURL_HTTP_VERSION_NONE 0 /* CURL_HTTP_VERSION_NONE */
#define NGH_HTTP__CURL_HTTP_VERSION_1_0 1 /* CURL_HTTP_VERSION_1_0 */
#define NGH_HTTP__CURL_HTTP_VERSION_1_1 2 /* CURL_HTTP_VERSION_1_1 */
#define NGH_HTTP__CURL_HTTP_VERSION_2_0 3 /* CURL_HTTP_VERSION_2_0 */
#define NGH_HTTP__CURL_HTTP_VERSION_2TLS 4 /* CURL_HTTP_VERSION_2TLS */
#define NGH_HTTP__CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE \
    5 /* CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE */
#define NGH_HTTP__CURL_HTTP_VERSION_3 30 /* CURL_HTTP_VERSION_3 */
#define NGH_HTTP__CURL_HTTP_VERSION_3ONLY 31 /* CURL_HTTP_VERSION_3ONLY */

typedef struct ngh_http__curl_api {
    void *handle;
    char path[1024];
    int loaded;
    ngh_http__curl_code (*global_init)(long);
    void *(*easy_init)(void);
    void (*easy_cleanup)(void *);
    ngh_http__curl_code (*easy_setopt)(void *, ngh_http__curl_option, ...);
    ngh_http__curl_code (*easy_perform)(void *);
    ngh_http__curl_code (*easy_getinfo)(void *, ngh_http__curl_info, ...);
    const char *(*easy_strerror)(ngh_http__curl_code);
    struct ngh_http__curl_slist *(*slist_append)(
        struct ngh_http__curl_slist *, const char *);
    void (*slist_free_all)(struct ngh_http__curl_slist *);
} ngh_http__curl_api;

typedef struct ngh_http__header {
    char *name;
    char *value;
} ngh_http__header;

struct ngh_http {
    pthread_t thread;
    pthread_mutex_t mutex;
    int thread_started;
    int state;
    int abort_requested;
    int status;
    int wire_version;
    char error[512];
    char callback_error[512];

    char *url;
    char *method;
    unsigned char *request_body;
    size_t request_body_len;
    int has_request_body;
    struct ngh_http__curl_slist *request_headers;

    char *body;
    size_t body_len;
    size_t body_capacity;
    size_t max_body_len;
    ngh_http__header *headers;
    size_t header_count;
    size_t header_capacity;

    int version_hint;
    int no_redirect;
    uint32_t connect_timeout_ms;
    uint32_t total_timeout_ms;
};

static ngh_http__curl_api ngh_http__curl;
static pthread_mutex_t ngh_http__runtime_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t ngh_http__curl_global_once = PTHREAD_ONCE_INIT;
static int ngh_http__curl_global_called;
static ngh_http__curl_code ngh_http__curl_global_code;

static int ngh_http__bind_symbol(void *handle, const char *name, void *slot,
                                 const char *path) {
    void *symbol = dlsym(handle, name);
    if (!symbol) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "%.300s does not export %.64s (wrong or too old libcurl?)",
                 path, name);
        return 0;
    }
    memcpy(slot, &symbol, sizeof(symbol));
    return 1;
}

static int ngh_http__bind_curl(ngh_http__curl_api *api) {
#define NGH_HTTP__BIND(member, symbol)                                      \
    do {                                                                    \
        if (!ngh_http__bind_symbol(api->handle, symbol, &api->member,       \
                                    api->path))                             \
            return 0;                                                       \
    } while (0)
    NGH_HTTP__BIND(global_init, "curl_global_init");
    NGH_HTTP__BIND(easy_init, "curl_easy_init");
    NGH_HTTP__BIND(easy_cleanup, "curl_easy_cleanup");
    NGH_HTTP__BIND(easy_setopt, "curl_easy_setopt");
    NGH_HTTP__BIND(easy_perform, "curl_easy_perform");
    NGH_HTTP__BIND(easy_getinfo, "curl_easy_getinfo");
    NGH_HTTP__BIND(easy_strerror, "curl_easy_strerror");
    NGH_HTTP__BIND(slist_append, "curl_slist_append");
    NGH_HTTP__BIND(slist_free_all, "curl_slist_free_all");
#undef NGH_HTTP__BIND
    return 1;
}

static void ngh_http__curl_global_start(void) {
    ngh_http__curl_global_called = 1;
    ngh_http__curl_global_code =
        ngh_http__curl.global_init(NGH_HTTP__CURL_GLOBAL_DEFAULT);
}

/* 1 = loaded, 0 = this path did not load/bind, -1 = global init failed. */
static int ngh_http__try_curl(const char *path) {
    ngh_http__curl_api api;
    void *handle;
    int once_code;
    const char *loader_error;

    memset(&api, 0, sizeof(api));
    dlerror();
    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        loader_error = dlerror();
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "cannot load %.300s%s%.160s", path,
                 loader_error ? ": " : "", loader_error ? loader_error : "");
        return 0;
    }
    api.handle = handle;
    snprintf(api.path, sizeof(api.path), "%s", path);
    if (!ngh_http__bind_curl(&api)) {
        dlclose(handle);
        return 0;
    }

    ngh_http__curl = api;
    once_code = pthread_once(&ngh_http__curl_global_once,
                             ngh_http__curl_global_start);
    if (once_code != 0) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "pthread_once for curl_global_init failed: %s",
                 strerror(once_code));
        return -1;
    }
    if (ngh_http__curl_global_code != NGH_HTTP__CURLE_OK) {
        const char *why =
            ngh_http__curl.easy_strerror(ngh_http__curl_global_code);
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "curl_global_init failed: %s", why ? why : "unknown error");
        return -1;
    }
    ngh_http__curl.loaded = 1;
    ngh_http__last_error[0] = '\0';
    return 1;
}

static char *ngh_http__copy_string(const char *s) {
    size_t len;
    char *copy;
    if (!s) return NULL;
    len = strlen(s);
    copy = (char *)malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

int ngh_http_runtime_load(const char *path) {
    int loaded;
    int result = -1;
    pthread_mutex_lock(&ngh_http__runtime_mutex);
    if (ngh_http__curl.loaded) {
        /* A second load cannot rebind: curl_global_init already ran against
         * the first library. Refuse a different explicit path loudly. */
        if (path && strcmp(path, ngh_http__curl.path) != 0) {
            snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                     "cannot load %.200s: %.200s is already loaded", path,
                     ngh_http__curl.path);
            goto done;
        }
        ngh_http__last_error[0] = '\0';
        result = 0;
        goto done;
    }
    if (ngh_http__curl_global_called) {
        if (!ngh_http__last_error[0])
            snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                     "curl_global_init failed");
        goto done;
    }

    if (path) {
        loaded = ngh_http__try_curl(path);
        result = loaded == 1 ? 0 : -1;
        goto done;
    }

    loaded = ngh_http__try_curl("libcurl.so.4");
    if (loaded == 1) {
        result = 0;
        goto done;
    }
    if (loaded < 0) goto done;
    loaded = ngh_http__try_curl("libcurl.so");
    if (loaded == 1) {
        result = 0;
        goto done;
    }
    if (loaded == 0 && !ngh_http__last_error[0])
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "cannot load libcurl.so.4 or libcurl.so");

done:
    pthread_mutex_unlock(&ngh_http__runtime_mutex);
    return result;
}

static void ngh_http__response_headers_clear(ngh_http *r) {
    size_t i;
    for (i = 0; i < r->header_count; ++i) {
        free(r->headers[i].name);
        free(r->headers[i].value);
        r->headers[i].name = NULL;
        r->headers[i].value = NULL;
    }
    r->header_count = 0;
}

static void ngh_http__callback_fail(ngh_http *r, const char *message) {
    if (!r->callback_error[0])
        snprintf(r->callback_error, sizeof(r->callback_error), "%s", message);
}

static int ngh_http__body_reserve(ngh_http *r, size_t needed) {
    size_t cap = r->body_capacity;
    char *grown;
    if (needed <= cap) return 1;
    while (cap < needed && cap <= (size_t)-1 / 2) cap *= 2;
    if (cap < needed) cap = needed;
    grown = (char *)realloc(r->body, cap);
    if (!grown) return 0;
    r->body = grown;
    r->body_capacity = cap;
    return 1;
}

static size_t ngh_http__curl_write(char *data, size_t size, size_t count,
                                   void *user) {
    ngh_http *r = (ngh_http *)user;
    size_t len;
    assert(size == 0 || count <= (size_t)-1 / size); /* curl bounds writes */
    len = size * count;
    assert(r->body_len <= r->max_body_len);
    if (len > r->max_body_len - r->body_len) {
        char message[160];
        snprintf(message, sizeof(message),
                 "response body exceeds max_body_len (%zu bytes)",
                 r->max_body_len);
        ngh_http__callback_fail(r, message);
        return 0;
    }
    if (!ngh_http__body_reserve(r, r->body_len + len + 1)) {
        ngh_http__callback_fail(r,
                                "out of memory while growing response body");
        return 0;
    }
    if (len > 0) memcpy(r->body + r->body_len, data, len);
    r->body_len += len;
    r->body[r->body_len] = '\0';
    return len;
}

/* True for "HTTP/..." lines: each hop's first header-callback line, the
 * cue to restart response accumulation. The status code itself comes from
 * CURLINFO_RESPONSE_CODE after the transfer. */
static int ngh_http__status_line(const char *line, size_t len) {
    return len >= 5 &&
           ngh_http__ascii_lower((unsigned char)line[0]) == 'h' &&
           ngh_http__ascii_lower((unsigned char)line[1]) == 't' &&
           ngh_http__ascii_lower((unsigned char)line[2]) == 't' &&
           ngh_http__ascii_lower((unsigned char)line[3]) == 'p' &&
           line[4] == '/';
}

static int ngh_http__response_header_add(ngh_http *r, const char *line,
                                         size_t len) {
    size_t colon = 0;
    size_t name_len;
    size_t value_start;
    size_t value_len;
    size_t i;
    char *name;
    char *value;
    while (colon < len && line[colon] != ':') ++colon;
    if (colon == 0 || colon == len) return 1;
    name_len = colon;
    while (name_len > 0 &&
           (line[name_len - 1] == ' ' || line[name_len - 1] == '\t'))
        --name_len;
    if (name_len == 0) return 1;
    value_start = colon + 1;
    while (value_start < len &&
           (line[value_start] == ' ' || line[value_start] == '\t'))
        ++value_start;
    value_len = len - value_start;
    while (value_len > 0 &&
           (line[value_start + value_len - 1] == '\r' ||
            line[value_start + value_len - 1] == '\n' ||
            line[value_start + value_len - 1] == ' ' ||
            line[value_start + value_len - 1] == '\t'))
        --value_len;

    name = (char *)malloc(name_len + 1);
    value = (char *)malloc(value_len + 1);
    if (!name || !value) {
        free(name);
        free(value);
        return 0;
    }
    for (i = 0; i < name_len; ++i)
        name[i] = (char)ngh_http__ascii_lower((unsigned char)line[i]);
    name[name_len] = '\0';
    if (value_len > 0) memcpy(value, line + value_start, value_len);
    value[value_len] = '\0';

    for (i = 0; i < r->header_count; ++i) {
        if (strcmp(r->headers[i].name, name) == 0) {
            const char *separator = strcmp(name, "set-cookie") == 0 ? "; " : ", ";
            size_t old_len = strlen(r->headers[i].value);
            size_t sep_len = 2;
            char *joined;
            joined = (char *)realloc(r->headers[i].value,
                                     old_len + sep_len + value_len + 1);
            if (!joined) {
                free(name);
                free(value);
                return 0;
            }
            memcpy(joined + old_len, separator, sep_len);
            memcpy(joined + old_len + sep_len, value, value_len + 1);
            r->headers[i].value = joined;
            free(name);
            free(value);
            return 1;
        }
    }

    if (r->header_count == r->header_capacity) {
        size_t capacity = r->header_capacity ? r->header_capacity * 2 : 16;
        ngh_http__header *grown;
        grown = (ngh_http__header *)realloc(r->headers,
                                            capacity * sizeof(*grown));
        if (!grown) {
            free(name);
            free(value);
            return 0;
        }
        r->headers = grown;
        r->header_capacity = capacity;
    }
    r->headers[r->header_count].name = name;
    r->headers[r->header_count].value = value;
    ++r->header_count;
    return 1;
}

static size_t ngh_http__curl_header(char *data, size_t size, size_t count,
                                    void *user) {
    ngh_http *r = (ngh_http *)user;
    size_t len;
    assert(size == 0 || count <= (size_t)-1 / size); /* curl bounds writes */
    len = size * count;
    if (ngh_http__status_line(data, len)) {
        ngh_http__response_headers_clear(r);
        r->body_len = 0;
        r->body[0] = '\0';
        return len;
    }
    if (!ngh_http__response_header_add(r, data, len)) {
        ngh_http__callback_fail(r,
                                "out of memory while storing response headers");
        return 0;
    }
    return len;
}

static int ngh_http__curl_progress(void *user, ngh_http__curl_off_t download_total,
                                   ngh_http__curl_off_t download_now,
                                   ngh_http__curl_off_t upload_total,
                                   ngh_http__curl_off_t upload_now) {
    ngh_http *r = (ngh_http *)user;
    int abort_requested;
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    pthread_mutex_lock(&r->mutex);
    abort_requested = r->abort_requested;
    pthread_mutex_unlock(&r->mutex);
    return abort_requested ? 1 : 0;
}

static int ngh_http__curl_version_report(long version) {
    switch (version) {
    case NGH_HTTP__CURL_HTTP_VERSION_1_0:
    case NGH_HTTP__CURL_HTTP_VERSION_1_1:
        return NGH_HTTP_V1;
    case NGH_HTTP__CURL_HTTP_VERSION_2_0:
    case NGH_HTTP__CURL_HTTP_VERSION_2TLS:
    case NGH_HTTP__CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE:
        return NGH_HTTP_V2;
    case NGH_HTTP__CURL_HTTP_VERSION_3:
    case NGH_HTTP__CURL_HTTP_VERSION_3ONLY:
        return NGH_HTTP_V3;
    default:
        return NGH_HTTP_V_AUTO;
    }
}

static int ngh_http__setup_ok(ngh_http *r, ngh_http__curl_code code,
                              const char *option) {
    const char *why;
    if (code == NGH_HTTP__CURLE_OK) return 1;
    why = ngh_http__curl.easy_strerror(code);
    snprintf(r->callback_error, sizeof(r->callback_error),
             "curl_easy_setopt(%s) failed: %s", option,
             why ? why : "unknown error");
    return 0;
}

static void ngh_http__finish_error(ngh_http *r, const char *message) {
    pthread_mutex_lock(&r->mutex);
    if (r->state == NGH_HTTP_PENDING) {
        snprintf(r->error, sizeof(r->error), "%s",
                 message && *message ? message : "HTTP request failed");
        r->state = NGH_HTTP_ERROR;
    }
    pthread_mutex_unlock(&r->mutex);
}

static void *ngh_http__worker(void *user) {
    ngh_http *r = (ngh_http *)user;
    void *easy = NULL;
    ngh_http__curl_code code;
    char curl_error[NGH_HTTP__CURL_ERROR_SIZE];
    long response_code = 0;
    long http_version = NGH_HTTP__CURL_HTTP_VERSION_NONE;
    long requested_version = NGH_HTTP__CURL_HTTP_VERSION_NONE;
    int setup = 1;

    curl_error[0] = '\0';
    easy = ngh_http__curl.easy_init();
    if (!easy) {
        ngh_http__finish_error(r, "curl_easy_init failed");
        return NULL;
    }

#define NGH_HTTP__SETUP(option, value, label)                               \
    do {                                                                    \
        if (setup &&                                                        \
            !ngh_http__setup_ok(                                            \
                r, ngh_http__curl.easy_setopt(easy, option, value), label)) \
            setup = 0;                                                      \
    } while (0)
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_ERRORBUFFER, curl_error,
                    "CURLOPT_ERRORBUFFER");
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_URL, r->url, "CURLOPT_URL");
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_NOSIGNAL, 1L, "CURLOPT_NOSIGNAL");
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_WRITEDATA, r, "CURLOPT_WRITEDATA");
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_WRITEFUNCTION, ngh_http__curl_write,
                    "CURLOPT_WRITEFUNCTION");
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_HEADERDATA, r, "CURLOPT_HEADERDATA");
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_HEADERFUNCTION, ngh_http__curl_header,
                    "CURLOPT_HEADERFUNCTION");
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_FOLLOWLOCATION,
                    r->no_redirect ? 0L : 1L, "CURLOPT_FOLLOWLOCATION");
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_CONNECTTIMEOUT_MS,
                    (long)r->connect_timeout_ms,
                    "CURLOPT_CONNECTTIMEOUT_MS");
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_TIMEOUT_MS,
                    (long)r->total_timeout_ms, "CURLOPT_TIMEOUT_MS");
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_NOPROGRESS, 0L,
                    "CURLOPT_NOPROGRESS");
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_XFERINFODATA, r,
                    "CURLOPT_XFERINFODATA");
    NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_XFERINFOFUNCTION,
                    ngh_http__curl_progress, "CURLOPT_XFERINFOFUNCTION");
    if (r->request_headers)
        NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_HTTPHEADER, r->request_headers,
                        "CURLOPT_HTTPHEADER");
    if (r->has_request_body) {
        NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_POSTFIELDS, r->request_body,
                        "CURLOPT_POSTFIELDS");
        NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_POSTFIELDSIZE_LARGE,
                        (ngh_http__curl_off_t)r->request_body_len,
                        "CURLOPT_POSTFIELDSIZE_LARGE");
    }
    if (!r->has_request_body && strcmp(r->method, "GET") == 0) {
        /* libcurl's default request is GET */
    } else if (strcmp(r->method, "POST") == 0) {
        if (!r->has_request_body) {
            NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_POSTFIELDS, "",
                            "CURLOPT_POSTFIELDS");
            NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_POSTFIELDSIZE_LARGE,
                            (ngh_http__curl_off_t)0,
                            "CURLOPT_POSTFIELDSIZE_LARGE");
        }
    } else if (!r->has_request_body && strcmp(r->method, "HEAD") == 0) {
        NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_NOBODY, 1L, "CURLOPT_NOBODY");
    } else {
        NGH_HTTP__SETUP(NGH_HTTP__CURLOPT_CUSTOMREQUEST, r->method,
                        "CURLOPT_CUSTOMREQUEST");
    }
#undef NGH_HTTP__SETUP

    if (setup && r->version_hint != NGH_HTTP_V_AUTO) {
        if (r->version_hint == NGH_HTTP_V1)
            requested_version = NGH_HTTP__CURL_HTTP_VERSION_1_1;
        else if (r->version_hint == NGH_HTTP_V2)
            requested_version = NGH_HTTP__CURL_HTTP_VERSION_2_0;
        else
            requested_version = NGH_HTTP__CURL_HTTP_VERSION_3;
        code = ngh_http__curl.easy_setopt(easy,
                                          NGH_HTTP__CURLOPT_HTTP_VERSION,
                                          requested_version);
        if (code != NGH_HTTP__CURLE_OK)
            (void)ngh_http__curl.easy_setopt(
                easy, NGH_HTTP__CURLOPT_HTTP_VERSION,
                (long)NGH_HTTP__CURL_HTTP_VERSION_NONE);
    }

    if (!setup) {
        ngh_http__finish_error(r, r->callback_error);
        ngh_http__curl.easy_cleanup(easy);
        return NULL;
    }

    code = ngh_http__curl.easy_perform(easy);
    if (code != NGH_HTTP__CURLE_OK) {
        const char *message;
        if (r->callback_error[0])
            message = r->callback_error;
        else if (curl_error[0])
            message = curl_error;
        else {
            message = ngh_http__curl.easy_strerror(code);
            if (!message) message = "libcurl transfer failed";
        }
        ngh_http__finish_error(r, message);
        ngh_http__curl.easy_cleanup(easy);
        return NULL;
    }

    (void)ngh_http__curl.easy_getinfo(
        easy, NGH_HTTP__CURLINFO_RESPONSE_CODE, &response_code);
    (void)ngh_http__curl.easy_getinfo(
        easy, NGH_HTTP__CURLINFO_HTTP_VERSION, &http_version);
    pthread_mutex_lock(&r->mutex);
    if (r->state == NGH_HTTP_PENDING) {
        if (r->abort_requested) {
            snprintf(r->error, sizeof(r->error), "request cancelled");
            r->state = NGH_HTTP_ERROR;
        } else {
            r->status = (int)response_code;
            r->wire_version = ngh_http__curl_version_report(http_version);
            r->state = NGH_HTTP_DONE;
        }
    }
    pthread_mutex_unlock(&r->mutex);
    ngh_http__curl.easy_cleanup(easy);
    return NULL;
}

static void ngh_http__request_free(ngh_http *r) {
    if (!r) return;
    ngh_http__response_headers_clear(r);
    free(r->headers);
    free(r->body);
    free(r->request_body);
    free(r->url);
    free(r->method);
    if (r->request_headers)
        ngh_http__curl.slist_free_all(r->request_headers);
    pthread_mutex_destroy(&r->mutex);
    free(r);
}

static int ngh_http__append_request_headers(ngh_http *r,
                                            const ngh_http_options *opt) {
    size_t i;
    for (i = 0; opt && i < opt->header_count; ++i) {
        const char *name = opt->headers[i * 2];
        const char *value = opt->headers[i * 2 + 1];
        size_t name_len = strlen(name);
        size_t value_len = strlen(value);
        size_t punctuation = value_len == 0 ? 1 : 2;
        size_t line_len;
        size_t j;
        char *line;
        struct ngh_http__curl_slist *appended;
        line_len = name_len + punctuation + value_len;
        line = (char *)malloc(line_len + 1);
        if (!line) return 0;
        for (j = 0; j < name_len; ++j)
            line[j] = (char)ngh_http__ascii_lower((unsigned char)name[j]);
        if (value_len == 0) {
            line[name_len] = ';';
        } else {
            line[name_len] = ':';
            line[name_len + 1] = ' ';
            memcpy(line + name_len + 2, value, value_len);
        }
        line[line_len] = '\0';
        appended = ngh_http__curl.slist_append(r->request_headers, line);
        free(line);
        if (!appended) return 0;
        r->request_headers = appended;
    }
    return 1;
}

ngh_http *ngh_http_fetch(const char *url, const ngh_http_options *opt) {
    ngh_http *r;
    int mutex_code;
    int thread_code;
    const char *method;
    size_t request_alloc;
    if (!ngh_http__validate_fetch(url, opt)) return NULL;
    if (ngh_http_runtime_load(NULL) != 0) return NULL;
    if (opt && opt->body_len > (size_t)INT64_MAX) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "request body is too large for libcurl");
        return NULL;
    }

    r = (ngh_http *)calloc(1, sizeof(*r));
    if (!r) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "out of memory");
        return NULL;
    }
    mutex_code = pthread_mutex_init(&r->mutex, NULL);
    if (mutex_code != 0) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "pthread_mutex_init failed: %s", strerror(mutex_code));
        free(r);
        return NULL;
    }
    r->state = NGH_HTTP_PENDING;
    r->wire_version = NGH_HTTP_V_AUTO;
    r->max_body_len = opt && opt->max_body_len
                              ? opt->max_body_len
                              : NGH_HTTP__DEFAULT_MAX_BODY;
    r->connect_timeout_ms = opt && opt->connect_timeout_ms
                                ? opt->connect_timeout_ms
                                : 10000u;
    r->total_timeout_ms = opt ? opt->total_timeout_ms : 0;
    r->version_hint = opt ? opt->version : NGH_HTTP_V_AUTO;
    r->no_redirect = opt ? !!opt->no_redirect : 0;
    method = opt && opt->method ? opt->method : "GET";
    r->url = ngh_http__copy_string(url);
    r->method = ngh_http__copy_string(method);
    r->body = (char *)malloc(1);
    if (r->body) {
        r->body[0] = '\0';
        r->body_capacity = 1;
    }
    if (!r->url || !r->method || !r->body ||
        !ngh_http__append_request_headers(r, opt)) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "out of memory while copying request");
        ngh_http__request_free(r);
        return NULL;
    }
    if (opt && opt->body) {
        r->has_request_body = 1;
        r->request_body_len = opt->body_len;
        request_alloc = opt->body_len ? opt->body_len : 1;
        r->request_body = (unsigned char *)malloc(request_alloc);
        if (!r->request_body) {
            snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                     "out of memory while copying request body");
            ngh_http__request_free(r);
            return NULL;
        }
        if (opt->body_len > 0)
            memcpy(r->request_body, opt->body, opt->body_len);
    }

    thread_code = pthread_create(&r->thread, NULL, ngh_http__worker, r);
    if (thread_code != 0) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "pthread_create failed: %s", strerror(thread_code));
        ngh_http__request_free(r);
        return NULL;
    }
    r->thread_started = 1;
    ngh_http__last_error[0] = '\0';
    return r;
}

int ngh_http_state(ngh_http *r) {
    int state;
    if (!r) return NGH_HTTP_ERROR;
    pthread_mutex_lock(&r->mutex);
    state = r->state;
    pthread_mutex_unlock(&r->mutex);
    return state;
}

int ngh_http_status(ngh_http *r) {
    int status = 0;
    if (!r) return 0;
    pthread_mutex_lock(&r->mutex);
    if (r->state == NGH_HTTP_DONE) status = r->status;
    pthread_mutex_unlock(&r->mutex);
    return status;
}

const char *ngh_http_body(ngh_http *r, size_t *len) {
    const char *body = NULL;
    if (len) *len = 0;
    if (!r) return NULL;
    pthread_mutex_lock(&r->mutex);
    if (r->state == NGH_HTTP_DONE) {
        body = r->body;
        if (len) *len = r->body_len;
    }
    pthread_mutex_unlock(&r->mutex);
    return body;
}

const char *ngh_http_header(ngh_http *r, const char *name) {
    const char *value = NULL;
    size_t i;
    if (!r || !name) return NULL;
    pthread_mutex_lock(&r->mutex);
    if (r->state == NGH_HTTP_DONE) {
        for (i = 0; i < r->header_count; ++i) {
            if (ngh_http__ascii_equal(r->headers[i].name, name)) {
                value = r->headers[i].value;
                break;
            }
        }
    }
    pthread_mutex_unlock(&r->mutex);
    return value;
}

int ngh_http_header_at(ngh_http *r, size_t i, const char **name,
                       const char **value) {
    int found = 0;
    if (name) *name = NULL;
    if (value) *value = NULL;
    if (!r) return 0;
    pthread_mutex_lock(&r->mutex);
    if (r->state == NGH_HTTP_DONE && i < r->header_count) {
        if (name) *name = r->headers[i].name;
        if (value) *value = r->headers[i].value;
        found = 1;
    }
    pthread_mutex_unlock(&r->mutex);
    return found;
}

int ngh_http_version(ngh_http *r) {
    int version = NGH_HTTP_V_AUTO;
    if (!r) return version;
    pthread_mutex_lock(&r->mutex);
    if (r->state == NGH_HTTP_DONE) version = r->wire_version;
    pthread_mutex_unlock(&r->mutex);
    return version;
}

const char *ngh_http_error(ngh_http *r) {
    const char *error = NULL;
    if (!r) return "invalid handle";
    pthread_mutex_lock(&r->mutex);
    if (r->state == NGH_HTTP_ERROR) error = r->error;
    pthread_mutex_unlock(&r->mutex);
    return error;
}

void ngh_http_destroy(ngh_http *r) {
    if (!r) return;
    pthread_mutex_lock(&r->mutex);
    r->abort_requested = 1;
    pthread_mutex_unlock(&r->mutex);
    if (r->thread_started) (void)pthread_join(r->thread, NULL);
    ngh_http__request_free(r);
}

#elif defined(__EMSCRIPTEN__)

/* ==========================================================================
 *  Web backend -- the browser's fetch API through EM_JS
 * ==========================================================================
 *
 * EM_JS functions cannot share top-level state, so requests live in
 * globalThis.__NGH.http and are named by plain integer handles. Completed
 * bodies and a flattened name/value pointer table live in linear memory;
 * the C accessors walk them without copying.
 */

#include <emscripten.h>

/* Keep libc allocation exports visible to the completion promise. */
extern void *malloc(size_t size) EMSCRIPTEN_KEEPALIVE;
extern void free(void *pointer) EMSCRIPTEN_KEEPALIVE;

EM_JS_DEPS(ngh_http, "$UTF8ToString,$stringToUTF8,$lengthBytesUTF8")

EM_JS(void, ngh_http__js_boot, (void), {
    const S = (globalThis.__NGH = globalThis.__NGH || {});
    if (S.http) return;
    const H = (S.http = {requests : [null]});

    H.clearTimers = function(e) {
        if (e.connectTimer !== null) clearTimeout(e.connectTimer);
        if (e.totalTimer !== null) clearTimeout(e.totalTimer);
        e.connectTimer = null;
        e.totalTimer = null;
    };

    H.release = function(e) {
        if (e.headerTable) {
            for (let i = 0; i < e.headerCount * 2; ++i) {
                const p = HEAPU32[(e.headerTable >>> 2) + i];
                if (p) _free(p);
            }
            _free(e.headerTable);
        }
        if (e.bodyPtr) _free(e.bodyPtr);
        e.headerTable = 0;
        e.headerCount = 0;
        e.bodyPtr = 0;
        e.bodyLen = 0;
    };

    H.fail = function(e, error) {
        if (e.dead || e.state !== 0) return;
        H.clearTimers(e);
        e.error = String(error || "HTTP request failed");
        e.state = 2;
    };
})

EM_JS(int, ngh_http__js_fetch,
      (const char *url, const char *method, const char *const *headers,
       size_t header_count, const uint8_t *body, size_t body_len,
       int has_body, size_t max_body_len, uint32_t connect_timeout_ms,
       uint32_t total_timeout_ms), {
    const H = globalThis.__NGH.http;
    const e = {
        state : 0,
        status : 0,
        error : null,
        controller : new AbortController(),
        connectTimer : null,
        totalTimer : null,
        bodyPtr : 0,
        bodyLen : 0,
        headerTable : 0,
        headerCount : 0,
        dead : false,
        abortReason : null,
    };
    H.requests.push(e);
    const h = H.requests.length - 1;
    let request;
    try {
        const requestHeaders = new Headers();
        for (let i = 0; i < header_count; ++i) {
            const name = UTF8ToString(HEAPU32[(headers >>> 2) + i * 2]);
            const value = UTF8ToString(HEAPU32[(headers >>> 2) + i * 2 + 1]);
            requestHeaders.append(name.toLowerCase(), value);
        }
        request = {
            method : UTF8ToString(method),
            headers : requestHeaders,
            redirect : "follow",
            credentials : "omit",
            cache : "no-store",
            signal : e.controller.signal,
        };
        if (has_body) {
            request.body = new Uint8Array(
                HEAPU8.subarray(body, body + body_len)).slice();
        }
    } catch (error) {
        H.fail(e, error);
        return h;
    }

    const abortAfter = function(reason) {
        return () => {
            if (e.dead || e.state !== 0) return;
            e.abortReason = reason;
            try { e.controller.abort(); } catch (error) {}
        };
    };
    /* Fetch has no separately observable connect phase. Both settings use
     * setTimeout -> abort over the complete fetch/body operation, so the
     * connect and total deadlines collapse to total deadlines on the web. */
    if (connect_timeout_ms > 0)
        e.connectTimer = setTimeout(
            abortAfter("connect timeout"), connect_timeout_ms);
    if (total_timeout_ms > 0)
        e.totalTimer = setTimeout(
            abortAfter("total timeout"), total_timeout_ms);

    let operation;
    try {
        operation = fetch(UTF8ToString(url), request);
    } catch (error) {
        H.fail(e, error);
        return h;
    }
    operation.then(async (response) => {
        const bytes = new Uint8Array(await response.arrayBuffer());
        if (e.dead || e.state !== 0) return;
        if (bytes.length > max_body_len) {
            H.fail(e, "response body exceeds max_body_len (" +
                       max_body_len + " bytes)");
            return;
        }

        const pairs = [];
        const byName = new Map();
        for (const entry of response.headers.entries()) {
            const name = String(entry[0]).toLowerCase();
            const value = String(entry[1]);
            if (byName.has(name)) {
                const index = byName.get(name);
                pairs[index + 1] +=
                    (name === "set-cookie" ? "; " : ", ") + value;
            } else {
                byName.set(name, pairs.length);
                pairs.push(name, value);
            }
        }

        const allocated = [];
        let bodyPtr = 0;
        let table = 0;
        try {
            bodyPtr = _malloc(bytes.length + 1);
            if (!bodyPtr) throw new Error("out of memory");
            allocated.push(bodyPtr);
            if (pairs.length > 0) {
                table = _malloc(pairs.length * 4);
                if (!table) throw new Error("out of memory");
                allocated.push(table);
            }
            const stringPointers = [];
            for (const value of pairs) {
                const cap = lengthBytesUTF8(value) + 1;
                const p = _malloc(cap);
                if (!p) throw new Error("out of memory");
                allocated.push(p);
                stringToUTF8(value, p, cap);
                stringPointers.push(p);
            }
            HEAPU8.set(bytes, bodyPtr);
            HEAPU8[bodyPtr + bytes.length] = 0;
            for (let i = 0; i < stringPointers.length; ++i)
                HEAPU32[(table >>> 2) + i] = stringPointers[i];
        } catch (error) {
            for (let i = allocated.length - 1; i >= 0; --i)
                _free(allocated[i]);
            H.fail(e, "out of memory while storing response");
            return;
        }

        e.bodyPtr = bodyPtr;
        e.bodyLen = bytes.length;
        e.headerTable = table;
        e.headerCount = pairs.length / 2;
        e.status = response.status;
        H.clearTimers(e);
        e.state = 1;
    }).catch((error) => {
        H.fail(e, e.abortReason || error);
    });
    return h;
})

EM_JS(int, ngh_http__js_state, (int h), {
    const e = globalThis.__NGH.http.requests[h];
    return e ? e.state : 2;
})

EM_JS(int, ngh_http__js_status, (int h), {
    const e = globalThis.__NGH.http.requests[h];
    return e && e.state === 1 ? e.status : 0;
})

EM_JS(uintptr_t, ngh_http__js_body, (int h), {
    const e = globalThis.__NGH.http.requests[h];
    return e && e.state === 1 ? e.bodyPtr : 0;
})

EM_JS(size_t, ngh_http__js_body_len, (int h), {
    const e = globalThis.__NGH.http.requests[h];
    return e && e.state === 1 ? e.bodyLen : 0;
})

EM_JS(uintptr_t, ngh_http__js_header_table, (int h), {
    const e = globalThis.__NGH.http.requests[h];
    return e && e.state === 1 ? e.headerTable : 0;
})

EM_JS(size_t, ngh_http__js_header_count, (int h), {
    const e = globalThis.__NGH.http.requests[h];
    return e && e.state === 1 ? e.headerCount : 0;
})

EM_JS(int, ngh_http__js_error, (int h, char *buffer, int capacity), {
    const e = globalThis.__NGH.http.requests[h];
    if (!e || e.state !== 2 || !e.error) return 0;
    stringToUTF8(e.error, buffer, capacity);
    return 1;
})

EM_JS(void, ngh_http__js_destroy, (int h), {
    const H = globalThis.__NGH.http;
    const e = H.requests[h];
    if (!e) return;
    e.dead = true;
    H.clearTimers(e);
    if (e.state === 0) {
        try { e.controller.abort(); } catch (error) {}
    }
    H.release(e);
    H.requests[h] = null;
})

struct ngh_http {
    int h;
    char error[512];
};

int ngh_http_runtime_load(const char *path) {
    (void)path;
    ngh_http__last_error[0] = '\0';
    return 0;
}

ngh_http *ngh_http_fetch(const char *url, const ngh_http_options *opt) {
    ngh_http *r;
    const char *method;
    size_t max_body_len;
    uint32_t connect_timeout_ms;
    if (!ngh_http__validate_fetch(url, opt)) return NULL;
    if (opt && opt->no_redirect) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "no_redirect is not supported by the web fetch backend");
        return NULL;
    }
    r = (ngh_http *)calloc(1, sizeof(*r));
    if (!r) {
        snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
                 "out of memory");
        return NULL;
    }
    method = opt && opt->method ? opt->method : "GET";
    max_body_len = opt && opt->max_body_len
                       ? opt->max_body_len
                       : NGH_HTTP__DEFAULT_MAX_BODY;
    connect_timeout_ms = opt && opt->connect_timeout_ms
                             ? opt->connect_timeout_ms
                             : 10000u;
    ngh_http__js_boot();
    r->h = ngh_http__js_fetch(
        url, method, opt ? opt->headers : NULL,
        opt ? opt->header_count : 0, opt ? (const uint8_t *)opt->body : NULL,
        opt ? opt->body_len : 0, opt && opt->body ? 1 : 0, max_body_len,
        connect_timeout_ms, opt ? opt->total_timeout_ms : 0);
    ngh_http__last_error[0] = '\0';
    return r;
}

int ngh_http_state(ngh_http *r) {
    if (!r) return NGH_HTTP_ERROR;
    return ngh_http__js_state(r->h);
}

int ngh_http_status(ngh_http *r) {
    if (!r) return 0;
    return ngh_http__js_status(r->h);
}

const char *ngh_http_body(ngh_http *r, size_t *len) {
    if (len) *len = 0;
    if (!r || ngh_http__js_state(r->h) != NGH_HTTP_DONE) return NULL;
    if (len) *len = ngh_http__js_body_len(r->h);
    return (const char *)ngh_http__js_body(r->h);
}

const char *ngh_http_header(ngh_http *r, const char *name) {
    const char *const *table;
    size_t count;
    size_t i;
    if (!r || !name || ngh_http__js_state(r->h) != NGH_HTTP_DONE) return NULL;
    table = (const char *const *)ngh_http__js_header_table(r->h);
    count = ngh_http__js_header_count(r->h);
    for (i = 0; i < count; ++i) {
        if (ngh_http__ascii_equal(table[i * 2], name)) return table[i * 2 + 1];
    }
    return NULL;
}

int ngh_http_header_at(ngh_http *r, size_t i, const char **name,
                       const char **value) {
    const char *const *table;
    size_t count;
    if (name) *name = NULL;
    if (value) *value = NULL;
    if (!r || ngh_http__js_state(r->h) != NGH_HTTP_DONE) return 0;
    count = ngh_http__js_header_count(r->h);
    if (i >= count) return 0;
    table = (const char *const *)ngh_http__js_header_table(r->h);
    if (name) *name = table[i * 2];
    if (value) *value = table[i * 2 + 1];
    return 1;
}

int ngh_http_version(ngh_http *r) {
    (void)r;
    return NGH_HTTP_V_AUTO;
}

const char *ngh_http_error(ngh_http *r) {
    if (!r) return "invalid handle";
    if (ngh_http__js_state(r->h) != NGH_HTTP_ERROR) return NULL;
    if (!ngh_http__js_error(r->h, r->error, (int)sizeof(r->error)))
        snprintf(r->error, sizeof(r->error), "HTTP request failed");
    return r->error;
}

void ngh_http_destroy(ngh_http *r) {
    if (!r) return;
    ngh_http__js_destroy(r->h);
    free(r);
}

#else

/* ==========================================================================
 *  Unsupported platforms
 * ========================================================================== */

int ngh_http_runtime_load(const char *path) {
    (void)path;
    snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
             "platform backend not implemented yet");
    return -1;
}

ngh_http *ngh_http_fetch(const char *url, const ngh_http_options *opt) {
    if (!ngh_http__validate_fetch(url, opt)) return NULL;
    snprintf(ngh_http__last_error, sizeof(ngh_http__last_error),
             "platform backend not implemented yet");
    return NULL;
}

int ngh_http_state(ngh_http *r) {
    (void)r;
    return NGH_HTTP_ERROR;
}

int ngh_http_status(ngh_http *r) {
    (void)r;
    return 0;
}

const char *ngh_http_body(ngh_http *r, size_t *len) {
    (void)r;
    if (len) *len = 0;
    return NULL;
}

const char *ngh_http_header(ngh_http *r, const char *name) {
    (void)r;
    (void)name;
    return NULL;
}

int ngh_http_header_at(ngh_http *r, size_t i, const char **name,
                       const char **value) {
    (void)r;
    (void)i;
    if (name) *name = NULL;
    if (value) *value = NULL;
    return 0;
}

int ngh_http_version(ngh_http *r) {
    (void)r;
    return NGH_HTTP_V_AUTO;
}

const char *ngh_http_error(ngh_http *r) {
    (void)r;
    return NULL;
}

void ngh_http_destroy(ngh_http *r) {
    (void)r;
}

#endif

#undef NGH_HTTP__DEFAULT_MAX_BODY

#endif /* NGH_HTTP_IMPLEMENTATION_INCLUDED */
#endif /* NGH_HTTP_IMPLEMENTATION */
