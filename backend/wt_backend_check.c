/* wt_backend_check.c -- manual interop check for the WebTransport backend.
 *
 * Usage: ngh_wt_backend_check <url> <cert_sha256_hex>
 *
 * Connects, echoes one datagram and one bidi stream against an echo server
 * (e.g. the wtransport wt-echo tool), prints PASS/FAIL, exits 0/1. Not a
 * ctest unit because it needs a live server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <unistd.h>
static void sleep_ms(unsigned ms) { usleep(ms * 1000); }
#endif

#include "wt_backend.h"

#define STREAM_MSG "hello stream over the backend abi"
#define DGRAM_MSG  "hello datagram over the backend abi"

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int hex_decode(const char* hex, uint8_t* out, size_t out_len)
{
    size_t i;
    if (strlen(hex) != out_len * 2) {
        return -1;
    }
    for (i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

int main(int argc, char** argv)
{
    const char* url = (argc > 1) ? argv[1] : "https://localhost:4433/";
    uint8_t hash[32];
    ngh_wtb_options opt;
    ngh_wtb_ctx* ctx;
    ngh_wtb_session* s;
    ngh_wtb_stream* st;
    int dgram_ok = 0, stream_ok = 0;
    int i;

    if (argc < 3 || hex_decode(argv[2], hash, sizeof(hash)) != 0) {
        fprintf(stderr, "usage: %s <url> <cert_sha256_hex>\n", argv[0]);
        return 2;
    }

    ctx = ngh_wtb_ctx_create();
    if (ctx == NULL) {
        fprintf(stderr, "ctx_create failed\n");
        return 1;
    }
    printf("abi version: %u\n", ngh_wtb_abi_version());

    memset(&opt, 0, sizeof(opt));
    opt.struct_size = sizeof(opt);
    opt.cert_hashes = hash;
    opt.cert_hash_count = 1;
    s = ngh_wtb_connect(ctx, url, &opt);
    if (s == NULL) {
        fprintf(stderr, "connect failed: %s\n", ngh_wtb_ctx_error(ctx));
        ngh_wtb_ctx_destroy(ctx);
        return 1;
    }

    for (i = 0; i < 500 && ngh_wtb_session_state(s) == NGH_WTB_PENDING; i++) {
        sleep_ms(10);
    }
    if (ngh_wtb_session_state(s) != NGH_WTB_READY) {
        fprintf(stderr, "session not ready: state=%d err=%s\n",
            ngh_wtb_session_state(s),
            ngh_wtb_session_error(s) ? ngh_wtb_session_error(s) : "-");
        ngh_wtb_session_destroy(s);
        ngh_wtb_ctx_destroy(ctx);
        return 1;
    }
    printf("session ready, dgram_max=%zu\n", ngh_wtb_dgram_max(s));

    if (ngh_wtb_dgram_send(s, (const uint8_t*)DGRAM_MSG,
            strlen(DGRAM_MSG)) != 0) {
        fprintf(stderr, "dgram_send failed\n");
    }
    st = ngh_wtb_stream_open(s, 1);
    if (st == NULL) {
        fprintf(stderr, "stream_open failed\n");
    }
    else {
        int n = ngh_wtb_stream_write(st, (const uint8_t*)STREAM_MSG,
            strlen(STREAM_MSG));
        printf("stream accepted %d bytes\n", n);
        ngh_wtb_stream_finish(st);
    }

    for (i = 0; i < 500 && !(dgram_ok && stream_ok); i++) {
        uint8_t buf[2048];
        int n = ngh_wtb_dgram_recv(s, buf, sizeof(buf));
        if (n > 0) {
            printf("dgram echo: %d bytes\n", n);
            if ((size_t)n == strlen(DGRAM_MSG) &&
                memcmp(buf, DGRAM_MSG, (size_t)n) == 0) {
                dgram_ok = 1;
            }
        }
        if (st != NULL) {
            static char acc[256];
            static size_t acc_len = 0;
            n = ngh_wtb_stream_read(st, buf, sizeof(buf));
            if (n > 0 && acc_len + (size_t)n < sizeof(acc)) {
                memcpy(acc + acc_len, buf, (size_t)n);
                acc_len += (size_t)n;
                printf("stream echo: %zu/%zu bytes\n",
                    acc_len, strlen(STREAM_MSG));
                if (acc_len == strlen(STREAM_MSG) &&
                    memcmp(acc, STREAM_MSG, acc_len) == 0) {
                    stream_ok = 1;
                }
            }
        }
        sleep_ms(10);
    }

    ngh_wtb_session_close(s, 0, "done");
    for (i = 0; i < 100 && ngh_wtb_session_state(s) == NGH_WTB_READY; i++) {
        sleep_ms(10);
    }
    if (st != NULL) {
        ngh_wtb_stream_destroy(st);
    }
    ngh_wtb_session_destroy(s);

    /* Pin isolation: a second session pinning a different hash must not
     * ride on the first session's pin. */
    {
        ngh_wtb_session* s2;
        int isolated = 0;
        hash[0] ^= 0xff;
        s2 = ngh_wtb_connect(ctx, url, &opt);
        if (s2 == NULL) {
            fprintf(stderr, "pin-isolation connect failed to start\n");
        }
        else {
            for (i = 0; i < 500 && ngh_wtb_session_state(s2) == NGH_WTB_PENDING;
                 i++) {
                sleep_ms(10);
            }
            isolated = (ngh_wtb_session_state(s2) != NGH_WTB_READY);
            printf("pin isolation: %s\n", isolated ? "OK (rejected)" : "BROKEN");
            ngh_wtb_session_destroy(s2);
        }
        if (!isolated) {
            dgram_ok = stream_ok = 0; /* fail the run */
        }
    }
    ngh_wtb_ctx_destroy(ctx);

    if (dgram_ok && stream_ok) {
        printf("CHECK RESULT: PASS\n");
        return 0;
    }
    printf("CHECK RESULT: FAIL (dgram=%d stream=%d)\n", dgram_ok, stream_ok);
    return 1;
}
