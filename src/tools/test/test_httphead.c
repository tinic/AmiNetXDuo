/* Tests for httphead.c, the pass over the request head.
 *
 * Every head here is fed exactly as httpd's loop would feed it: the buffer
 * from the first byte to the end of the blank line.  What is asserted is the
 * status a head is refused with, the reason that goes with it, and what the
 * fields say afterwards, including after a refusal: the Connection header
 * of a 400 and the drain that follows it are decided from that state.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httphead.h"

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

static char target[HTTP_URL_MAX];
static char value[HTTP_URL_MAX];

/* A fresh head, as a new connection has one: keepalive and http11 zero. */
static void fresh(HttpHead *h)
{
    memset(h, 0, sizeof(*h));
    http_head_reset(h);
    target[0] = '\0';
    value[0]  = '\0';
}

static unsigned long parse(HttpHead *h, const char *head)
{
    return http_head_parse(h, (const unsigned char *)head, strlen(head),
                           target, sizeof(target), value, sizeof(value));
}

static void test_request_line(void)
{
    HttpHead h;

    printf("request line\n");

    fresh(&h);
    CHECK(parse(&h, "GET / HTTP/1.1\r\nHost: amiga\r\n\r\n") == 0UL);
    CHECK(strcmp(h.method, "GET") == 0);
    CHECK(strcmp(target, "/") == 0);
    CHECK(h.http11 == 1 && h.keepalive == 1);
    CHECK(strcmp(h.host, "amiga") == 0);
    CHECK(h.reason == 0 && h.note == 0);

    /* Bare LF line endings are the same head. */
    fresh(&h);
    CHECK(parse(&h, "HEAD /a/b%20c HTTP/1.0\n\n") == 0UL);
    CHECK(strcmp(h.method, "HEAD") == 0);
    CHECK(strcmp(target, "/a/b%20c") == 0);
    CHECK(h.http11 == 0 && h.keepalive == 0);

    /* HTTP/1.0 keeps alive only when asked. */
    fresh(&h);
    CHECK(parse(&h, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
          == 0UL);
    CHECK(h.keepalive == 1);

    /* The version is part of the framing. */
    fresh(&h);
    CHECK(parse(&h, "GET /\r\n\r\n") == 400UL);
    CHECK(strcmp(h.reason, "that is not an HTTP version this server reads")
          == 0);
    fresh(&h);
    CHECK(parse(&h, "GET / HTTP/1.1junk\r\n\r\n") == 400UL);
    fresh(&h);
    CHECK(parse(&h, "GET / HTTP/2\r\n\r\n") == 400UL);

    /* No method at all. */
    fresh(&h);
    CHECK(parse(&h, " / HTTP/1.1\r\n\r\n") == 400UL);
    CHECK(strcmp(h.reason, "that is not a request line") == 0);

    /* A method the buffer cannot hold is 501, and it is refused before the
       version is read, so the connection's state is left as it was. */
    fresh(&h);
    h.keepalive = 1;
    h.http11    = 1;
    CHECK(parse(&h, "ABCDEFGHIJKLMNOPQRSTUVWX / HTTP/1.1\r\n\r\n") == 501UL);
    CHECK(strcmp(h.reason, "that is not a method this server has") == 0);
    CHECK(h.keepalive == 1 && h.http11 == 1);

    /* Twenty-three characters fit. */
    fresh(&h);
    CHECK(parse(&h, "ABCDEFGHIJKLMNOPQRSTUVW / HTTP/1.1\r\n\r\n") == 0UL);
    CHECK(strcmp(h.method, "ABCDEFGHIJKLMNOPQRSTUVW") == 0);

    /* A target the buffer cannot hold is 414. */
    {
        char head[HTTP_URL_MAX + 64];
        unsigned long n = 0;

        memcpy(head, "GET /", 5);
        n = 5;
        while (n < 4 + HTTP_URL_MAX)           /* a target of HTTP_URL_MAX */
            head[n++] = 'x';
        memcpy(&head[n], " HTTP/1.1\r\n\r\n", 14);

        fresh(&h);
        CHECK(parse(&h, head) == 414UL);
        CHECK(strcmp(h.reason, "that address is longer than this server "
                               "will read") == 0);
    }
}

static void test_framing(void)
{
    HttpHead h;

    printf("Content-Length and Transfer-Encoding\n");

    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\ncontent-length: 12\r\n\r\n") == 0UL);
    CHECK(h.body_left == 12UL);
    CHECK(h.chunk.state == HTTP_CHUNK_OFF);

    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nContent-Length: 12\r\n"
                    "Content-Length: 12\r\n\r\n") == 0UL);
    CHECK(h.body_left == 12UL);

    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nContent-Length: 12\r\n"
                    "Content-Length: 13\r\n\r\n") == 400UL);
    CHECK(strcmp(h.reason, "two Content-Lengths that disagree") == 0);

    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nContent-Length: 5abc\r\n\r\n")
          == 400UL);
    CHECK(strcmp(h.reason, "that is not a Content-Length") == 0);
    CHECK(h.note != 0 && strcmp(h.note, http_frame_error(HTTP_FRAME_JUNK))
          == 0);

    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nContent-Length: 4294967306\r\n\r\n")
          == 400UL);
    CHECK(h.note != 0 &&
          strcmp(h.note, http_frame_error(HTTP_FRAME_OVERFLOW)) == 0);

    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n")
          == 0UL);
    CHECK(h.chunk.state != HTTP_CHUNK_OFF);
    CHECK(h.body_left == 0UL);

    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n")
          == 501UL);
    CHECK(strcmp(h.reason, "that is not a transfer encoding this server can "
                           "undo") == 0);

    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n") == 400UL);
    CHECK(strcmp(h.reason, "two Transfer-Encodings") == 0);

    /* Both together is request smuggling, whichever order they came in. */
    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nContent-Length: 3\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n") == 400UL);
    CHECK(strcmp(h.reason, "a body cannot have both a length and an "
                           "encoding") == 0);
    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
                    "Content-Length: 3\r\n\r\n") == 400UL);
}

