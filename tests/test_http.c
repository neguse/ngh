#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#define NGH_HTTP_IMPLEMENTATION
#include "ngh_http.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
static uint64_t now_ms(void) { return (uint64_t)GetTickCount64(); }
#else
#include <sys/time.h>
#include <time.h>
static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}
#endif

#define CASE_TIMEOUT_MS 15000u
#define URL_CAPACITY 2048u
#define DETAIL_CAPACITY 512u

static const char expected_get_body[] = "ngh-http-get\n";
static uint64_t case_deadline_ms;

static void set_detail(char *detail, size_t capacity, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, capacity, fmt, ap);
    va_end(ap);
}

static const char *state_name(int state) {
    switch (state) {
    case NGH_HTTP_PENDING: return "PENDING";
    case NGH_HTTP_DONE: return "DONE";
    case NGH_HTTP_ERROR: return "ERROR";
    default: return "unknown state";
    }
}

static int make_url(char *url, size_t capacity, const char *base,
                    const char *path, char *detail, size_t detail_capacity) {
    const char *suffix = path;
    size_t base_len = strlen(base);
    int written;

    if (base_len != 0 && base[base_len - 1] == '/' && path[0] == '/')
        suffix = path + 1;
    written = snprintf(url, capacity, "%s%s", base, suffix);
    if (written < 0 || (size_t)written >= capacity) {
        set_detail(detail, detail_capacity, "URL is too long");
        return 0;
    }
    return 1;
}

static int wait_for_terminal(ngh_http *request) {
    int state;

    for (;;) {
        state = ngh_http_state(request);
        if (state != NGH_HTTP_PENDING || now_ms() >= case_deadline_ms)
            return state;
        sleep_ms(1);
    }
}

static ngh_http *fetch_url(const char *url, const ngh_http_options *options,
                           int *state, char *detail,
                           size_t detail_capacity) {
    ngh_http *request = ngh_http_fetch(url, options);
    const char *error;

    if (request == NULL) {
        error = ngh_http_last_error();
        set_detail(detail, detail_capacity, "fetch returned NULL%s%s",
                   error && error[0] ? ": " : "",
                   error && error[0] ? error : "");
        *state = -1;
        return NULL;
    }
    *state = wait_for_terminal(request);
    if (*state == NGH_HTTP_PENDING)
        set_detail(detail, detail_capacity,
                   "request remained PENDING after about %u ms",
                   (unsigned)CASE_TIMEOUT_MS);
    return request;
}

static ngh_http *fetch_path(const char *base, const char *path,
                            const ngh_http_options *options, int *state,
                            char *detail, size_t detail_capacity) {
    char url[URL_CAPACITY];
    if (!make_url(url, sizeof(url), base, path, detail, detail_capacity)) {
        *state = -1;
        return NULL;
    }
    return fetch_url(url, options, state, detail, detail_capacity);
}

static void describe_state(char *detail, size_t capacity, int state,
                           ngh_http *request, int expected) {
    const char *error = state == NGH_HTTP_ERROR
                            ? ngh_http_error(request)
                            : NULL;
    set_detail(detail, capacity, "state was %s, expected %s%s%s",
               state_name(state), state_name(expected),
               error && error[0] ? ": " : "",
               error && error[0] ? error : "");
}

static const char *find_bytes(const char *haystack, size_t haystack_len,
                              const char *needle, size_t needle_len) {
    size_t i;
    if (needle_len == 0)
        return haystack;
    if (needle_len > haystack_len)
        return NULL;
    for (i = 0; i <= haystack_len - needle_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return haystack + i;
    }
    return NULL;
}

static int ascii_contains_ci(const char *text, const char *needle) {
    size_t text_len = strlen(text);
    size_t needle_len = strlen(needle);
    size_t i;
    size_t j;

    if (needle_len == 0)
        return 1;
    if (needle_len > text_len)
        return 0;
    for (i = 0; i <= text_len - needle_len; ++i) {
        for (j = 0; j < needle_len; ++j) {
            unsigned char a = (unsigned char)text[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == needle_len)
            return 1;
    }
    return 0;
}

static int test_get(const char *base, char *detail, size_t capacity) {
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    const char *body;
    size_t body_len = 0;
    int state;
    int passed = 0;

    request = fetch_path(base, "/get", &options, &state, detail, capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_DONE) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_DONE);
        goto done;
    }
    if (ngh_http_status(request) != 200) {
        set_detail(detail, capacity, "status was %d, expected 200",
                   ngh_http_status(request));
        goto done;
    }
    body = ngh_http_body(request, &body_len);
    if (body == NULL) {
        set_detail(detail, capacity, "DONE response had a NULL body");
        goto done;
    }
    if (body_len != sizeof(expected_get_body) - 1 ||
        memcmp(body, expected_get_body, sizeof(expected_get_body) - 1) != 0) {
        set_detail(detail, capacity, "body did not match the fixed /get body");
        goto done;
    }
    if (body[body_len] != '\0') {
        set_detail(detail, capacity, "body[len] was not NUL");
        goto done;
    }
    passed = 1;

