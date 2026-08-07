/* wt_backend.c -- implements wt_backend.h on a statically linked picoquic.
 *
 * Threading model: one network thread per context runs picoquic's packet
 * loop; it is the only thread that touches picoquic/h3zero objects. The
 * application thread only moves bytes in and out of queues under ctx->lock
 * (recursive: h3zero teardown fires callbacks re-entering the lock) and
 * wakes the network thread. Everything the network thread must do on the
 * application's behalf travels as an op.
 */
#define NGH_WTB_BUILD
#include "wt_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#define strdup _strdup
#endif

#include <picotls.h>
#include <picoquic.h>
#include <picoquic_utils.h>
#include <picosocks.h>
#include <picoquic_packet_loop.h>
#include <h3zero.h>
#include <h3zero_common.h>
#include <pico_webtransport.h>

#define WTB_IS_BIDIR_STREAM_ID(id) (((id) & 2) == 0)

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

/* The only thread primitive this file needs is a recursive mutex; the
 * network thread itself is picoquic's. */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef CRITICAL_SECTION wtb_mutex_t; /* recursive by construction */
static void wtb_mutex_init(wtb_mutex_t* m) { InitializeCriticalSection(m); }
static void wtb_mutex_destroy(wtb_mutex_t* m) { DeleteCriticalSection(m); }
static void wtb_mutex_lock(wtb_mutex_t* m) { EnterCriticalSection(m); }
static void wtb_mutex_unlock(wtb_mutex_t* m) { LeaveCriticalSection(m); }
#else
#include <pthread.h>
typedef pthread_mutex_t wtb_mutex_t;
static void wtb_mutex_init(wtb_mutex_t* m) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
}
static void wtb_mutex_destroy(wtb_mutex_t* m) { pthread_mutex_destroy(m); }
static void wtb_mutex_lock(wtb_mutex_t* m) { pthread_mutex_lock(m); }
static void wtb_mutex_unlock(wtb_mutex_t* m) { pthread_mutex_unlock(m); }
#endif

#define WTB_STREAM_SENDQ_MAX (1u << 20)
#define WTB_DGRAM_QUEUE_MAX 64
#define WTB_ACCEPT_QUEUE_MAX 64
#define WTB_CONNECT_TIMEOUT_MS_DEFAULT 10000
/* Reported before the path is measured; QUIC guarantees at least a 1200
 * byte UDP payload, minus QUIC + H3 datagram overhead. */
#define WTB_DGRAM_MAX_SAFE 1150

/* ------------------------------------------------------------- queues -- */

typedef struct wtb_chunk {
    struct wtb_chunk* next;
    size_t len;
    size_t off;
    uint8_t data[];
} wtb_chunk;

typedef struct wtb_queue {
    wtb_chunk* head;
    wtb_chunk* tail;
    size_t bytes;
    size_t count;
} wtb_queue;

static int wtb_queue_push(wtb_queue* q, const uint8_t* data, size_t len)
{
    wtb_chunk* c = (wtb_chunk*)malloc(sizeof(wtb_chunk) + len);
    if (c == NULL) {
        return -1;
    }
    c->next = NULL;
    c->len = len;
    c->off = 0;
    if (len > 0) {
        memcpy(c->data, data, len);
    }
    if (q->tail == NULL) {
        q->head = q->tail = c;
    }
    else {
        q->tail->next = c;
        q->tail = c;
    }
    q->bytes += len;
    q->count += 1;
    return 0;
}

static void wtb_queue_pop(wtb_queue* q)
{
    wtb_chunk* c = q->head;
    if (c != NULL) {
        q->head = c->next;
        if (q->head == NULL) {
            q->tail = NULL;
        }
        q->bytes -= c->len - c->off;
        q->count -= 1;
        free(c);
    }
}

/* Copy up to cap bytes out of the queue, freeing drained chunks. */
static size_t wtb_queue_read(wtb_queue* q, uint8_t* buf, size_t cap)
{
    size_t got = 0;
    while (q->head != NULL && got < cap) {
        wtb_chunk* c = q->head;
        size_t take = c->len - c->off;
        if (take > cap - got) {
            take = cap - got;
        }
        memcpy(buf + got, c->data + c->off, take);
        got += take;
        c->off += take;
        q->bytes -= take;
        if (c->off == c->len) {
            q->head = c->next;
            if (q->head == NULL) {
                q->tail = NULL;
            }
            q->count -= 1;
            free(c);
        }
    }
    return got;
}

static void wtb_queue_clear(wtb_queue* q)
{
    while (q->head != NULL) {
        wtb_queue_pop(q);
    }
}

/* -------------------------------------------------------------- types -- */

typedef enum {
    WTB_OP_CONNECT,
    WTB_OP_STREAM_OPEN,
    WTB_OP_STREAM_RESET,
    WTB_OP_SESSION_CLOSE,
    WTB_OP_SESSION_DESTROY,
    WTB_OP_STREAM_DESTROY
} wtb_op_type;

typedef struct wtb_op {
    struct wtb_op* next;
    wtb_op_type type;
    ngh_wtb_session* s;
    ngh_wtb_stream* st;
    uint32_t code;
    char* reason;
} wtb_op;