static void test_connection(void)
{
    HttpHead h;

    printf("Connection and the upgrade\n");

    /* close wins wherever it appears. */
    fresh(&h);
    CHECK(parse(&h, "GET / HTTP/1.1\r\nConnection: close\r\n"
                    "Connection: keep-alive\r\n\r\n") == 0UL);
    CHECK(h.keepalive == 0);
    fresh(&h);
    CHECK(parse(&h, "GET / HTTP/1.1\r\nConnection: keep-alive, close\r\n\r\n")
          == 0UL);
    CHECK(h.keepalive == 0);

    /* And is still in force when a later header is refused. */
    fresh(&h);
    CHECK(parse(&h, "PROPFIND / HTTP/1.1\r\nConnection: close\r\n"
                    "Depth: 2\r\n\r\n") == 400UL);
    CHECK(strcmp(h.reason, "that is not a Depth this server has") == 0);
    CHECK(h.keepalive == 0 && h.http11 == 1);

    fresh(&h);
    CHECK(parse(&h, "GET /shell HTTP/1.1\r\n"
                    "Connection: Upgrade\r\n"
                    "Upgrade: websocket\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Version: 13\r\n\r\n") == 0UL);
    CHECK(h.ws_connection == 1 && h.ws_upgrade == 1);
    CHECK(h.ws_version == HTTPD_WS_VERSION);
    CHECK(strcmp(h.ws_key, "dGhlIHNhbXBsZSBub25jZQ==") == 0);

    fresh(&h);
    CHECK(parse(&h, "GET /shell HTTP/1.1\r\n"
                    "Connection: keep-alive, upgrade\r\n"
                    "Upgrade: websocketX\r\n"
                    "Sec-WebSocket-Version: 8\r\n\r\n") == 0UL);
    CHECK(h.ws_connection == 1 && h.ws_upgrade == 0);
    CHECK(h.ws_version == 0);
    CHECK(h.ws_key[0] == '\0');

    /* A key that does not fit is no key. */
    fresh(&h);
    CHECK(parse(&h, "GET /shell HTTP/1.1\r\n"
                    "Sec-WebSocket-Key: 0123456789abcdef0123456789abcdef\r\n"
                    "\r\n") == 0UL);
    CHECK(h.ws_key[0] == '\0');
}

