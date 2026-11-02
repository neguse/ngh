/* Stream teardown against a live echo server (run by wt_interop.py).
 *
 * Usage: test_wt_teardown <url> <cert_sha256_hex>
 *
 * Destroying a stream handle has to end the stream on the wire, as the
 * browser backend does: queued bytes and a requested FIN still go out,
 * otherwise the peer sees a reset. The wt-echo server reports how each bidi
 * stream ended as a datagram ("stream-fin <bytes>", "stream-reset <code>
 * <bytes>"), which is what this checks. The last case resets a stream,
 * destroys it and lets the server's side finish afterwards: that event
 * reaches a stream whose handle is gone and must not touch it.
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
#else
static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static int g_failures = 0;

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

/* Waits for the server's report of a stream's end and compares it. */
static void expect_report(ngh_wt *wt, const char *name, const char *want) {
    char got[128];
    int i;
    got[0] = 0;
    for (i = 0; i < 300; i++) {
        uint8_t buf[256];
        int n = ngh_wt_dgram_recv(wt, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            if (strncmp((const char *)buf, "stream-", 7) == 0) {
                snprintf(got, sizeof(got), "%.120s", (const char *)buf);
                break;
            }
        } else {
            sleep_ms(10);
        }
    }
    if (strcmp(got, want) == 0) {
        printf("  %-44s ok (%s)\n", name, got);
    } else {
        printf("  %-44s FAIL: server saw \"%s\", want \"%s\"\n", name,
               got[0] ? got : "(nothing)", want);
        g_failures++;
    }
}

/* Writes `len` bytes and reads the echo of all of them back. */
static void write_and_wait_echo(ngh_wt_stream *s, size_t len) {
    static uint8_t data[64];
    uint8_t buf[256];
    size_t got = 0;
    int i;
    memset(data, 'x', sizeof(data));
    ngh_wt_stream_write(s, data, len);
    for (i = 0; i < 300 && got < len; i++) {
        int n = ngh_wt_stream_read(s, buf, sizeof(buf));
        if (n > 0)
            got += (size_t)n;
        else
            sleep_ms(10);
    }
}

int main(int argc, char **argv) {
    static uint8_t big[200 * 1024];
    uint8_t hash[32];
    ngh_wt_options opt;
    ngh_wt *wt;
    ngh_wt_stream *s;
    int i;

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
    printf("test_wt_teardown\n");

    /* finish() then destroy at once: every queued byte and the FIN must
     * still reach the peer. */
    s = ngh_wt_stream_open(wt, NGH_WT_BIDI);
    memset(big, 'y', sizeof(big));
    if (ngh_wt_stream_write(s, big, sizeof(big)) != (int)sizeof(big))
        printf("  (short write)\n");
    ngh_wt_stream_finish(s);
    ngh_wt_stream_destroy(s);
    expect_report(wt, "destroy after finish flushes, then FIN",
                  "stream-fin 204800");

    /* destroy without finish(): the peer must see a reset, not a stream
     * left open forever. */
    s = ngh_wt_stream_open(wt, NGH_WT_BIDI);
    write_and_wait_echo(s, 10);
    ngh_wt_stream_destroy(s);
    expect_report(wt, "destroy without finish resets", "stream-reset 0 10");

    /* reset, then destroy; the server's FIN arrives after the handle is
     * gone. */
    s = ngh_wt_stream_open(wt, NGH_WT_BIDI);
    write_and_wait_echo(s, 10);
    ngh_wt_stream_reset(s, 7);
    ngh_wt_stream_destroy(s);
    expect_report(wt, "reset then destroy keeps the reset code",
                  "stream-reset 7 10");

    /* The session must still work after all of that. */
    s = ngh_wt_stream_open(wt, NGH_WT_BIDI);
    write_and_wait_echo(s, 10);
    ngh_wt_stream_finish(s);
    for (i = 0; i < 300; i++) {
        uint8_t buf[64];
        int n = ngh_wt_stream_read(s, buf, sizeof(buf));
        if (n == NGH_WT_FIN) break;
        if (n == 0) sleep_ms(10);
    }
    ngh_wt_stream_destroy(s);
    expect_report(wt, "session still healthy afterwards", "stream-fin 10");

    ngh_wt_close(wt, 0, "done");
    ngh_wt_destroy(wt);
    printf("test_wt_teardown: %s\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