done:
    ngh_http_destroy(request);
    return passed;
}

static int test_status_404(const char *base, char *detail, size_t capacity) {
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    int state;
    int passed = 0;

    request = fetch_path(base, "/status/404", &options, &state, detail,
                         capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_DONE) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_DONE);
    } else if (ngh_http_status(request) != 404) {
        set_detail(detail, capacity, "status was %d, expected 404",
                   ngh_http_status(request));
    } else {
        passed = 1;
    }
    ngh_http_destroy(request);
    return passed;
}

static int test_request_header_lowercase(const char *base, char *detail,
                                         size_t capacity) {
    const char *headers[] = {"X-Ngh-Test", "hello"};
    static const char expected[] = "HEADER x-ngh-test: hello\n";
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    const char *body;
    size_t body_len = 0;
    int state;
    int passed = 0;

    options.headers = headers;
    options.header_count = 1;
    request = fetch_path(base, "/echo", &options, &state, detail, capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_DONE) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_DONE);
        goto done;
    }
    body = ngh_http_body(request, &body_len);
    if (body == NULL ||
        find_bytes(body, body_len, expected, sizeof(expected) - 1) == NULL) {
        set_detail(detail, capacity,
                   "echo did not contain the wire name x-ngh-test");
        goto done;
    }
    passed = 1;

done:
    ngh_http_destroy(request);
    return passed;
}

static int test_post_binary(const char *base, char *detail, size_t capacity) {
    static const unsigned char sent[] = {
        0x00, 0x41, 0xff, 0x0a, 0x00, 0x7f, 0x80, 0x5a
    };
    static const char method_line[] = "METHOD POST\n";
    static const char separator[] = "\n\n";
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    const char *body;
    const char *payload;
    size_t body_len = 0;
    size_t payload_len;
    int state;
    int passed = 0;

    options.method = "POST";
    options.body = sent;
    options.body_len = sizeof(sent);
    request = fetch_path(base, "/echo", &options, &state, detail, capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_DONE) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_DONE);
        goto done;
    }
    body = ngh_http_body(request, &body_len);
    if (body == NULL || body_len < sizeof(method_line) - 1 ||
        memcmp(body, method_line, sizeof(method_line) - 1) != 0) {
        set_detail(detail, capacity, "echo did not report method POST");
        goto done;
    }
    payload = find_bytes(body, body_len, separator, sizeof(separator) - 1);
    if (payload == NULL) {
        set_detail(detail, capacity, "echo body separator was missing");
        goto done;
    }
    payload += sizeof(separator) - 1;
    payload_len = body_len - (size_t)(payload - body);
    if (payload_len != sizeof(sent) || memcmp(payload, sent, sizeof(sent)) != 0) {
        set_detail(detail, capacity,
                   "echoed binary payload differed (got %lu bytes, want %lu)",
                   (unsigned long)payload_len, (unsigned long)sizeof(sent));
        goto done;
    }
    passed = 1;

done:
    ngh_http_destroy(request);
    return passed;
}

static int test_redirect(const char *base, char *detail, size_t capacity) {
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    const char *body;
    const char *location;
    size_t body_len = 0;
    int state;
    int passed = 0;

    request = fetch_path(base, "/redirect", &options, &state, detail, capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_DONE) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_DONE);
        ngh_http_destroy(request);
        return 0;
    }
    body = ngh_http_body(request, &body_len);
    if (ngh_http_status(request) != 200 || body == NULL ||
        body_len != sizeof(expected_get_body) - 1 ||
        memcmp(body, expected_get_body, sizeof(expected_get_body) - 1) != 0) {
        set_detail(detail, capacity,
                   "default redirect did not expose the final 200 /get body");
        ngh_http_destroy(request);
        return 0;
    }
    ngh_http_destroy(request);

    options = ngh_http_options_default();
    options.no_redirect = 1;
    request = fetch_path(base, "/redirect", &options, &state, detail, capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_DONE) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_DONE);
        goto done;
    }
    if (ngh_http_status(request) != 302) {
        set_detail(detail, capacity, "no_redirect status was %d, expected 302",
                   ngh_http_status(request));
        goto done;
    }
    location = ngh_http_header(request, "location");
    if (location == NULL || strcmp(location, "/get") != 0) {
        set_detail(detail, capacity,
                   "no_redirect Location was %s, expected /get",
                   location ? location : "NULL");
        goto done;
    }
    passed = 1;