struct ngh_wtb_stream {
    ngh_wtb_session* s;
    struct ngh_wtb_stream* next;        /* session->streams */
    struct ngh_wtb_stream* accept_next; /* session->accept_head */
    h3zero_stream_ctx_t* hs;            /* network thread only; NULL once gone */
    int bidi;
    int is_local;
    int fin_received;
    int reset_received;
    int finish_requested;
    int fin_sent;
    int dead;
    wtb_queue recvq;
    wtb_queue sendq;
};

struct ngh_wtb_session {
    ngh_wtb_ctx* ctx;
    struct ngh_wtb_session* next;
    int refs; /* 1 for the app handle + 1 per live stream shell */
    /* connect parameters, owned */
    char* host;
    int port;
    char* path;
    char* authority;
    uint32_t idle_timeout_ms;
    uint32_t connect_timeout_ms;
    uint64_t connect_deadline;
    /* network-thread objects */
    picoquic_cnx_t* cnx;
    h3zero_callback_ctx_t* h3;
    h3zero_stream_ctx_t* control;
    uint64_t control_id;
    picowt_capsule_t capsule;
    /* shared, under ctx->lock */
    int state;
    int close_requested;
    int dead;
    char err[256];
    size_t dgram_max;
    wtb_queue dgram_recvq;
    wtb_queue dgram_sendq;
    ngh_wtb_stream* streams;
    ngh_wtb_stream* accept_head;
    ngh_wtb_stream* accept_tail;
    size_t accept_count;
};

typedef struct wtb_pin {
    struct wtb_pin* next;
    uint8_t hash[32];
} wtb_pin;

struct ngh_wtb_ctx {
    picoquic_quic_t* quic;
    picoquic_network_thread_ctx_t* net;
    picoquic_packet_loop_param_t loop_param; /* must outlive the thread */
    wtb_mutex_t lock;
    wtb_op* op_head;
    wtb_op* op_tail;
    ngh_wtb_session* sessions;
    wtb_pin* pins;
    ptls_verify_certificate_t verifier;
    char err[256];
};

static void wtb_wake(ngh_wtb_ctx* ctx)
{
    if (ctx->net != NULL) {
        (void)picoquic_wake_up_network_thread(ctx->net);
    }
}

static void wtb_op_push(ngh_wtb_ctx* ctx, wtb_op_type type,
    ngh_wtb_session* s, ngh_wtb_stream* st, uint32_t code, const char* reason)
{
    wtb_op* op = (wtb_op*)calloc(1, sizeof(wtb_op));
    if (op == NULL) {
        return;
    }
    op->type = type;
    op->s = s;
    op->st = st;
    op->code = code;
    op->reason = (reason != NULL) ? strdup(reason) : NULL;
    if (ctx->op_tail == NULL) {
        ctx->op_head = ctx->op_tail = op;
    }
    else {
        ctx->op_tail->next = op;
        ctx->op_tail = op;
    }
}

static void wtb_session_fail(ngh_wtb_session* s, const char* msg)
{
    if (s->state == NGH_WTB_PENDING || s->state == NGH_WTB_READY) {
        s->state = NGH_WTB_ERROR;
        snprintf(s->err, sizeof(s->err), "%s", msg);
    }
}

/* ------------------------------------------- certificate pinning (TLS) -- */
/* serverCertificateHashes semantics: accept the chain iff the SHA-256 of
 * the end-entity DER matches a pin, then still verify the TLS
 * CertificateVerify signature against that certificate's public key --
 * without the signature check a pin authenticates nothing. */

typedef struct wtb_verify_ctx {
    EVP_PKEY* pkey;
} wtb_verify_ctx;

static int wtb_verify_sign(void* verify_ctx, uint16_t algo,
    ptls_iovec_t data, ptls_iovec_t sign)
{
    wtb_verify_ctx* vc = (wtb_verify_ctx*)verify_ctx;
    const EVP_MD* md = NULL;
    int is_pss = 0;
    int ret = -1;

    if (vc == NULL) {
        return -1;
    }
    if (data.base == NULL) {
        /* disposal call */
        EVP_PKEY_free(vc->pkey);
        free(vc);
        return 0;
    }
    switch (algo) {
    case 0x0403: md = EVP_sha256(); break; /* ecdsa_secp256r1_sha256 */
    case 0x0503: md = EVP_sha384(); break; /* ecdsa_secp384r1_sha384 */
    case 0x0603: md = EVP_sha512(); break; /* ecdsa_secp521r1_sha512 */
    case 0x0804: md = EVP_sha256(); is_pss = 1; break; /* rsa_pss_rsae_* */
    case 0x0805: md = EVP_sha384(); is_pss = 1; break;
    case 0x0806: md = EVP_sha512(); is_pss = 1; break;
    case 0x0807: md = NULL; break; /* ed25519 */
    default:
        goto done;
    }
    {
        EVP_MD_CTX* mctx = EVP_MD_CTX_new();
        EVP_PKEY_CTX* pctx = NULL;
        if (mctx == NULL) {
            goto done;
        }
        if (EVP_DigestVerifyInit(mctx, &pctx, md, NULL, vc->pkey) == 1) {
            if (is_pss) {
                (void)EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING);
                (void)EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST);
            }
            if (EVP_DigestVerify(mctx, sign.base, sign.len,
                    data.base, data.len) == 1) {
                ret = 0;
            }
        }
        EVP_MD_CTX_free(mctx);
    }
done:
    EVP_PKEY_free(vc->pkey);
    free(vc);
    return ret;
}

