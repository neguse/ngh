/* Wiring tests for ngh_webtransport.h that need no server: argument
 * checks, runtime loading, and the failure path of a connect to a dead
 * port. Exits 77 (the automake skip convention) when the backend library
 * cannot be found. */
#define NGH_WEBTRANSPORT_IMPLEMENTATION
#include "ngh_webtransport.h"

#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
#if defined(__linux__)
#include <dirent.h>
#endif
static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

#include "ngh_test.h"

static const uint8_t dummy_hash[32] = {0};

static void test_args(void) {
    ngh_wt_options opt = ngh_wt_options_default();
    NGH_CHECK(opt.cert_hashes == NULL);
    NGH_CHECK(ngh_wt_connect(NULL, NULL) == NULL);
    NGH_CHECK(ngh_wt_last_error() != NULL);
    NGH_CHECK(ngh_wt_state(NULL) == NGH_WT_ERROR);
    NGH_CHECK(ngh_wt_dgram_send(NULL, "x", 1) == NGH_WT_ERR);
    NGH_CHECK(ngh_wt_dgram_recv(NULL, NULL, 0) == NGH_WT_ERR);
    NGH_CHECK(ngh_wt_dgram_max(NULL) == 0);
    NGH_CHECK(ngh_wt_stream_open(NULL, NGH_WT_BIDI) == NULL);
    NGH_CHECK(ngh_wt_stream_accept(NULL) == NULL);
    NGH_CHECK(ngh_wt_stream_write(NULL, "x", 1) == NGH_WT_ERR);
    NGH_CHECK(ngh_wt_stream_read(NULL, NULL, 0) == NGH_WT_ERR);
}

static void test_connect_failure(void) {
    /* Nothing listens on this port; the session must fail on its own
     * within the connect timeout, without blocking any call. */
    ngh_wt_options opt = ngh_wt_options_default();
    ngh_wt *wt;
    int i;
    opt.cert_hashes = dummy_hash;
    opt.cert_hash_count = 1;
    opt.connect_timeout_ms = 3000;
    wt = ngh_wt_connect("https://127.0.0.1:9/", &opt);
    NGH_CHECK(wt != NULL);
    for (i = 0; i < 800 && ngh_wt_state(wt) == NGH_WT_PENDING; i++)
        sleep_ms(10);
    NGH_CHECK(ngh_wt_state(wt) == NGH_WT_ERROR);
    NGH_CHECK(ngh_wt_error(wt) != NULL);
    ngh_wt_destroy(wt);
}

static void test_missing_cert_hash(void) {
    /* Desktop: CA validation is unimplemented, so connect without pins
     * must fail loudly, not silently skip verification. */
    ngh_wt *wt = ngh_wt_connect("https://127.0.0.1:9/", NULL);
    NGH_CHECK(wt == NULL);
    NGH_CHECK(ngh_wt_last_error() != NULL);
}

static void test_url_parsing(void) {
    static const char *good[] = {
        "https://[::1]:9/",      /* IPv6 literal, browser syntax */
        "https://[::1]/",        /* default port */
        "https://127.0.0.1:9",   /* no path */
        "https://127.0.0.1:9?x=1", /* query without a path */
        "https://example.invalid:9/a/b?c#d",
    };
    static const char *bad[] = {
        "http://127.0.0.1:9/",   /* not https */
        "https://::1:9/",        /* unbracketed IPv6 */
        "https://[::1:9/",       /* unclosed bracket */
        "https://[::1]x/",       /* junk after the bracket */
        "https://127.0.0.1:/",   /* empty port */
        "https://127.0.0.1:12ab/", "https://127.0.0.1:70000/",
        "https://127.0.0.1:0/",  "https://:443/", "https://u@host/",
    };
    ngh_wt_options opt = ngh_wt_options_default();
    size_t i;
    opt.cert_hashes = dummy_hash;
    opt.cert_hash_count = 1;
    opt.connect_timeout_ms = 200;
    for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        ngh_wt *wt = ngh_wt_connect(good[i], &opt);
        if (!wt) printf("    rejected %s: %s\n", good[i], ngh_wt_last_error());
        NGH_CHECK(wt != NULL);
        ngh_wt_destroy(wt);
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ngh_wt *wt = ngh_wt_connect(bad[i], &opt);
        if (wt) printf("    accepted %s\n", bad[i]);
        NGH_CHECK(wt == NULL);
        NGH_CHECK(ngh_wt_last_error() != NULL &&
                  strstr(ngh_wt_last_error(), "malformed") != NULL);
        ngh_wt_destroy(wt);
    }
}

static int thread_count(void) {
#if defined(__linux__)
    /* one /proc/self/task entry per thread, plus "." and ".." */
    int n = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) return -1;
    while (readdir(d)) n++;
    closedir(d);
    return n - 2;
#else
    return -1;
#endif
}

static void test_unload(void) {
    ngh_wt_options opt = ngh_wt_options_default();
    ngh_wt *wt;
    int with_backend, after;
    opt.cert_hashes = dummy_hash;
    opt.cert_hash_count = 1;
    opt.connect_timeout_ms = 200;

    /* the backend context (and its network thread) exists after a connect */
    wt = ngh_wt_connect("https://127.0.0.1:9/", &opt);
    NGH_CHECK(wt != NULL);
    ngh_wt_destroy(wt);
    with_backend = thread_count();

    ngh_wt_runtime_unload();
    ngh_wt_runtime_unload(); /* twice is harmless */
    after = thread_count();
    if (with_backend >= 0) {
        printf("    threads: %d with the backend, %d after unload\n",
               with_backend, after);
        NGH_CHECK(after < with_backend);
    }

    /* and everything starts over on the next use */
    NGH_CHECK(ngh_wt_runtime_load(NULL) == 0);
}

int main(void) {
    test_args();

    if (ngh_wt_runtime_load(NULL) != 0) {
        printf("SKIP: %s\n", ngh_wt_last_error());
        return 77;
    }
    test_missing_cert_hash();
    test_url_parsing();
    test_connect_failure();
    test_unload();
    test_connect_failure(); /* works again after a reload */

    return ngh_test_report("test_wt");
}
