/*
 * httphead, the request head: what every header this server acts on said.
 *
 * Split out of httpd.c for the reason httpframe.c is: one pass over the head
 * decides the method, the framing and every condition a write is checked
 * against, and a wrong decision here is answered as though it were right.
 * Includes only its portable siblings so src/tools/test/test_httphead.c can
 * compile the same code the m68k server runs.  httpd.c reaches proto/dos.h
 * and tx_api.h, and neither builds on a host.
 *
 * The parser fills the struct as it reads, exactly as the code did when it
 * wrote into the connection: a refusal leaves everything read before it in
 * place, which is what the refusal's Connection header and the drain after
 * it are decided from.  What the head means for the connection, the method
 * table, the reserved addresses and the path, stays in httpd.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPHEAD_H
#define AMINETXDUO_HTTPHEAD_H

#include "httppath.h"
#include "httpframe.h"
#include "httplock.h"

#define HTTPD_HEADERS_MAX     48    /* header lines in one request          */
#define HTTPD_METHOD_MAX      24    /* the method token, as text            */
#define HTTPD_HOST_MAX        80    /* Host:, which decides a Destination   */

/* An entity tag is four decimal numbers and three separators inside quotes. */
#define HTTPD_ETAG_MAX        48
/* The If: header, kept whole so it can be evaluated rather than skimmed. */
#define HTTPD_IF_MAX         256

#define HTTPD_LOCK_CAP     3600UL   /* the longest this server will hold one */

/* The version of RFC 6455 there is.  A client asking for another gets 426 and
   this number back, which is what 4.4 says to do rather than refusing flat. */
#define HTTPD_WS_VERSION    13

/* Sec-WebSocket-Key is 24 characters and the accept it produces is 28. */
#define HTTPD_WS_KEY_MAX    32

typedef struct HttpHead
{
    char           method[HTTPD_METHOD_MAX];
    unsigned char  http11;
    unsigned char  keepalive;
    long           depth;           /* 0, 1, or -1 for infinity            */
    unsigned long  body_left;
    unsigned char  has_range;
    unsigned long  range_from;
    unsigned long  range_to;        /* inclusive                           */
    unsigned char  expect;          /* the client is waiting for a 100     */
    unsigned char  overwrite;       /* COPY/MOVE: Overwrite was not F      */
    unsigned char  gzip_ok;         /* Accept-Encoding offered gzip         */
    HttpChunk      chunk;           /* HTTP_CHUNK_OFF unless it is chunked  */
    unsigned long  lock_secs;       /* Timeout: seconds asked for, 0 if none */
    char           host[HTTPD_HOST_MAX];    /* Host:, to tell a local Destination */
    char           ifmatch[HTTPD_ETAG_MAX]; /* If-Match:                   */
    char           ifnone[HTTPD_ETAG_MAX];  /* If-None-Match:              */
    char           dest_url[HTTP_URL_MAX];  /* Destination:, still as it arrived */
    char           iftoken[2][HTTPD_TOKEN_MAX];  /* the tokens inside If:  */
    char           ifhdr[HTTPD_IF_MAX];          /* and the whole of it    */
    char           unlock_token[HTTPD_TOKEN_MAX]; /* Lock-Token:           */
    unsigned char  ws_upgrade;      /* Upgrade: websocket was there        */
    unsigned char  ws_connection;   /* and Connection: listed upgrade      */
    unsigned short ws_version;
    char           ws_key[HTTPD_WS_KEY_MAX];

    /* The refusal, when http_head_parse() returned one.  `note` is a line for
       the verbose log and NULL when there is none. */
    const char    *reason;
    const char    *note;
} HttpHead;

/* Between requests.  http11 and keepalive are not touched: a refusal made
   before the version was read answers with the connection's current state. */
void http_head_reset(HttpHead *h);

/*
 * The request head, from the first byte to the blank line.  Zero when it was
 * read, or the status to refuse it with, `h->reason` saying why.  `target`
 * receives the request-target as it arrived and `value` is the scratch a
 * header value is collected in; both are the caller's because the server
 * shares one of each across its connections.
 */
unsigned long http_head_parse(HttpHead *h, const unsigned char *in,
                              unsigned long headlen,
                              char *target, unsigned long target_max,
                              char *value, unsigned long value_max);

#endif /* AMINETXDUO_HTTPHEAD_H */