static int wtb_verify_cb(ptls_verify_certificate_t* self, ptls_t* tls,
    const char* server_name,
    int (**verify_sign)(void*, uint16_t, ptls_iovec_t, ptls_iovec_t),
    void** verify_data, ptls_iovec_t* certs, size_t num_certs)
{
    ngh_wtb_ctx* ctx = (ngh_wtb_ctx*)((char*)self - offsetof(ngh_wtb_ctx, verifier));
    uint8_t digest[32];
    wtb_pin* pin;
    int matched = 0;
    (void)tls;
    (void)server_name;

    *verify_sign = NULL;
    *verify_data = NULL;
    if (num_certs == 0) {
        return -1;
    }
    SHA256(certs[0].base, certs[0].len, digest);
    wtb_mutex_lock(&ctx->lock);
    for (pin = ctx->pins; pin != NULL; pin = pin->next) {
        if (memcmp(pin->hash, digest, 32) == 0) {
            matched = 1;
            break;
        }
    }
    wtb_mutex_unlock(&ctx->lock);
    if (!matched) {
        return -1;
    }
    {
        const uint8_t* p = certs[0].base;
        X509* x = d2i_X509(NULL, &p, (long)certs[0].len);
        wtb_verify_ctx* vc;
        if (x == NULL) {
            return -1;
        }
        vc = (wtb_verify_ctx*)calloc(1, sizeof(wtb_verify_ctx));
        if (vc == NULL) {
            X509_free(x);
            return -1;
        }
        vc->pkey = X509_get_pubkey(x);
        X509_free(x);
        if (vc->pkey == NULL) {
            free(vc);
            return -1;
        }
        *verify_sign = wtb_verify_sign;
        *verify_data = vc;
    }
    return 0;
}

static const uint16_t wtb_verify_algos[] = {
    0x0403, 0x0503, 0x0603, 0x0804, 0x0805, 0x0806, 0x0807, UINT16_MAX
};

/* ------------------------------------------------ stream data callback -- */

static int wtb_stream_cb(picoquic_cnx_t* cnx, uint8_t* bytes, size_t length,
    picohttp_call_back_event_t event, h3zero_stream_ctx_t* stream_ctx,
    void* v_ctx);

static ngh_wtb_stream* wtb_stream_shell(ngh_wtb_session* s,
    h3zero_stream_ctx_t* hs, int is_local)
{
    ngh_wtb_stream* st = (ngh_wtb_stream*)calloc(1, sizeof(ngh_wtb_stream));
    if (st == NULL) {
        return NULL;
    }
    st->s = s;
    st->hs = hs;
    st->is_local = is_local;
    st->bidi = (hs != NULL) ? (WTB_IS_BIDIR_STREAM_ID(hs->stream_id) != 0) : 1;
    st->next = s->streams;
    s->streams = st;
    s->refs += 1;
    return st;
}

static void wtb_session_unref(ngh_wtb_session* s)
{
    s->refs -= 1;
    if (s->refs == 0) {
        wtb_queue_clear(&s->dgram_recvq);
        wtb_queue_clear(&s->dgram_sendq);
        free(s->host);
        free(s->path);
        free(s->authority);
        free(s);
    }
}

static void wtb_stream_free_shell(ngh_wtb_stream* st)
{
    ngh_wtb_session* s = st->s;
    ngh_wtb_stream** pp = &s->streams;
    while (*pp != NULL && *pp != st) {
        pp = &(*pp)->next;
    }
    if (*pp == st) {
        *pp = st->next;
    }
    wtb_queue_clear(&st->recvq);
    wtb_queue_clear(&st->sendq);
    free(st);
    wtb_session_unref(s);
}

static int wtb_stream_cb(picoquic_cnx_t* cnx, uint8_t* bytes, size_t length,
    picohttp_call_back_event_t event, h3zero_stream_ctx_t* stream_ctx,
    void* v_ctx)
{
    ngh_wtb_stream* st = (ngh_wtb_stream*)v_ctx;
    ngh_wtb_ctx* ctx = st->s->ctx;
    int ret = 0;

    wtb_mutex_lock(&ctx->lock);
    switch (event) {
    case picohttp_callback_post_data:
    case picohttp_callback_post_fin:
        if (length > 0 && !st->dead) {
            (void)wtb_queue_push(&st->recvq, bytes, length);
        }
        if (event == picohttp_callback_post_fin) {
            st->fin_received = 1;
        }
        break;
    case picohttp_callback_provide_data: {
        /* `bytes` is the buffer context for picoquic_provide_stream_data_buffer */
        size_t queued = st->sendq.bytes;
        size_t n = (queued < length) ? queued : length;
        int fin_now = (st->finish_requested && n == queued && !st->fin_sent);
        int still_active = (queued > n) ? 1 : 0;
        uint8_t* buf = picoquic_provide_stream_data_buffer(bytes, n,
            fin_now, still_active);
        if (buf == NULL) {
            ret = -1;
        }
        else if (n > 0) {
            (void)wtb_queue_read(&st->sendq, buf, n);
        }
        if (fin_now) {
            st->fin_sent = 1;
        }
        break;
    }
    case picohttp_callback_reset:
        st->reset_received = 1;
        break;
    case picohttp_callback_deregister:
    case picohttp_callback_free:
        st->hs = NULL;
        break;
    default:
        break;
    }
    wtb_mutex_unlock(&ctx->lock);
    (void)cnx;
    (void)stream_ctx;
    return ret;
}

