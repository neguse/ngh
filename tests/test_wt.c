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

int main(void) {
    test_args();

    if (ngh_wt_runtime_load(NULL) != 0) {
        printf("SKIP: %s\n", ngh_wt_last_error());
        return 77;
    }
    test_missing_cert_hash();
    test_connect_failure();

    return ngh_test_report("test_wt");
}