static void test_webdav_headers(void)
{
    HttpHead h;

    printf("Depth, Destination, Overwrite, If, Lock-Token, Timeout\n");

    fresh(&h);
    CHECK(h.depth == 1L && h.overwrite == 1);

    fresh(&h);
    CHECK(parse(&h, "PROPFIND / HTTP/1.1\r\nDepth: 0\r\n\r\n") == 0UL);
    CHECK(h.depth == 0L);
    fresh(&h);
    CHECK(parse(&h, "PROPFIND / HTTP/1.1\r\nDepth: 1\r\n\r\n") == 0UL);
    CHECK(h.depth == 1L);
    fresh(&h);
    CHECK(parse(&h, "PROPFIND / HTTP/1.1\r\nDepth: Infinity\r\n\r\n") == 0UL);
    CHECK(h.depth == -1L);
    fresh(&h);
    CHECK(parse(&h, "PROPFIND / HTTP/1.1\r\nDepth: infinityx\r\n\r\n")
          == 400UL);
    fresh(&h);
    CHECK(parse(&h, "PROPFIND / HTTP/1.1\r\nDepth: 10\r\n\r\n") == 400UL);

    fresh(&h);
    CHECK(parse(&h, "COPY /a HTTP/1.1\r\nHost: amiga:8080\r\n"
                    "Destination: http://amiga:8080/b\r\n"
                    "Overwrite: F\r\n\r\n") == 0UL);
    CHECK(strcmp(h.host, "amiga:8080") == 0);
    CHECK(strcmp(h.dest_url, "http://amiga:8080/b") == 0);
    CHECK(h.overwrite == 0);

    fresh(&h);
    CHECK(parse(&h, "COPY /a HTTP/1.1\r\nOverwrite: t\r\n\r\n") == 0UL);
    CHECK(h.overwrite == 1);
    fresh(&h);
    CHECK(parse(&h, "COPY /a HTTP/1.1\r\nOverwrite: yes\r\n\r\n") == 400UL);
    CHECK(strcmp(h.reason, "Overwrite must be T or F") == 0);
    fresh(&h);
    CHECK(parse(&h, "COPY /a HTTP/1.1\r\nOverwrite: T\r\nOverwrite: T\r\n\r\n")
          == 400UL);
    CHECK(strcmp(h.reason, "two Overwrite directives") == 0);

    /* A Destination that does not fit is 414, never a shorter path. */
    {
        char head[HTTP_URL_MAX + 96];
        unsigned long n;

        strcpy(head, "COPY /a HTTP/1.1\r\nDestination: /");
        n = strlen(head);
        while (n < strlen("COPY /a HTTP/1.1\r\nDestination: ") + HTTP_URL_MAX)
            head[n++] = 'd';
        strcpy(&head[n], "\r\n\r\n");

        fresh(&h);
        CHECK(parse(&h, head) == 414UL);
        CHECK(strcmp(h.reason, "that destination is longer than this server "
                               "will read") == 0);
    }

    fresh(&h);
    CHECK(parse(&h, "PUT /a HTTP/1.1\r\n"
                    "If: (<opaquelocktoken:one>) (<opaquelocktoken:two>)\r\n"
                    "\r\n") == 0UL);
    CHECK(strcmp(h.ifhdr, "(<opaquelocktoken:one>) (<opaquelocktoken:two>)")
          == 0);
    CHECK(strcmp(h.iftoken[0], "opaquelocktoken:one") == 0);
    CHECK(strcmp(h.iftoken[1], "opaquelocktoken:two") == 0);

    /* Half an If: is a different condition. */
    {
        char head[HTTPD_IF_MAX + 96];
        unsigned long n;

        strcpy(head, "PUT /a HTTP/1.1\r\nIf: (<opaquelocktoken:");
        n = strlen(head);
        while (n < strlen("PUT /a HTTP/1.1\r\nIf: ") + HTTPD_IF_MAX)
            head[n++] = 'a';
        strcpy(&head[n], ">)\r\n\r\n");

        fresh(&h);
        CHECK(parse(&h, head) == 431UL);
        CHECK(strcmp(h.reason, "that If: is longer than this server will "
                               "read") == 0);
        CHECK(h.ifhdr[0] == '\0');
    }

    fresh(&h);
    CHECK(parse(&h, "UNLOCK /a HTTP/1.1\r\n"
                    "Lock-Token: <opaquelocktoken:abc-123>\r\n\r\n") == 0UL);
    CHECK(strcmp(h.unlock_token, "opaquelocktoken:abc-123") == 0);

    /* Without the brackets nothing is read out of it. */
    fresh(&h);
    CHECK(parse(&h, "UNLOCK /a HTTP/1.1\r\n"
                    "Lock-Token: opaquelocktoken:abc-123\r\n\r\n") == 0UL);
    CHECK(h.unlock_token[0] == '\0');

    fresh(&h);
    CHECK(parse(&h, "LOCK /a HTTP/1.1\r\nTimeout: Second-60\r\n\r\n") == 0UL);
    CHECK(h.lock_secs == 60UL);
    fresh(&h);
    CHECK(parse(&h, "LOCK /a HTTP/1.1\r\nTimeout: Infinite\r\n\r\n") == 0UL);
    CHECK(h.lock_secs == HTTPD_LOCK_CAP);
    fresh(&h);
    CHECK(parse(&h, "LOCK /a HTTP/1.1\r\n\r\n") == 0UL);
    CHECK(h.lock_secs == 0UL);
}