/* --------------------------------------------- session (WT) callback -- */

static int wtb_session_cb(picoquic_cnx_t* cnx, uint8_t* bytes, size_t length,
    picohttp_call_back_event_t event, h3zero_stream_ctx_t* stream_ctx,
    void* v_ctx)
{
    ngh_wtb_session* s = (ngh_wtb_session*)v_ctx;
    ngh_wtb_ctx* ctx = s->ctx;
    int ret = 0;

    wtb_mutex_lock(&ctx->lock);
    switch (event) {
    case picohttp_callback_connecting:
        break;
    case picohttp_callback_connect_accepted:
        if (stream_ctx != NULL) {
            stream_ctx->path_callback = wtb_session_cb;
            stream_ctx->path_callback_ctx = s;
        }
        if (s->state == NGH_WTB_PENDING) {
            s->state = NGH_WTB_READY;
        }
        s->dgram_max = WTB_DGRAM_MAX_SAFE;
        break;
    case picohttp_callback_connect_refused:
        wtb_session_fail(s, "connect refused by server");
        break;
    case picohttp_callback_post_data:
    case picohttp_callback_post_fin:
        if (stream_ctx == s->control) {
            /* control stream carries capsules */
            if (length > 0) {
                const uint8_t* b = bytes;
                if (picowt_receive_capsule(cnx, b, b + length, &s->capsule) == 0 &&
                    s->capsule.h3_capsule.is_stored) {
                    char reason[128] = { 0 };
                    if (s->capsule.error_msg_len > 0) {
                        size_t n = s->capsule.error_msg_len;
                        if (n >= sizeof(reason)) {
                            n = sizeof(reason) - 1;
                        }
                        memcpy(reason, s->capsule.error_msg, n);
                    }
                    if (s->state == NGH_WTB_PENDING || s->state == NGH_WTB_READY) {
                        s->state = NGH_WTB_CLOSED;
                        snprintf(s->err, sizeof(s->err),
                            "closed by peer: code %u%s%s",
                            (unsigned)s->capsule.error_code,
                            reason[0] ? ", " : "", reason);
                    }
                    picowt_release_capsule(&s->capsule);
                }
            }
            if (event == picohttp_callback_post_fin &&
                (s->state == NGH_WTB_PENDING || s->state == NGH_WTB_READY)) {
                s->state = NGH_WTB_CLOSED;
            }
        }
        else if (stream_ctx != NULL) {
            /* first event of a peer-initiated stream: adopt it */
            if (s->accept_count >= WTB_ACCEPT_QUEUE_MAX || s->dead) {
                ret = picoquic_reset_stream(cnx, stream_ctx->stream_id,
                    H3ZERO_WEBTRANSPORT_BUFFERED_STREAM_REJECTED);
            }
            else {
                ngh_wtb_stream* st = wtb_stream_shell(s, stream_ctx, 0);
                if (st == NULL) {
                    ret = -1;
                }
                else {
                    stream_ctx->path_callback = wtb_stream_cb;
                    stream_ctx->path_callback_ctx = st;
                    if (s->accept_tail == NULL) {
                        s->accept_head = s->accept_tail = st;
                    }
                    else {
                        s->accept_tail->accept_next = st;
                        s->accept_tail = st;
                    }
                    s->accept_count += 1;
                    if (length > 0) {
                        (void)wtb_queue_push(&st->recvq, bytes, length);
                    }
                    if (event == picohttp_callback_post_fin) {
                        st->fin_received = 1;
                    }
                }
            }
        }
        break;
    case picohttp_callback_provide_datagram: {
        /* `bytes` is the buffer context for h3zero_provide_datagram_buffer */
        wtb_chunk* c = s->dgram_sendq.head;
        if (c != NULL && c->len <= length) {
            int more = (c->next != NULL) ? 1 : 0;
            uint8_t* buf = h3zero_provide_datagram_buffer(bytes, c->len, more);
            if (buf != NULL) {
                memcpy(buf, c->data, c->len);
                wtb_queue_pop(&s->dgram_sendq);
            }
        }
        break;
    }
    case picohttp_callback_post_datagram:
        if (!s->dead) {
            while (s->dgram_recvq.count >= WTB_DGRAM_QUEUE_MAX) {
                wtb_queue_pop(&s->dgram_recvq);
            }
            (void)wtb_queue_push(&s->dgram_recvq, bytes, length);
        }
        break;
    case picohttp_callback_reset:
        wtb_session_fail(s, "session control stream reset by peer");
        break;
    case picohttp_callback_drain:
        break;
    case picohttp_callback_deregister:
    case picohttp_callback_free:
        s->control = NULL;
        if (s->state == NGH_WTB_PENDING) {
            wtb_session_fail(s, "connection closed during setup");
        }
        else if (s->state == NGH_WTB_READY) {
            s->state = NGH_WTB_CLOSED;
        }
        break;
    default:
        break;
    }
    wtb_mutex_unlock(&ctx->lock);
    return ret;
}

/* ------------------------------------------------- network thread work -- */

