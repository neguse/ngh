/* Minimal test scaffolding shared by the tests. No dependencies beyond libc. */

#ifndef NGH_TEST_H_INCLUDED
#define NGH_TEST_H_INCLUDED

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define NGH_TEST_UNUSED __attribute__((unused))
#else
#define NGH_TEST_UNUSED
#endif

static int ngh_test_failures = 0;
static int ngh_test_checks = 0;
static const char *ngh_test_current NGH_TEST_UNUSED = "";

#define NGH_TEST_CASE(name)               \
    do {                                  \
        ngh_test_current = name;          \
        printf("  %-44s ", name);         \
        fflush(stdout);                   \
    } while (0)

#define NGH_TEST_DONE()                                       \
    do {                                                      \
        printf("%s\n", ngh_test_case_failed ? "FAIL" : "ok"); \
        ngh_test_case_failed = 0;                             \
    } while (0)

static int ngh_test_case_failed = 0;

NGH_TEST_UNUSED static void ngh_test_fail(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    if (!ngh_test_case_failed) printf("FAIL\n");
    ngh_test_case_failed = 1;
    ++ngh_test_failures;
    printf("    %s:%d: ", file, line);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

#define NGH_CHECK(cond)                                              \
    do {                                                             \
        ++ngh_test_checks;                                           \
        if (!(cond)) ngh_test_fail(__FILE__, __LINE__, "%s", #cond); \
    } while (0)

#define NGH_CHECK_EQ_INT(got, want)                                     \
    do {                                                                \
        long g_ = (long)(got), w_ = (long)(want);                       \
        ++ngh_test_checks;                                              \
        if (g_ != w_)                                                   \
            ngh_test_fail(__FILE__, __LINE__, "%s: got %ld, want %ld",  \
                          #got, g_, w_);                                \
    } while (0)

#define NGH_CHECK_NEAR(got, want, tol)                                       \
    do {                                                                     \
        double g_ = (double)(got), w_ = (double)(want), t_ = (double)(tol);  \
        double d_ = g_ > w_ ? g_ - w_ : w_ - g_;                             \
        ++ngh_test_checks;                                                   \
        if (!(d_ <= t_))                                                     \
            ngh_test_fail(__FILE__, __LINE__,                                \
                          "%s: got %.6f, want %.6f (delta %.6f > %.6f)",     \
                          #got, g_, w_, d_, t_);                             \
    } while (0)

NGH_TEST_UNUSED static int ngh_test_report(const char *suite) {
    printf("%s: %d checks, %d failures\n", suite, ngh_test_checks,
           ngh_test_failures);
    return ngh_test_failures == 0 ? 0 : 1;
}

/* ------------------------------------------------------------ test data -- */

NGH_TEST_UNUSED static char *ngh_test_read_file(const char *path,
                                                size_t *size_out) {
    long size;
    char *buf;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[size] = '\0';
    if (size_out) *size_out = (size_t)size;
    return buf;
}

/* Pulls every "<key>: <float>" out of a text proto, in file order. Enough for
 * MediaPipe's golden files, which are flat repeated messages. */
NGH_TEST_UNUSED static int ngh_test_scan_floats(const char *text,
                                                const char *key, float *out,
                                                int max) {
    size_t klen = strlen(key);
    const char *p = text;
    int n = 0;
    while (n < max && (p = strstr(p, key)) != NULL) {
        const char *after = p + klen;
        /* Require "key:" so that "x:" never matches inside "max:". */
        if (*after != ':' || (p != text && (p[-1] != ' ' && p[-1] != '\n'))) {
            p = after;
            continue;
        }
        out[n++] = (float)atof(after + 1);
        p = after + 1;
    }
    return n;
}

#endif /* NGH_TEST_H_INCLUDED */
