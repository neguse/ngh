/* wt_backend.h -- the ABI between ngh_webtransport.h and the prebuilt
 * desktop backend (libngh_wt_backend.{so,dll,dylib}).
 *
 * This is a private contract, not a user API. ngh_webtransport.h resolves
 * exactly these symbols at run time; the backend implements them on top of a
 * statically linked picoquic. picoquic's own API never crosses this
 * boundary, so bumping the vendored picoquic does not bump this ABI.
 *
 * Rules of the contract:
 * - Everything is poll-based. No callback ever crosses the ABI.
 * - The backend owns a network thread per context. ABI calls never block on
 *   the network; they only move bytes in and out of internal queues.
 * - Calls for one context must come from a single application thread at a
 *   time (external synchronization is the caller's job); the backend
 *   synchronizes internally against its network thread.
 * - Handles are opaque. A handle stays valid until its *_destroy call, even
 *   after errors -- destroying is always the caller's act.
 * - NGH_WTB_ABI_VERSION changes on any breaking change; the loader refuses
 *   a mismatch. Additive change = new function + version bump; the loader
 *   treats missing new symbols as "old backend".
 */
#ifndef NGH_WT_BACKEND_H
#define NGH_WT_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(NGH_WTB_BUILD)
#define NGH_WTB_API __declspec(dllexport)
#elif defined(_WIN32)
#define NGH_WTB_API __declspec(dllimport)
#else
#define NGH_WTB_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define NGH_WTB_ABI_VERSION 1u

/* Session / stream states. */
#define NGH_WTB_PENDING 0  /* connect in progress */
#define NGH_WTB_READY   1  /* usable */
#define NGH_WTB_CLOSED  2  /* closed cleanly (by either side) */
#define NGH_WTB_ERROR   3  /* failed; see ngh_wtb_session_error() */

/* Negative return codes. Positive returns are byte counts; 0 means
 * "nothing available right now" unless documented otherwise. */
#define NGH_WTB_FIN       (-1) /* stream: peer finished, all data consumed */
#define NGH_WTB_ERR       (-2) /* generic failure / bad handle state */
#define NGH_WTB_ERR_SIZE  (-3) /* dgram_recv: buffer smaller than datagram
                                * (the datagram is dropped, per datagram
                                * semantics) */
#define NGH_WTB_ERR_RESET (-4) /* stream: peer reset the stream */

typedef struct ngh_wtb_ctx ngh_wtb_ctx;
typedef struct ngh_wtb_session ngh_wtb_session;
typedef struct ngh_wtb_stream ngh_wtb_stream;

typedef struct ngh_wtb_options {
    size_t struct_size;         /* = sizeof(ngh_wtb_options); versioning aid */
    /* Server certificate pinning, same model as the browser's
     * serverCertificateHashes: SHA-256 over the end-entity certificate in
     * DER form. `cert_hashes` is cert_hash_count concatenated 32-byte
     * hashes; a presented certificate matching any hash passes. NULL/0
     * selects normal CA verification (Linux: system store; Windows CA
     * verification is a later work item -- pinning always works). */
    const uint8_t* cert_hashes;
    size_t cert_hash_count;
    uint32_t idle_timeout_ms;   /* 0 = backend default */
    uint32_t connect_timeout_ms;/* 0 = backend default; PENDING turns ERROR
                                 * when it expires */
} ngh_wtb_options;

/* ABI version of the loaded backend. The loader calls this first and
 * refuses anything != NGH_WTB_ABI_VERSION. */
NGH_WTB_API uint32_t ngh_wtb_abi_version(void);

/* Context: owns the network thread and any number of sessions.
 * ctx_destroy tears down every session still alive in it. */
NGH_WTB_API ngh_wtb_ctx* ngh_wtb_ctx_create(void);
NGH_WTB_API void ngh_wtb_ctx_destroy(ngh_wtb_ctx* ctx);
/* Explains the most recent NULL return from ngh_wtb_connect. Static
 * lifetime within the context. */