done:
    ngh_http_destroy(request);
    return passed;
}

static int test_repeated_headers(const char *base, char *detail,
                                 size_t capacity) {
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    const char *multi;
    const char *cookies;
    int state;
    int passed = 0;

    request = fetch_path(base, "/repeat", &options, &state, detail, capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_DONE) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_DONE);
        goto done;
    }
    multi = ngh_http_header(request, "x-multi");
    if (multi == NULL || strcmp(multi, "a, b") != 0) {
        set_detail(detail, capacity, "X-Multi was %s, expected a, b",
                   multi ? multi : "NULL");
        goto done;
    }
    cookies = ngh_http_header(request, "set-cookie");
    if (cookies == NULL || strcmp(cookies, "a=1; b=2") != 0) {
        set_detail(detail, capacity,
                   "Set-Cookie was %s, expected a=1; b=2",
                   cookies ? cookies : "NULL");
        goto done;
    }
    passed = 1;

done:
    ngh_http_destroy(request);
    return passed;
}

static int test_max_body(const char *base, char *detail, size_t capacity) {
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    const char *error;
    int state;
    int passed = 0;

    options.max_body_len = 1024;
    request = fetch_path(base, "/big/4096", &options, &state, detail, capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_ERROR) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_ERROR);
        goto done;
    }
    error = ngh_http_error(request);
    if (error == NULL || error[0] == '\0') {
        set_detail(detail, capacity, "body-cap error text was empty");
        goto done;
    }
    if (strstr(error, "1024") == NULL && !ascii_contains_ci(error, "cap") &&
        !ascii_contains_ci(error, "limit") &&
        !ascii_contains_ci(error, "maximum")) {
        set_detail(detail, capacity,
                   "error did not identify the configured body cap: %s", error);
        goto done;
    }
    passed = 1;

done:
    ngh_http_destroy(request);
    return passed;
}

static int test_connection_refused(const char *base, char *detail,
                                   size_t capacity) {
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    int state;
    int passed = 0;
    (void)base;

    request = fetch_url("http://127.0.0.1:9", &options, &state, detail,
                        capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_ERROR) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_ERROR);
    } else if (ngh_http_error(request) == NULL ||
               ngh_http_error(request)[0] == '\0') {
        set_detail(detail, capacity, "connection error text was empty");
    } else {
        passed = 1;
    }
    ngh_http_destroy(request);
    return passed;
}

static int test_total_timeout(const char *base, char *detail, size_t capacity) {
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    int state;
    int passed = 0;

    options.total_timeout_ms = 500;
    request = fetch_path(base, "/slow", &options, &state, detail, capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_ERROR) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_ERROR);
    } else if (ngh_http_error(request) == NULL ||
               ngh_http_error(request)[0] == '\0') {
        set_detail(detail, capacity, "timeout error text was empty");
    } else {
        passed = 1;
    }
    ngh_http_destroy(request);
    return passed;
}

static int test_version(const char *base, char *detail, size_t capacity) {
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    int state;
    int version;
    int passed = 0;

    request = fetch_path(base, "/get", &options, &state, detail, capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_DONE) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_DONE);
        goto done;
    }
    version = ngh_http_version(request);
    if (version != NGH_HTTP_V_AUTO && version != NGH_HTTP_V1 &&
        version != NGH_HTTP_V2 && version != NGH_HTTP_V3) {
        set_detail(detail, capacity, "version was %d, expected 0, 1, 2, or 3",
                   version);
        goto done;
    }
    passed = 1;

done:
    ngh_http_destroy(request);
    return passed;
}

