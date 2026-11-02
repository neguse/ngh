/* Stream flow control against a live echo server (run by wt_interop.py).
 *
 * Usage: test_wt_flow <url> <cert_sha256_hex>
 *
 * A stream reader that stops polling must push back on the sender, never
 * buffer without bound. The client writes into a bidi stream for two
 * seconds without reading: the echo server's replies fill our receive
 * window, the server stops reading, its window fills, and our writes must
 * stall at a bounded total. Then it reads everything back and checks every
 * byte, so a credit re-grant that never comes shows up as a stall.
 */
#define NGH_WEBTRANSPORT_IMPLEMENTATION
#include "ngh_webtransport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
static double now_ms(void) { return (double)GetTickCount64(); }
#else
static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#endif

/* Without backpressure a loopback echo takes hundreds of megabytes in two
 * seconds; with it, the total is our send queue plus the windows and
 * buffers on both sides -- a few megabytes. */
#define STALL_BOUND (16u << 20)

static unsigned char pattern(size_t i) { return (unsigned char)(i * 7 % 251); }

static int hex_decode(const char *hex, uint8_t *out, size_t out_len) {
    size_t i;
    if (strlen(hex) != out_len * 2) return -1;
    for (i = 0; i < out_len * 2; i++) {
        char c = hex[i];
        int v = (c >= '0' && c <= '9')   ? c - '0'
                : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                         : -1;
        if (v < 0) return -1;
        if (i % 2 == 0)
            out[i / 2] = (uint8_t)(v << 4);
        else
            out[i / 2] |= (uint8_t)v;
    }
    return 0;
}

int main(int argc, char **argv) {
    static unsigned char chunk[64 * 1024];
    static unsigned char buf[64 * 1024];
    uint8_t hash[32];
    ngh_wt_options opt;
    ngh_wt *wt;
    ngh_wt_stream *s;
    size_t sent = 0, got = 0, bad = 0;
    double start;
    int i, fin = 0, ok;

    if (argc < 3 || hex_decode(argv[2], hash, sizeof(hash)) != 0) {
        fprintf(stderr, "usage: %s <url> <cert_sha256_hex>\n", argv[0]);
        return 2;
    }
    opt = ngh_wt_options_default();
    opt.cert_hashes = hash;
    opt.cert_hash_count = 1;
    wt = ngh_wt_connect(argv[1], &opt);
    if (!wt) {
        fprintf(stderr, "connect: %s\n", ngh_wt_last_error());
        return 1;
    }
    for (i = 0; i < 500 && ngh_wt_state(wt) == NGH_WT_PENDING; i++)
        sleep_ms(10);
    if (ngh_wt_state(wt) != NGH_WT_READY) {
        fprintf(stderr, "session failed: %s\n",
                ngh_wt_error(wt) ? ngh_wt_error(wt) : "-");
        return 1;
    }
    s = ngh_wt_stream_open(wt, NGH_WT_BIDI);
    if (!s) {
        fprintf(stderr, "stream_open failed\n");
        return 1;
    }

    /* Phase 1: write without reading. */
    start = now_ms();
    while (now_ms() - start < 2000.0 && sent < 4 * STALL_BOUND) {
        size_t k;
        int n;
        for (k = 0; k < sizeof(chunk); k++) chunk[k] = pattern(sent + k);
        n = ngh_wt_stream_write(s, chunk, sizeof(chunk));
        if (n < 0) {
            fprintf(stderr, "write failed: %d\n", n);
            return 1;
        }
        sent += (size_t)n;
        if (n == 0) sleep_ms(5);
    }
    printf("unread phase: writes stalled at %zu bytes (bound %u)\n", sent,
           STALL_BOUND);

    /* Phase 2: read everything back; FIN once it is all queued. */
    ngh_wt_stream_finish(s);
    start = now_ms();
    while (!fin && now_ms() - start < 20000.0) {
        int n = ngh_wt_stream_read(s, buf, sizeof(buf));
        if (n > 0) {
            int k;
            for (k = 0; k < n; k++)
                if (buf[k] != pattern(got + (size_t)k)) bad++;
            got += (size_t)n;
        } else if (n == NGH_WT_FIN) {
            fin = 1;
        } else if (n < 0) {
            fprintf(stderr, "read failed: %d\n", n);
            break;
        } else {
            sleep_ms(1);
        }
    }
    printf("drain phase: read %zu of %zu bytes back, %zu corrupt, fin=%d, "
           "%.0f ms\n",
           got, sent, bad, fin, now_ms() - start);

    ok = sent > 0 && sent <= STALL_BOUND && fin && got == sent && bad == 0;
    ngh_wt_close(wt, 0, "done");
    ngh_wt_stream_destroy(s);
    ngh_wt_destroy(wt);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