static void wtb_do_connect(ngh_wtb_ctx* ctx, ngh_wtb_session* s)
{
    struct sockaddr_storage addr;
    int is_name = 0;
    uint64_t now = picoquic_current_time();

    if (picoquic_get_server_address(s->host, s->port, &addr, &is_name) != 0) {
        wtb_session_fail(s, "cannot resolve host");
        return;
    }
    if (picowt_prepare_client_cnx(ctx->quic, (struct sockaddr*)&addr,
            &s->cnx, &s->h3, &s->control, now, s->host) != 0) {
        wtb_session_fail(s, "connection setup failed");
        return;
    }
    s->control_id = s->control->stream_id;
    if (picowt_connect(s->cnx, s->h3, s->control, s->authority, s->path,
            wtb_session_cb, s, NULL) != 0) {
        wtb_session_fail(s, "CONNECT setup failed");
        return;
    }
    if (picoquic_start_client_cnx(s->cnx) != 0) {
        wtb_session_fail(s, "cannot start connection");
        return;
    }
    s->connect_deadline = now + (uint64_t)s->connect_timeout_ms * 1000;
}

static void wtb_do_session_teardown(ngh_wtb_ctx* ctx, ngh_wtb_session* s)
{
    ngh_wtb_stream* st;
    /* h3zero teardown fires deregister/free callbacks that clear hs. */
    if (s->h3 != NULL) {
        h3zero_callback_delete_context(s->cnx, s->h3);
        s->h3 = NULL;
        s->control = NULL;
    }
    if (s->cnx != NULL) {
        picoquic_delete_cnx(s->cnx);
        s->cnx = NULL;
    }
    for (st = s->streams; st != NULL; st = st->next) {
        st->hs = NULL;
    }
    /* unlink from ctx */
    {
        ngh_wtb_session** pp = &ctx->sessions;
        while (*pp != NULL && *pp != s) {
            pp = &(*pp)->next;
        }
        if (*pp == s) {
            *pp = s->next;
        }
    }
    wtb_session_unref(s);
}

static void wtb_process_ops(ngh_wtb_ctx* ctx)
{
    for (;;) {
        wtb_op* op = ctx->op_head;
        if (op == NULL) {
            break;
        }
        ctx->op_head = op->next;
        if (ctx->op_head == NULL) {
            ctx->op_tail = NULL;
        }
        switch (op->type) {
        case WTB_OP_CONNECT:
            wtb_do_connect(ctx, op->s);
            break;
        case WTB_OP_STREAM_OPEN:
            if (op->s->cnx != NULL && op->s->h3 != NULL && !op->st->dead) {
                h3zero_stream_ctx_t* hs = picowt_create_local_stream(
                    op->s->cnx, op->st->bidi, op->s->h3, op->s->control_id);
                if (hs == NULL) {
                    op->st->reset_received = 1;
                }
                else {
                    hs->path_callback = wtb_stream_cb;
                    hs->path_callback_ctx = op->st;
                    op->st->hs = hs;
                }
            }
            break;
        case WTB_OP_STREAM_RESET:
            if (op->st->hs != NULL && op->s->cnx != NULL) {
                (void)picowt_reset_stream(op->s->cnx, op->st->hs, op->code);
                op->st->hs = NULL;
            }
            break;
        case WTB_OP_SESSION_CLOSE:
            if (op->s->cnx != NULL && op->s->control != NULL) {
                (void)picowt_send_close_session_message(op->s->cnx,
                    op->s->control, op->code,
                    op->reason != NULL ? op->reason : "");
            }
            if (op->s->cnx != NULL) {
                (void)picoquic_close(op->s->cnx, 0);
            }
            if (op->s->state == NGH_WTB_PENDING || op->s->state == NGH_WTB_READY) {
                op->s->state = NGH_WTB_CLOSED;
            }
            break;
        case WTB_OP_SESSION_DESTROY:
            wtb_do_session_teardown(ctx, op->s);
            break;
        case WTB_OP_STREAM_DESTROY:
            if (op->st->hs != NULL) {
                /* detach; h3zero keeps managing the stream lifecycle */
                op->st->hs->path_callback = NULL;
                op->st->hs->path_callback_ctx = NULL;
                op->st->hs = NULL;
            }
            wtb_stream_free_shell(op->st);
            break;
        }
        free(op->reason);
        free(op);
    }
}

static void wtb_pump(ngh_wtb_ctx* ctx)
{
    ngh_wtb_session* s;
    uint64_t now = picoquic_current_time();

    wtb_process_ops(ctx);
    for (s = ctx->sessions; s != NULL; s = s->next) {
        ngh_wtb_stream* st;
        if (s->cnx == NULL) {
            continue;
        }
        if (picoquic_get_cnx_state(s->cnx) == picoquic_state_disconnected) {
            if (s->state == NGH_WTB_PENDING) {
                wtb_session_fail(s, "connection failed");
            }
            else if (s->state == NGH_WTB_READY) {
                s->state = NGH_WTB_CLOSED;
            }
            continue;
        }
        if (s->state == NGH_WTB_PENDING && s->connect_deadline != 0 &&
            now > s->connect_deadline) {
            wtb_session_fail(s, "connect timeout");
            (void)picoquic_close(s->cnx, 0);
            continue;
        }
        if (s->dgram_sendq.head != NULL && s->state == NGH_WTB_READY) {
            (void)h3zero_set_datagram_ready(s->cnx, s->control_id);
        }
        for (st = s->streams; st != NULL; st = st->next) {
            if (st->hs != NULL &&
                (st->sendq.bytes > 0 ||
                    (st->finish_requested && !st->fin_sent))) {
                (void)picoquic_mark_active_stream(s->cnx, st->hs->stream_id,
                    1, st->hs);
            }
        }
    }
}