static void test_conditions(void)
{
    HttpHead h;

    printf("Range, Expect, Accept-Encoding, If-Match, If-None-Match\n");

    fresh(&h);
    CHECK(parse(&h, "GET /f HTTP/1.1\r\nRange: bytes=0-1023\r\n\r\n") == 0UL);
    CHECK(h.has_range == 1 && h.range_from == 0UL && h.range_to == 1023UL);

    fresh(&h);
    CHECK(parse(&h, "GET /f HTTP/1.1\r\nRange: bytes=1024-\r\n\r\n") == 0UL);
    CHECK(h.has_range == 1 && h.range_from == 1024UL);
    CHECK(h.range_to == 0xFFFFFFFFUL);

    /* A suffix range sends the whole file; it is not an error. */
    fresh(&h);
    CHECK(parse(&h, "GET /f HTTP/1.1\r\nRange: bytes=-512\r\n\r\n") == 0UL);
    CHECK(h.has_range == 0);
    fresh(&h);
    CHECK(parse(&h, "GET /f HTTP/1.1\r\nRange: bytes=0-1,5-9\r\n\r\n")
          == 0UL);
    CHECK(h.has_range == 0);

    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nExpect: 100-continue\r\n\r\n")
          == 0UL);
    CHECK(h.expect == 1);
    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nExpect: 200-ok\r\n\r\n") == 417UL);
    CHECK(strcmp(h.reason, "that is not an expectation this server can meet")
          == 0);

    fresh(&h);
    CHECK(parse(&h, "GET /shell HTTP/1.1\r\n"
                    "Accept-Encoding: deflate, gzip;q=1.0\r\n\r\n") == 0UL);
    CHECK(h.gzip_ok == 1);
    fresh(&h);
    CHECK(parse(&h, "GET /shell HTTP/1.1\r\nAccept-Encoding: br\r\n\r\n")
          == 0UL);
    CHECK(h.gzip_ok == 0);

    /* Repeated field lines are one list. */
    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nIf-None-Match: \"1-2-3-4\"\r\n"
                    "If-None-Match: \"5-6-7-8\"\r\n\r\n") == 0UL);
    CHECK(strcmp(h.ifnone, "\"1-2-3-4\", \"5-6-7-8\"") == 0);
    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\nIf-Match: *\r\n\r\n") == 0UL);
    CHECK(strcmp(h.ifmatch, "*") == 0);

    /* A list that does not fit is refused, not cut. */
    fresh(&h);
    CHECK(parse(&h, "PUT /f HTTP/1.1\r\n"
                    "If-Match: \"1-2-3-4\", \"5-6-7-8\", \"9-10-11-12\", "
                    "\"13-14-15-16\", \"17-18-19-20\"\r\n\r\n") == 431UL);
    CHECK(strcmp(h.reason, "that If-Match list is longer than this server "
                           "reads") == 0);
}

