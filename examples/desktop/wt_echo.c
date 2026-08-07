/* Echo round-trip over WebTransport: one datagram and one bidi stream.
 *
 * Usage: ngh_wt_echo <url> <cert_sha256_hex>
 *
 * Pair it with any WebTransport echo server; the hash is the SHA-256 of
 * the server's certificate in DER form (serverCertificateHashes model).
 */
#define NGH_WEBTRANSPORT_IMPLEMENTATION
#include "ngh_webtransport.h"

#include <stdio.h>
#include <stdlib.h>
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

#define MSG "echo me over webtransport"

static int hex_decode(const char *hex, uint8_t *out, size_t out_len) {
    size_t i;
    if (strlen(hex) != out_len * 2) return -1;
    for (i = 0; i < out_len; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

int main(int argc, char **argv) {
    uint8_t hash[32];
    ngh_wt_options opt;
    ngh_wt *wt;
    ngh_wt_stream *s;
    int dgram_ok = 0, stream_ok = 0;
    char acc[256];
    size_t acc_len = 0;
    int i;

    if (argc < 3 || hex_decode(argv[2], hash, sizeof(hash)) != 0) {
        fprintf(stderr, "usage: %s <url> <cert_sha256_hex>\n", argv[0]);
        return 2;
    }
    if (ngh_wt_runtime_load(NULL) != 0) {
        fprintf(stderr, "backend not found: %s\n", ngh_wt_last_error());
        return 1;
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
        ngh_wt_destroy(wt);
        return 1;
    }
    printf("ready, dgram_max=%zu\n", ngh_wt_dgram_max(wt));

    ngh_wt_dgram_send(wt, MSG, strlen(MSG));
    s = ngh_wt_stream_open(wt, NGH_WT_BIDI);
    ngh_wt_stream_write(s, MSG, strlen(MSG));
    ngh_wt_stream_finish(s);

    for (i = 0; i < 500 && !(dgram_ok && stream_ok); i++) {
        uint8_t buf[2048];
        int n = ngh_wt_dgram_recv(wt, buf, sizeof(buf));
        if (n > 0 && (size_t)n == strlen(MSG) && !memcmp(buf, MSG, (size_t)n))
            dgram_ok = 1;
        n = ngh_wt_stream_read(s, buf, sizeof(buf));
        if (n > 0 && acc_len + (size_t)n < sizeof(acc)) {
            memcpy(acc + acc_len, buf, (size_t)n);
            acc_len += (size_t)n;
            if (acc_len == strlen(MSG) && !memcmp(acc, MSG, acc_len))
                stream_ok = 1;
        }
        sleep_ms(10);
    }

    ngh_wt_close(wt, 0, "done");
    ngh_wt_stream_destroy(s);
    ngh_wt_destroy(wt);

    printf("%s (dgram=%d stream=%d)\n",
           dgram_ok && stream_ok ? "PASS" : "FAIL", dgram_ok, stream_ok);
    return dgram_ok && stream_ok ? 0 : 1;
}
