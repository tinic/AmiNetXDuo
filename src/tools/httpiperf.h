/*
 * The /iperf URI decode, split out of httpd.c so the path grammar can be
 * tested on a host.  The direction names, the whole-number seconds and the
 * dotted peer are the part of the endpoint that must not drift, and httpd.c
 * itself does not build on a host.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPIPERF_H
#define AMINETXDUO_HTTPIPERF_H

/* The four directions, in the same order and with the same values as the
   IPERF_TCP_TX..IPERF_UDP_RX enum in iperfcore.h, so a caller can assign the
   decoded direction straight into an IperfPlan.dir. */
typedef enum
{
    HTTPIPERF_TCP_TX = 0,
    HTTPIPERF_TCP_RX,
    HTTPIPERF_UDP_TX,
    HTTPIPERF_UDP_RX
} HttpiperfDir;

/* What a decoded /iperf path is. */
typedef enum
{
    HTTPIPERF_NONE,   /* not under /iperf; ordinary routing continues        */
    HTTPIPERF_PAGE,   /* the bare /iperf or /iperf/: serve the page          */
    HTTPIPERF_RUN,    /* a run: dir, seconds and (TX) the raw peer are set   */
    HTTPIPERF_ERR     /* decoded but invalid: status and reason name the error*/
} HttpiperfKind;

typedef struct HttpiperfParsed
{
    HttpiperfKind kind;
    HttpiperfDir  dir;        /* RUN                                         */
    unsigned long seconds;    /* RUN                                         */
    const char   *peer;       /* RUN, TX only: raw peer text, or NULL        */
    unsigned long status;     /* ERR: the HTTP status to send                */
    const char   *reason;     /* ERR: the exact error text                   */
} HttpiperfParsed;

/* Decode a request path into one of the four kinds above.  `path` is the raw
   request target (for example "/iperf/udp-rx/10/1.2.3.4") and is never NULL. */
void httpiperf_parse(const char *path, HttpiperfParsed *out);

/* Decode the dotted peer of a TX run.  Returns 200 and fills *peer_v4, or 400
   with *reason (the peer is missing, or is not a dotted address). */
unsigned long httpiperf_peer(const char *peer, unsigned long *peer_v4,
                             const char **reason);

#endif /* AMINETXDUO_HTTPIPERF_H */
