/* Name resolution must never stall the polling API. Run with
 * slow_getaddrinfo preloaded and NGH_TEST_DNS_DELAY_MS set: every lookup
 * then takes that long, like a slow DNS server. Linux only (LD_PRELOAD).
 * Exits 77 when the backend library cannot be found. */
#define NGH_WEBTRANSPORT_IMPLEMENTATION
#include "ngh_webtransport.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ngh_test.h"

static const uint8_t dummy_hash[32] = {0};

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static ngh_wt *connect_localhost(uint32_t timeout_ms) {
    ngh_wt_options opt = ngh_wt_options_default();
    opt.cert_hashes = dummy_hash;
    opt.cert_hash_count = 1;
    opt.connect_timeout_ms = timeout_ms;
    /* A name, so the backend has to resolve it; nothing listens on 9. */
    return ngh_wt_connect("https://localhost:9/", &opt);
}

static void test_polls_do_not_wait_for_dns(long delay) {
    ngh_wt *wt;
    double start, worst = 0.0;

    NGH_TEST_CASE("state polls do not wait for name resolution");
    wt = connect_localhost(10000);
    NGH_CHECK(wt != NULL);
    if (!wt) return;
    start = now_ms();
    while (now_ms() - start < (double)delay * 1.5) {
        double t0 = now_ms(), dt;
        (void)ngh_wt_state(wt);
        (void)ngh_wt_dgram_max(wt);
        dt = now_ms() - t0;
        if (dt > worst) worst = dt;
        sleep_ms(5);
    }
    printf("    worst poll %.1f ms during a %ld ms lookup\n", worst, delay);
    NGH_CHECK(worst < 100.0);
    ngh_wt_destroy(wt);
    NGH_TEST_DONE();
}

static void test_timeout_covers_dns(long delay) {
    ngh_wt *wt;
    double start, took;
    const uint32_t timeout = 300;

    NGH_TEST_CASE("connect timeout covers name resolution");
    wt = connect_localhost(timeout);
    NGH_CHECK(wt != NULL);
    if (!wt) return;
    start = now_ms();
    while (ngh_wt_state(wt) == NGH_WT_PENDING && now_ms() - start < 5000)
        sleep_ms(5);
    took = now_ms() - start;
    printf("    gave up after %.0f ms (timeout %u, lookup %ld)\n", took,
           (unsigned)timeout, delay);
    NGH_CHECK_EQ_INT(ngh_wt_state(wt), NGH_WT_ERROR);
    NGH_CHECK(took < (double)delay);
    ngh_wt_destroy(wt);
    NGH_TEST_DONE();
}

int main(void) {
    const char *env = getenv("NGH_TEST_DNS_DELAY_MS");
    long delay = env ? atol(env) : 0;

    if (delay < 500) {
        printf("SKIP: run with slow_getaddrinfo preloaded and "
               "NGH_TEST_DNS_DELAY_MS >= 500\n");
        return 77;
    }
    if (ngh_wt_runtime_load(NULL) != 0) {
        printf("SKIP: %s\n", ngh_wt_last_error());
        return 77;
    }
    /* The first connect creates the backend context; do that outside the
     * timed cases. */
    {
        ngh_wt *warm = connect_localhost(100);
        ngh_wt_destroy(warm);
    }
    test_polls_do_not_wait_for_dns(delay);
    test_timeout_covers_dns(delay);
    return ngh_test_report("test_wt_dns");
}
