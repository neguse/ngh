/* LD_PRELOAD shim for test_wt_dns: getaddrinfo() sleeps for
 * $NGH_TEST_DNS_DELAY_MS before doing the real lookup, standing in for a
 * slow or unreachable DNS server. Linux only. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <netdb.h>
#include <stdlib.h>
#include <time.h>

typedef int (*getaddrinfo_fn)(const char *, const char *,
                              const struct addrinfo *, struct addrinfo **);

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    getaddrinfo_fn real = (getaddrinfo_fn)dlsym(RTLD_NEXT, "getaddrinfo");
    const char *env = getenv("NGH_TEST_DNS_DELAY_MS");
    long ms = env ? atol(env) : 0;
    if (ms > 0) {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
    return real(node, service, hints, res);
}
