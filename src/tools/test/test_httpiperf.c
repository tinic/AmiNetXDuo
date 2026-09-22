/* Tests for httpiperf.c, the /iperf URI decode.
 *
 * The endpoint accepts a grammar the server has to keep: the direction names,
 * the whole-number seconds and the dotted peer.  A decode that drops a case
 * still compiles and reads right, so each path is asserted against the exact
 * kind, direction, seconds, peer text, status and reason it must produce.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpiperf.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

/* Assert the parse of one path: kind, and for RUN the direction, seconds and
   peer text. */
static void parse(const char *path, HttpiperfKind kind, HttpiperfDir dir,
                  unsigned long seconds, const char *peer)
{
    HttpiperfParsed p;

    httpiperf_parse(path, &p);
    CHECK(p.kind == kind);
    if (kind == HTTPIPERF_RUN)
    {
        CHECK(p.dir == dir);
        CHECK(p.seconds == seconds);
        CHECK((p.peer == NULL && peer == NULL) ||
              (p.peer != NULL && peer != NULL && strcmp(p.peer, peer) == 0));
    }
}

/* Assert the parse of one invalid path: the exact status and reason. */
static void parse_err(const char *path, unsigned long status, const char *reason)
{
    HttpiperfParsed p;

    httpiperf_parse(path, &p);
    CHECK(p.kind == HTTPIPERF_ERR);
    CHECK(p.status == status);
    CHECK(strcmp(p.reason, reason) == 0);
}

/* Assert the decode of one peer string. */
static void peer(const char *text, unsigned long status, unsigned long v4,
                 const char *reason)
{
    unsigned long out = 0;
    const char *why  = NULL;

    CHECK(httpiperf_peer(text, &out, &why) == status);
    if (status == 200)
        CHECK(out == v4);
    else
        CHECK(strcmp(why, reason) == 0);
}