static void test_head_shape(void)
{
    HttpHead h;
    char     head[4096];
    int      i;

    printf("the head's shape\n");

    fresh(&h);
    CHECK(parse(&h, "GET / HTTP/1.1\r\nNoColonHere\r\n\r\n") == 400UL);
    CHECK(strcmp(h.reason, "that is not an HTTP header") == 0);

    /* A folded continuation is not a header this server reads. */
    fresh(&h);
    CHECK(parse(&h, "GET / HTTP/1.1\r\nHost: a\r\n b\r\n\r\n") == 400UL);

    /* Whitespace after the colon is dropped and a tab is whitespace. */
    fresh(&h);
    CHECK(parse(&h, "GET / HTTP/1.1\r\nHost:\t   amiga  \r\n\r\n") == 0UL);
    CHECK(strcmp(h.host, "amiga  ") == 0);

    /* Headers this server does not act on are walked over. */
    fresh(&h);
    CHECK(parse(&h, "GET / HTTP/1.1\r\nUser-Agent: x\r\nX-Anything: y\r\n"
                    "Host: z\r\n\r\n") == 0UL);
    CHECK(strcmp(h.host, "z") == 0);

    /* HTTPD_HEADERS_MAX fit and one more does not. */
    strcpy(head, "GET / HTTP/1.1\r\n");
    for (i = 0; i < HTTPD_HEADERS_MAX; i++)
        strcat(head, "X-H: v\r\n");
    strcat(head, "\r\n");
    fresh(&h);
    CHECK(parse(&h, head) == 0UL);

    strcpy(head, "GET / HTTP/1.1\r\n");
    for (i = 0; i < HTTPD_HEADERS_MAX + 1; i++)
        strcat(head, "X-H: v\r\n");
    strcat(head, "\r\n");
    fresh(&h);
    CHECK(parse(&h, head) == 431UL);
    CHECK(strcmp(h.reason, "too many headers") == 0);

    /* A value longer than the scratch: Host keeps what fit, since nothing
       is decided from its tail, and a validator list is refused. */
    {
        unsigned long n;

        strcpy(head, "GET / HTTP/1.1\r\nHost: ");
        n = strlen(head);
        while (n < strlen("GET / HTTP/1.1\r\nHost: ") + HTTP_URL_MAX + 10)
            head[n++] = 'h';
        strcpy(&head[n], "\r\n\r\n");
        fresh(&h);
        CHECK(parse(&h, head) == 0UL);
        CHECK(strlen(h.host) == HTTPD_HOST_MAX - 1);

        strcpy(head, "PUT /f HTTP/1.1\r\nIf-None-Match: ");
        n = strlen(head);
        while (n < strlen("PUT /f HTTP/1.1\r\nIf-None-Match: ") +
                   HTTP_URL_MAX + 10)
            head[n++] = 'e';
        strcpy(&head[n], "\r\n\r\n");
        fresh(&h);
        CHECK(parse(&h, head) == 431UL);
        CHECK(strcmp(h.reason, "that If-None-Match list is longer than this "
                               "server reads") == 0);
    }
}

static void test_reset(void)
{
    HttpHead h;

    printf("reset between requests\n");

    fresh(&h);
    CHECK(parse(&h, "COPY /a HTTP/1.1\r\nHost: x\r\nDepth: 0\r\n"
                    "Destination: /b\r\nOverwrite: F\r\n"
                    "Transfer-Encoding: chunked\r\nExpect: 100-continue\r\n"
                    "Range: bytes=1-2\r\nAccept-Encoding: gzip\r\n"
                    "If-Match: *\r\nIf-None-Match: *\r\n"
                    "If: (<opaquelocktoken:t>)\r\nLock-Token: <u>\r\n"
                    "Timeout: Second-5\r\nConnection: close, upgrade\r\n"
                    "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
                    "Sec-WebSocket-Key: k\r\n\r\n") == 0UL);
    CHECK(h.keepalive == 0);

    http_head_reset(&h);
    CHECK(h.method[0] == '\0');
    CHECK(h.host[0] == '\0' && h.dest_url[0] == '\0');
    CHECK(h.depth == 1L && h.overwrite == 1);
    CHECK(h.chunk.state == HTTP_CHUNK_OFF && h.body_left == 0UL);
    CHECK(h.expect == 0 && h.has_range == 0 && h.gzip_ok == 0);
    CHECK(h.ifmatch[0] == '\0' && h.ifnone[0] == '\0');
    CHECK(h.ifhdr[0] == '\0' && h.iftoken[0][0] == '\0' &&
          h.iftoken[1][0] == '\0');
    CHECK(h.unlock_token[0] == '\0' && h.lock_secs == 0UL);
    CHECK(h.ws_upgrade == 0 && h.ws_connection == 0 && h.ws_version == 0);
    CHECK(h.ws_key[0] == '\0');
    CHECK(h.reason == 0 && h.note == 0);

    /* The connection's state is not the request's. */
    CHECK(h.keepalive == 0 && h.http11 == 1);
    h.keepalive = 1;
    http_head_reset(&h);
    CHECK(h.keepalive == 1 && h.http11 == 1);
}

int main(void)
{
    test_request_line();
    test_framing();
    test_connection();
    test_webdav_headers();
    test_conditions();
    test_head_shape();
    test_reset();

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