static int test_header_case_insensitive(const char *base, char *detail,
                                        size_t capacity) {
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    const char *content_type;
    int state;
    int passed = 0;

    request = fetch_path(base, "/get", &options, &state, detail, capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_DONE) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_DONE);
        goto done;
    }
    content_type = ngh_http_header(request, "Content-Type");
    if (content_type == NULL || strcmp(content_type, "text/plain") != 0) {
        set_detail(detail, capacity,
                   "Content-Type lookup returned %s, expected text/plain",
                   content_type ? content_type : "NULL");
        goto done;
    }
    passed = 1;

done:
    ngh_http_destroy(request);
    return passed;
}

static int test_header_iteration(const char *base, char *detail,
                                 size_t capacity) {
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    const char *name;
    const char *value;
    size_t i;
    size_t j;
    int state;
    int passed = 0;

    request = fetch_path(base, "/get", &options, &state, detail, capacity);
    if (request == NULL) return 0;
    if (state != NGH_HTTP_DONE) {
        if (detail[0] == '\0')
            describe_state(detail, capacity, state, request, NGH_HTTP_DONE);
        goto done;
    }
    for (i = 0; i < 128; ++i) {
        name = NULL;
        value = NULL;
        if (!ngh_http_header_at(request, i, &name, &value))
            break;
        if (name == NULL || value == NULL) {
            set_detail(detail, capacity,
                       "header_at returned 1 with a NULL name or value at %lu",
                       (unsigned long)i);
            goto done;
        }
        for (j = 0; name[j] != '\0'; ++j) {
            if (name[j] >= 'A' && name[j] <= 'Z') {
                set_detail(detail, capacity,
                           "header_at returned non-lowercase name %s", name);
                goto done;
            }
        }
    }
    if (i == 0) {
        set_detail(detail, capacity, "header_at returned no response headers");
        goto done;
    }
    if (i == 128) {
        set_detail(detail, capacity,
                   "header_at did not reach the end within 128 entries");
        goto done;
    }
    if (ngh_http_header_at(request, i, &name, &value) != 0 ||
        ngh_http_header_at(request, i + 1, &name, &value) != 0) {
        set_detail(detail, capacity, "header_at returned nonzero past the end");
        goto done;
    }
    passed = 1;

done:
    ngh_http_destroy(request);
    return passed;
}

static int test_forbidden_connection_header(const char *base, char *detail,
                                            size_t capacity) {
    const char *headers[] = {"Connection", "close"};
    ngh_http_options options = ngh_http_options_default();
    ngh_http *request;
    const char *error;
    char url[URL_CAPACITY];

    options.headers = headers;
    options.header_count = 1;
    if (!make_url(url, sizeof(url), base, "/get", detail, capacity))
        return 0;
    request = ngh_http_fetch(url, &options);
    if (request != NULL) {
        set_detail(detail, capacity,
                   "fetch accepted the forbidden Connection header");
        ngh_http_destroy(request);
        return 0;
    }
    error = ngh_http_last_error();
    if (error == NULL || error[0] == '\0') {
        set_detail(detail, capacity, "last_error was empty after NULL fetch");
        return 0;
    }
    return 1;
}

typedef int (*test_function)(const char *, char *, size_t);

typedef struct test_case {
    const char *name;
    test_function run;
} test_case;

int main(int argc, char **argv) {
    static const test_case cases[] = {
        {"get", test_get},
        {"status-404", test_status_404},
        {"request-header-lowercase", test_request_header_lowercase},
        {"post-binary", test_post_binary},
        {"redirect", test_redirect},
        {"repeated-headers", test_repeated_headers},
        {"max-body", test_max_body},
        {"connection-refused", test_connection_refused},
        {"total-timeout", test_total_timeout},
        {"version", test_version},
        {"header-case-insensitive", test_header_case_insensitive},
        {"header-iteration", test_header_iteration},
        {"forbidden-connection-header", test_forbidden_connection_header}
    };
    char detail[DETAIL_CAPACITY];
    size_t i;
    int failures = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: test_http <base_url>\n");
        return 2;
    }

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        detail[0] = '\0';
        case_deadline_ms = now_ms() + CASE_TIMEOUT_MS;
        if (cases[i].run(argv[1], detail, sizeof(detail))) {
            printf("PASS %s\n", cases[i].name);
        } else {
            printf("FAIL %s: %s\n", cases[i].name,
                   detail[0] ? detail : "unspecified failure");
            ++failures;
        }
        fflush(stdout);
    }
    return failures == 0 ? 0 : 1;
}