int main(void)
{
    printf("iperf path decode\n");

    /* Not an iperf path: ordinary routing continues. */
    parse("", HTTPIPERF_NONE, HTTPIPERF_TCP_TX, 0, NULL);
    parse("/", HTTPIPERF_NONE, HTTPIPERF_TCP_TX, 0, NULL);
    parse("/iper", HTTPIPERF_NONE, HTTPIPERF_TCP_TX, 0, NULL);
    parse("/iperfx", HTTPIPERF_NONE, HTTPIPERF_TCP_TX, 0, NULL);
    parse("/iperf.txt", HTTPIPERF_NONE, HTTPIPERF_TCP_TX, 0, NULL);

    /* The bare names serve the page, case-insensitively. */
    parse("/iperf", HTTPIPERF_PAGE, HTTPIPERF_TCP_TX, 0, NULL);
    parse("/iperf/", HTTPIPERF_PAGE, HTTPIPERF_TCP_TX, 0, NULL);
    parse("/IPERF", HTTPIPERF_PAGE, HTTPIPERF_TCP_TX, 0, NULL);
    parse("/Iperf/", HTTPIPERF_PAGE, HTTPIPERF_TCP_TX, 0, NULL);

    /* All four directions, in both the exact and a mixed case. */
    parse("/iperf/tcp-tx/10/1.2.3.4", HTTPIPERF_RUN, HTTPIPERF_TCP_TX, 10,
          "1.2.3.4");
    parse("/iperf/tcp-rx/10", HTTPIPERF_RUN, HTTPIPERF_TCP_RX, 10, NULL);
    parse("/iperf/udp-tx/10/1.2.3.4", HTTPIPERF_RUN, HTTPIPERF_UDP_TX, 10,
          "1.2.3.4");
    parse("/iperf/udp-rx/10", HTTPIPERF_RUN, HTTPIPERF_UDP_RX, 10, NULL);
    parse("/iperf/TCP-RX/10", HTTPIPERF_RUN, HTTPIPERF_TCP_RX, 10, NULL);
    parse("/iperf/Udp-Tx/10/1.2.3.4", HTTPIPERF_RUN, HTTPIPERF_UDP_TX, 10,
          "1.2.3.4");

    /* A direction without a seconds segment asks for one before it is judged. */
    parse_err("/iperf/tcp-tx", 400,
              "say how many seconds: /iperf/<direction>/<seconds>");
    parse_err("/iperf/notdir", 400,
              "say how many seconds: /iperf/<direction>/<seconds>");

    /* A direction that is not one of the four. */
    parse_err("/iperf/tcptx/10", 404,
              "the directions are tcp-tx, tcp-rx, udp-tx, udp-rx");
    parse_err("/iperf/tcp-t/10", 404,
              "the directions are tcp-tx, tcp-rx, udp-tx, udp-rx");
    parse_err("/iperf/tcp-txx/10", 404,
              "the directions are tcp-tx, tcp-rx, udp-tx, udp-rx");
    parse_err("/iperf/udp_rx/10", 404,
              "the directions are tcp-tx, tcp-rx, udp-tx, udp-rx");

    /* Seconds: zero, malformed and overflow all refuse a whole number. */
    parse_err("/iperf/tcp-rx/0", 400, "a run needs a whole number of seconds");
    parse_err("/iperf/tcp-rx/abc", 400,
              "a run needs a whole number of seconds");
    parse_err("/iperf/tcp-rx/-1", 400,
              "a run needs a whole number of seconds");
    parse_err("/iperf/tcp-rx/1a", 400,
              "a run needs a whole number of seconds");
    parse_err("/iperf/tcp-rx/1234567890", 400,
              "a run needs a whole number of seconds");
    parse_err("/iperf/tcp-rx/1000000000", 400,
              "a run needs a whole number of seconds");

    /* Nine digits is the decimal ceiling and still a run. */
    parse("/iperf/tcp-rx/999999999", HTTPIPERF_RUN, HTTPIPERF_TCP_RX,
          999999999UL, NULL);
    parse("/iperf/tcp-rx/1", HTTPIPERF_RUN, HTTPIPERF_TCP_RX, 1, NULL);

    /* Trailing material after the seconds: ignored on a receive, so the
       segment after seconds is carried as the peer text either way. */
    parse("/iperf/tcp-rx/10/extra", HTTPIPERF_RUN, HTTPIPERF_TCP_RX, 10,
          "extra");
    parse("/iperf/tcp-rx/10/extra/more", HTTPIPERF_RUN, HTTPIPERF_TCP_RX, 10,
          "extra/more");

    /* The peer is only decoded on the send directions. */
    peer(NULL, 400, 0,
         "a sending direction needs a peer: /iperf/tcp-tx/<seconds>/<host>");
    peer("", 400, 0,
         "a sending direction needs a peer: /iperf/tcp-tx/<seconds>/<host>");

    /* Boundary dotted addresses. */
    peer("0.0.0.0", 200, 0x00000000UL, NULL);
    peer("255.255.255.255", 200, 0xFFFFFFFFUL, NULL);
    peer("1.2.3.4", 200, 0x01020304UL, NULL);
    peer("192.168.1.1", 200, 0xC0A80101UL, NULL);

    /* Accepted oddities: leading zeros fold like any octet. */
    peer("01.2.3.4", 200, 0x01020304UL, NULL);
    peer("1.2.3.004", 200, 0x01020304UL, NULL);

    /* Not a dotted quad, and nothing else. */
    peer("1.2.3", 400, 0,
         "the peer must be a dotted address, not a "
         "name: nothing here can wait on a resolver");
    peer("1.2.3.4.5", 400, 0,
         "the peer must be a dotted address, not a "
         "name: nothing here can wait on a resolver");
    peer("256.1.1.1", 400, 0,
         "the peer must be a dotted address, not a "
         "name: nothing here can wait on a resolver");
    peer("1.2.3.256", 400, 0,
         "the peer must be a dotted address, not a "
         "name: nothing here can wait on a resolver");
    peer("1..2.3", 400, 0,
         "the peer must be a dotted address, not a "
         "name: nothing here can wait on a resolver");
    peer("a.b.c.d", 400, 0,
         "the peer must be a dotted address, not a "
         "name: nothing here can wait on a resolver");
    peer("1.2.3.4x", 400, 0,
         "the peer must be a dotted address, not a "
         "name: nothing here can wait on a resolver");
    peer("1.2.3.4/extra", 400, 0,
         "the peer must be a dotted address, not a "
         "name: nothing here can wait on a resolver");

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