NGH_WTB_API const char* ngh_wtb_ctx_error(ngh_wtb_ctx* ctx);

/* Open a WebTransport session to `url` ("https://host[:port]/path",
 * default port 443). Returns immediately; poll ngh_wtb_session_state until
 * it leaves NGH_WTB_PENDING. `opt` may be NULL for defaults. NULL return =
 * malformed input or resource failure, see ngh_wtb_ctx_error. */
NGH_WTB_API ngh_wtb_session* ngh_wtb_connect(ngh_wtb_ctx* ctx,
    const char* url, const ngh_wtb_options* opt);
NGH_WTB_API int ngh_wtb_session_state(ngh_wtb_session* s);
/* Non-NULL exactly when state is NGH_WTB_ERROR or CLOSED-with-code;
 * static lifetime within the session. */
NGH_WTB_API const char* ngh_wtb_session_error(ngh_wtb_session* s);
/* Graceful close (CLOSE_WEBTRANSPORT_SESSION capsule, then QUIC close).
 * `reason` may be NULL. Idempotent. */
NGH_WTB_API void ngh_wtb_session_close(ngh_wtb_session* s,
    uint32_t error_code, const char* reason);
NGH_WTB_API void ngh_wtb_session_destroy(ngh_wtb_session* s);

/* Datagrams. Send queues one datagram (0 on accept, NGH_WTB_ERR when the
 * session is not READY or len > dgram_max). Receive copies the next whole
 * datagram into buf and returns its length, 0 if none pending. dgram_max
 * reports the current largest sendable payload (path MTU dependent; 0
 * while PENDING). Receive queueing is bounded and drops oldest. */
NGH_WTB_API int ngh_wtb_dgram_send(ngh_wtb_session* s,
    const uint8_t* data, size_t len);
NGH_WTB_API int ngh_wtb_dgram_recv(ngh_wtb_session* s,
    uint8_t* buf, size_t cap);
NGH_WTB_API size_t ngh_wtb_dgram_max(ngh_wtb_session* s);

/* Streams. open() creates a local stream (bidi != 0 for bidirectional) on
 * a READY session; NULL on failure. accept() pops the next peer-initiated
 * stream, NULL if none pending. Peer streams not accepted are held in a
 * bounded queue backed by QUIC flow control. */
NGH_WTB_API ngh_wtb_stream* ngh_wtb_stream_open(ngh_wtb_session* s, int bidi);
NGH_WTB_API ngh_wtb_stream* ngh_wtb_stream_accept(ngh_wtb_session* s);
NGH_WTB_API int ngh_wtb_stream_bidi(ngh_wtb_stream* st);

/* Write queues up to len bytes and returns how many were accepted (0 when
 * the send window is full -- retry next poll; that is the backpressure
 * signal). finish() marks FIN after all queued bytes; write after finish is
 * NGH_WTB_ERR. Read copies queued bytes and returns the count, 0 if none,
 * NGH_WTB_FIN once the peer finished and everything was consumed,
 * NGH_WTB_ERR_RESET if the peer reset. reset() abandons the local side with
 * an application error code. */
NGH_WTB_API int ngh_wtb_stream_write(ngh_wtb_stream* st,
    const uint8_t* data, size_t len);
NGH_WTB_API int ngh_wtb_stream_finish(ngh_wtb_stream* st);
NGH_WTB_API int ngh_wtb_stream_read(ngh_wtb_stream* st,
    uint8_t* buf, size_t cap);
NGH_WTB_API void ngh_wtb_stream_reset(ngh_wtb_stream* st, uint32_t error_code);
NGH_WTB_API void ngh_wtb_stream_destroy(ngh_wtb_stream* st);

#ifdef __cplusplus
}
#endif
#endif /* NGH_WT_BACKEND_H */
