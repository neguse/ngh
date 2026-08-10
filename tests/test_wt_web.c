/* Web wiring test, run under node (which has no WebTransport): argument
 * checks work, connect returns a handle, and the missing API surfaces as a
 * clean NGH_WT_ERROR with a message instead of a crash. */
#define NGH_WEBTRANSPORT_IMPLEMENTATION
#include "ngh_webtransport.h"

#include <stdio.h>

#include "ngh_test.h"

static const uint8_t dummy_hash[32] = {0};

int main(void) {
    ngh_wt_options opt = ngh_wt_options_default();
    ngh_wt *wt;

    NGH_CHECK(ngh_wt_runtime_load(NULL) == 0);
    NGH_CHECK(ngh_wt_connect(NULL, NULL) == NULL);
    NGH_CHECK(ngh_wt_last_error() != NULL);
    NGH_CHECK(ngh_wt_state(NULL) == NGH_WT_ERROR);
    NGH_CHECK(ngh_wt_stream_write(NULL, "x", 1) == NGH_WT_ERR);

    opt.cert_hashes = dummy_hash;
    opt.cert_hash_count = 1;
    wt = ngh_wt_connect("https://127.0.0.1:9/", &opt);
    NGH_CHECK(wt != NULL);
    /* node has no WebTransport constructor: the failure is synchronous */
    NGH_CHECK(ngh_wt_state(wt) == NGH_WT_ERROR);
    NGH_CHECK(ngh_wt_error(wt) != NULL);
    NGH_CHECK(ngh_wt_dgram_send(wt, "x", 1) == NGH_WT_ERR);
    NGH_CHECK(ngh_wt_dgram_max(wt) == 0);
    NGH_CHECK(ngh_wt_stream_open(wt, NGH_WT_BIDI) == NULL);
    NGH_CHECK(ngh_wt_stream_accept(wt) == NULL);
    ngh_wt_destroy(wt);

    return ngh_test_report("test_wt_web");
}