static int wtb_loop_cb(picoquic_quic_t* quic,
    picoquic_packet_loop_cb_enum cb_mode, void* callback_ctx,
    void* callback_arg)
{
    ngh_wtb_ctx* ctx = (ngh_wtb_ctx*)callback_ctx;
    (void)quic;

    switch (cb_mode) {
    case picoquic_packet_loop_ready:
        if (callback_arg != NULL) {
            /* enable wake-up support */
            ((picoquic_packet_loop_options_t*)callback_arg)->do_time_check = 1;
        }
        break;
    case picoquic_packet_loop_time_check: {
        /* bound the sleep so timeouts and state polls stay responsive */
        packet_loop_time_check_arg_t* tc = (packet_loop_time_check_arg_t*)callback_arg;
        if (tc->delta_t > 250000) {
            tc->delta_t = 250000;
        }
        wtb_mutex_lock(&ctx->lock);
        wtb_pump(ctx);
        wtb_mutex_unlock(&ctx->lock);
        break;
    }
    case picoquic_packet_loop_wake_up:
    case picoquic_packet_loop_after_receive:
    case picoquic_packet_loop_after_send:
        wtb_mutex_lock(&ctx->lock);
        wtb_pump(ctx);
        wtb_mutex_unlock(&ctx->lock);
        break;
    default:
        break;
    }
    return 0;
}

/* ---------------------------------------------------------- public API -- */

uint32_t ngh_wtb_abi_version(void)
{
    return NGH_WTB_ABI_VERSION;
}

ngh_wtb_ctx* ngh_wtb_ctx_create(void)
{
    ngh_wtb_ctx* ctx = (ngh_wtb_ctx*)calloc(1, sizeof(ngh_wtb_ctx));
    int thread_ret = 0;

    if (ctx == NULL) {
        return NULL;
    }
#if defined(_WIN32)
    {
        WSADATA wsa;
        (void)WSAStartup(MAKEWORD(2, 2), &wsa);
    }
#endif
    wtb_mutex_init(&ctx->lock);

    ctx->quic = picoquic_create(64, NULL, NULL, NULL, "h3", NULL, NULL,
        NULL, NULL, NULL, picoquic_current_time(), NULL, NULL, NULL, 0);
    if (ctx->quic == NULL) {
        free(ctx);
        return NULL;
    }
    ctx->verifier.cb = wtb_verify_cb;
    ctx->verifier.algos = wtb_verify_algos;
    picoquic_set_verify_certificate_callback(ctx->quic, &ctx->verifier, NULL);

    ctx->loop_param.local_af = AF_UNSPEC;
    ctx->net = picoquic_start_network_thread(ctx->quic, &ctx->loop_param,
        wtb_loop_cb, ctx, &thread_ret);
    if (ctx->net == NULL) {
        picoquic_free(ctx->quic);
        free(ctx);
        return NULL;
    }
    return ctx;
}

void ngh_wtb_ctx_destroy(ngh_wtb_ctx* ctx)
{
    ngh_wtb_session* s;

    if (ctx == NULL) {
        return;
    }
    wtb_mutex_lock(&ctx->lock);
    for (s = ctx->sessions; s != NULL; s = s->next) {
        if (s->cnx != NULL &&
            picoquic_get_cnx_state(s->cnx) != picoquic_state_disconnected) {
            wtb_op_push(ctx, WTB_OP_SESSION_CLOSE, s, NULL, 0, NULL);
        }
    }
    wtb_mutex_unlock(&ctx->lock);
    wtb_wake(ctx);

    picoquic_delete_network_thread(ctx->net);
    ctx->net = NULL;

    /* the network thread is gone; finish everything inline */
    wtb_mutex_lock(&ctx->lock);
    wtb_process_ops(ctx);
    while (ctx->sessions != NULL) {
        ngh_wtb_session* victim = ctx->sessions;
        victim->refs += 1; /* keep alive while tearing down streams */
        wtb_do_session_teardown(ctx, victim);
        while (victim->streams != NULL) {
            wtb_stream_free_shell(victim->streams);
        }
        wtb_session_unref(victim);
    }
    wtb_mutex_unlock(&ctx->lock);

    picoquic_free(ctx->quic);
    while (ctx->pins != NULL) {
        wtb_pin* p = ctx->pins;
        ctx->pins = p->next;
        free(p);
    }
    wtb_mutex_destroy(&ctx->lock);
    free(ctx);
}

const char* ngh_wtb_ctx_error(ngh_wtb_ctx* ctx)
{
    return (ctx != NULL && ctx->err[0] != 0) ? ctx->err : NULL;
}

