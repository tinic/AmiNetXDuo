/*
 * The /iperf path grammar, moved out of httpd.c verbatim.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpiperf.h"
#include "httprequest.h"

#include <stddef.h>

/* The two case-insensitive compares mirror hs_equal()/hs_nicmp() in httpstr.c,
   which is what httpd uses for every other path decision.  httpstr.h is not
   host-compilable (it pulls in <exec/types.h>), so this module restates the
   same ASCII A-Z fold here instead of linking it.  Keep the two in step. */

static int httpiperf_ni_cmp(const char *a, const char *b, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++)
    {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];

        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;

        if (ca != cb || ca == 0)
            return ca - cb;
    }

    return 0;
}

static int httpiperf_ci_equal(const char *a, const char *b)
{
    unsigned long na = 0;
    unsigned long nb = 0;

    while (a[na] != '\0') na++;
    while (b[nb] != '\0') nb++;

    return (na == nb && httpiperf_ni_cmp(a, b, nb) == 0);
}

void httpiperf_parse(const char *path, HttpiperfParsed *out)
{
    const char *dir;
    const char *secs;
    const char *host;
    unsigned long dirlen;
    unsigned long secslen;
    long n;

    out->kind    = HTTPIPERF_NONE;
    out->dir     = HTTPIPERF_TCP_TX;
    out->seconds = 0;
    out->peer    = NULL;
    out->status  = 0;
    out->reason  = NULL;

    if (!httpiperf_ci_equal(path, "/iperf") &&
        httpiperf_ni_cmp(path, "/iperf/", 7) != 0)
        return;

    if (httpiperf_ci_equal(path, "/iperf") ||
        httpiperf_ci_equal(path, "/iperf/"))
    {
        out->kind = HTTPIPERF_PAGE;
        return;
    }

    dir = path + 7;
    for (dirlen = 0; dir[dirlen] != '\0' && dir[dirlen] != '/'; dirlen++)
        ;

    secs = (dir[dirlen] == '/') ? dir + dirlen + 1 : NULL;
    if (secs == NULL)
    {
        out->kind   = HTTPIPERF_ERR;
        out->status = 400;
        out->reason = "say how many seconds: /iperf/<direction>/<seconds>";
        return;
    }

    for (secslen = 0; secs[secslen] != '\0' && secs[secslen] != '/'; secslen++)
        ;

    host = (secs[secslen] == '/') ? secs + secslen + 1 : NULL;

    if (dirlen == 6 && httpiperf_ni_cmp(dir, "tcp-tx", 6) == 0)
        out->dir = HTTPIPERF_TCP_TX;
    else if (dirlen == 6 && httpiperf_ni_cmp(dir, "tcp-rx", 6) == 0)
        out->dir = HTTPIPERF_TCP_RX;
    else if (dirlen == 6 && httpiperf_ni_cmp(dir, "udp-tx", 6) == 0)
        out->dir = HTTPIPERF_UDP_TX;
    else if (dirlen == 6 && httpiperf_ni_cmp(dir, "udp-rx", 6) == 0)
        out->dir = HTTPIPERF_UDP_RX;
    else
    {
        out->kind   = HTTPIPERF_ERR;
        out->status = 404;
        out->reason = "the directions are tcp-tx, tcp-rx, udp-tx, udp-rx";
        return;
    }

    n = http_request_decimal(secs, secslen);
    if (n < 1)
    {
        out->kind   = HTTPIPERF_ERR;
        out->status = 400;
        out->reason = "a run needs a whole number of seconds";
        return;
    }
    out->seconds = (unsigned long)n;

    out->kind = HTTPIPERF_RUN;
    out->peer = host;
}

unsigned long httpiperf_peer(const char *peer, unsigned long *peer_v4,
                             const char **reason)
{
    unsigned long v4;

    if (peer == NULL || peer[0] == '\0')
    {
        *reason = "a sending direction needs a peer: "
                  "/iperf/tcp-tx/<seconds>/<host>";
        return 400;
    }

    if (!http_request_dotted(peer, &v4))
    {
        *reason = "the peer must be a dotted address, not a "
                  "name: nothing here can wait on a resolver";
        return 400;
    }

    *peer_v4 = v4;
    return 200;
}