static int wtb_parse_url(ngh_wtb_session* s, const char* url)
{
    const char* p;
    const char* host_start;
    const char* host_end;
    const char* path_start;
    size_t host_len;

    if (url == NULL || strncmp(url, "https://", 8) != 0) {
        return -1;
    }
    host_start = url + 8;
    path_start = strchr(host_start, '/');
    if (path_start == NULL) {
        path_start = host_start + strlen(host_start);
    }
    p = memchr(host_start, ':', (size_t)(path_start - host_start));
    if (p != NULL) {
        host_end = p;
        s->port = atoi(p + 1);
        if (s->port <= 0 || s->port > 65535) {
            return -1;
        }
    }
    else {
        host_end = path_start;
        s->port = 443;
    }
    host_len = (size_t)(host_end - host_start);
    if (host_len == 0) {
        return -1;
    }
    s->host = (char*)malloc(host_len + 1);
    if (s->host == NULL) {
        return -1;
    }
    memcpy(s->host, host_start, host_len);
    s->host[host_len] = 0;
    s->path = strdup((*path_start != 0) ? path_start : "/");
    {
        size_t alen = host_len + 16;
        s->authority = (char*)malloc(alen);
        if (s->authority != NULL) {
            snprintf(s->authority, alen, "%s:%d", s->host, s->port);
        }
    }
    return (s->path != NULL && s->authority != NULL) ? 0 : -1;
}

ngh_wtb_session* ngh_wtb_connect(ngh_wtb_ctx* ctx, const char* url,
    const ngh_wtb_options* opt)
{
    ngh_wtb_session* s;

    if (ctx == NULL) {
        return NULL;
    }
    ctx->err[0] = 0;
    if (opt == NULL || opt->cert_hashes == NULL || opt->cert_hash_count == 0) {
        /* CA-store verification is not implemented yet; failing loudly beats
         * connecting insecurely. */
        snprintf(ctx->err, sizeof(ctx->err),
            "cert_hashes is required (CA verification not implemented)");
        return NULL;
    }
    s = (ngh_wtb_session*)calloc(1, sizeof(ngh_wtb_session));
    if (s == NULL) {
        snprintf(ctx->err, sizeof(ctx->err), "out of memory");
        return NULL;
    }
    s->ctx = ctx;
    s->refs = 1;
    s->state = NGH_WTB_PENDING;
    if (wtb_parse_url(s, url) != 0) {
        snprintf(ctx->err, sizeof(ctx->err), "malformed url: %s",
            url != NULL ? url : "(null)");
        free(s->host);
        free(s->path);
        free(s->authority);
        free(s);
        return NULL;
    }
    s->idle_timeout_ms = opt->idle_timeout_ms;
    s->connect_timeout_ms = (opt->connect_timeout_ms != 0) ?
        opt->connect_timeout_ms : WTB_CONNECT_TIMEOUT_MS_DEFAULT;

    wtb_mutex_lock(&ctx->lock);
    {
        size_t i;
        for (i = 0; i < opt->cert_hash_count; i++) {
            wtb_pin* pin = (wtb_pin*)malloc(sizeof(wtb_pin));
            if (pin != NULL) {
                memcpy(pin->hash, opt->cert_hashes + 32 * i, 32);
                pin->next = ctx->pins;
                ctx->pins = pin;
            }
        }
    }
    s->next = ctx->sessions;
    ctx->sessions = s;
    wtb_op_push(ctx, WTB_OP_CONNECT, s, NULL, 0, NULL);
    wtb_mutex_unlock(&ctx->lock);
    wtb_wake(ctx);
    return s;
}

int ngh_wtb_session_state(ngh_wtb_session* s)
{
    int state;
    if (s == NULL) {
        return NGH_WTB_ERROR;
    }
    wtb_mutex_lock(&s->ctx->lock);
    state = s->state;
    wtb_mutex_unlock(&s->ctx->lock);
    return state;
}

const char* ngh_wtb_session_error(ngh_wtb_session* s)
{
    return (s != NULL && s->err[0] != 0) ? s->err : NULL;
}

void ngh_wtb_session_close(ngh_wtb_session* s, uint32_t error_code,
    const char* reason)
{
    if (s == NULL) {
        return;
    }
    wtb_mutex_lock(&s->ctx->lock);
    if (!s->close_requested) {
        s->close_requested = 1;
        wtb_op_push(s->ctx, WTB_OP_SESSION_CLOSE, s, NULL, error_code, reason);
    }
    wtb_mutex_unlock(&s->ctx->lock);
    wtb_wake(s->ctx);
}

void ngh_wtb_session_destroy(ngh_wtb_session* s)
{
    ngh_wtb_ctx* ctx;
    if (s == NULL) {
        return;
    }
    ctx = s->ctx;
    wtb_mutex_lock(&ctx->lock);
    s->dead = 1;
    wtb_op_push(ctx, WTB_OP_SESSION_DESTROY, s, NULL, 0, NULL);
    wtb_mutex_unlock(&ctx->lock);
    wtb_wake(ctx);
}

int ngh_wtb_dgram_send(ngh_wtb_session* s, const uint8_t* data, size_t len)
{
    int ret = NGH_WTB_ERR;
    if (s == NULL || data == NULL) {
        return NGH_WTB_ERR;
    }
    wtb_mutex_lock(&s->ctx->lock);
    if (s->state == NGH_WTB_READY && len <= s->dgram_max &&
        s->dgram_sendq.count < WTB_DGRAM_QUEUE_MAX) {
        if (wtb_queue_push(&s->dgram_sendq, data, len) == 0) {
            ret = 0;
        }
    }
    wtb_mutex_unlock(&s->ctx->lock);
    if (ret == 0) {
        wtb_wake(s->ctx);
    }
    return ret;
}

int ngh_wtb_dgram_recv(ngh_wtb_session* s, uint8_t* buf, size_t cap)
{
    int ret = 0;
    if (s == NULL || buf == NULL) {
        return NGH_WTB_ERR;
    }
    wtb_mutex_lock(&s->ctx->lock);
    if (s->dgram_recvq.head != NULL) {
        wtb_chunk* c = s->dgram_recvq.head;
        if (c->len > cap) {
            ret = NGH_WTB_ERR_SIZE;
        }
        else {
            memcpy(buf, c->data, c->len);
            ret = (int)c->len;
        }
        wtb_queue_pop(&s->dgram_recvq);
    }
    wtb_mutex_unlock(&s->ctx->lock);
    return ret;
}

size_t ngh_wtb_dgram_max(ngh_wtb_session* s)
{
    size_t v;
    if (s == NULL) {
        return 0;
    }
    wtb_mutex_lock(&s->ctx->lock);
    v = (s->state == NGH_WTB_READY) ? s->dgram_max : 0;
    wtb_mutex_unlock(&s->ctx->lock);
    return v;
}

ngh_wtb_stream* ngh_wtb_stream_open(ngh_wtb_session* s, int bidi)
{
    ngh_wtb_stream* st = NULL;
    if (s == NULL) {
        return NULL;
    }
    wtb_mutex_lock(&s->ctx->lock);
    if (s->state == NGH_WTB_READY) {
        st = wtb_stream_shell(s, NULL, 1);
        if (st != NULL) {
            st->bidi = (bidi != 0);
            wtb_op_push(s->ctx, WTB_OP_STREAM_OPEN, s, st, 0, NULL);
        }
    }
    wtb_mutex_unlock(&s->ctx->lock);
    if (st != NULL) {
        wtb_wake(s->ctx);
    }
    return st;
}

ngh_wtb_stream* ngh_wtb_stream_accept(ngh_wtb_session* s)
{
    ngh_wtb_stream* st = NULL;
    if (s == NULL) {
        return NULL;
    }
    wtb_mutex_lock(&s->ctx->lock);
    st = s->accept_head;
    if (st != NULL) {
        s->accept_head = st->accept_next;
        if (s->accept_head == NULL) {
            s->accept_tail = NULL;
        }
        st->accept_next = NULL;
        s->accept_count -= 1;
    }
    wtb_mutex_unlock(&s->ctx->lock);
    return st;
}

int ngh_wtb_stream_bidi(ngh_wtb_stream* st)
{
    return (st != NULL) ? st->bidi : 0;
}

int ngh_wtb_stream_write(ngh_wtb_stream* st, const uint8_t* data, size_t len)
{
    int accepted = NGH_WTB_ERR;
    if (st == NULL || data == NULL) {
        return NGH_WTB_ERR;
    }
    wtb_mutex_lock(&st->s->ctx->lock);
    if (!st->finish_requested && !st->reset_received &&
        st->s->state == NGH_WTB_READY) {
        size_t room = (st->sendq.bytes < WTB_STREAM_SENDQ_MAX) ?
            WTB_STREAM_SENDQ_MAX - st->sendq.bytes : 0;
        size_t n = (len < room) ? len : room;
        if (n == 0) {
            accepted = 0;
        }
        else if (wtb_queue_push(&st->sendq, data, n) == 0) {
            accepted = (int)n;
        }
    }
    wtb_mutex_unlock(&st->s->ctx->lock);
    if (accepted > 0) {
        wtb_wake(st->s->ctx);
    }
    return accepted;
}

int ngh_wtb_stream_finish(ngh_wtb_stream* st)
{
    if (st == NULL) {
        return NGH_WTB_ERR;
    }
    wtb_mutex_lock(&st->s->ctx->lock);
    st->finish_requested = 1;
    wtb_mutex_unlock(&st->s->ctx->lock);
    wtb_wake(st->s->ctx);
    return 0;
}

int ngh_wtb_stream_read(ngh_wtb_stream* st, uint8_t* buf, size_t cap)
{
    int ret;
    if (st == NULL || buf == NULL) {
        return NGH_WTB_ERR;
    }
    wtb_mutex_lock(&st->s->ctx->lock);
    if (st->recvq.head != NULL) {
        ret = (int)wtb_queue_read(&st->recvq, buf, cap);
    }
    else if (st->reset_received) {
        ret = NGH_WTB_ERR_RESET;
    }
    else if (st->fin_received) {
        ret = NGH_WTB_FIN;
    }
    else {
        ret = 0;
    }
    wtb_mutex_unlock(&st->s->ctx->lock);
    return ret;
}

void ngh_wtb_stream_reset(ngh_wtb_stream* st, uint32_t error_code)
{
    if (st == NULL) {
        return;
    }
    wtb_mutex_lock(&st->s->ctx->lock);
    wtb_op_push(st->s->ctx, WTB_OP_STREAM_RESET, st->s, st, error_code, NULL);
    wtb_mutex_unlock(&st->s->ctx->lock);
    wtb_wake(st->s->ctx);
}

void ngh_wtb_stream_destroy(ngh_wtb_stream* st)
{
    if (st == NULL) {
        return;
    }
    wtb_mutex_lock(&st->s->ctx->lock);
    st->dead = 1;
    wtb_op_push(st->s->ctx, WTB_OP_STREAM_DESTROY, st->s, st, 0, NULL);
    wtb_mutex_unlock(&st->s->ctx->lock);
    wtb_wake(st->s->ctx);
}
