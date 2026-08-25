/*
 * httpws, RFC 6455 on the wire: the upgrade handshake and the frame codec.
 * Includes nothing, so a host test can compile it. Masking is not optional
 * (5.1) -- an unmasked client frame is 1002. No extensions: RSV1..3 must be 0.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPWS_H
#define AMINETXDUO_HTTPWS_H

/* ---------------------------------------------------------- the handshake --- */

/*
 * The accept value for a Sec-WebSocket-Key, RFC 6455 4.2.2 step 5. `key` is the
 * header value exactly as it arrived; it must be 24 characters of base64
 * decoding to exactly 16 bytes. `out` needs 29 bytes. 0 when the key is not one.
 */
int http_ws_accept(const char *key, char *out, unsigned long outlen);

/* base64, exposed because the accept is built from it and a test that cannot
   see it can only check the pair the RFC happens to print. */
unsigned long http_ws_b64_encode(const unsigned char *src, unsigned long len,
                                 char *out, unsigned long outlen);

/* Bytes written, or -1 when `text` is not base64 or does not fit. */
long http_ws_b64_decode(const char *text, unsigned char *out,
                        unsigned long outlen);

/* ------------------------------------------------------------- the frames --- */

typedef enum HttpWsEvent
{
    HTTP_WS_EV_NONE = 0,
    HTTP_WS_EV_TEXT,            /* opcode 1                                */
    HTTP_WS_EV_BINARY,          /* opcode 2                                */
    HTTP_WS_EV_CLOSE,           /* opcode 8                                */
    HTTP_WS_EV_PING,            /* opcode 9                                */
    HTTP_WS_EV_PONG             /* opcode 10                               */
} HttpWsEvent;

/*
 * The close codes this decoder produces, from RFC 6455 7.4.1.  Only the ones a
 * server generates on its own are here.  The rest are the client's to send.
 */
#define HTTP_WS_CLOSE_NORMAL    1000
#define HTTP_WS_CLOSE_GOING     1001
#define HTTP_WS_CLOSE_PROTOCOL  1002
#define HTTP_WS_CLOSE_DATA      1007
#define HTTP_WS_CLOSE_TOOBIG    1009

/* The longest control frame payload there is, RFC 6455 5.5.  A control frame
   is delivered whole, so this is a buffer in the decoder rather than a limit
   somebody has to remember. */
#define HTTP_WS_CTL_MAX         125

/*
 * A piece of one message. `ev` describes the message rather than the frame: a
 * continuation frame reports the opcode of the fragment that started it. A
 * control message arrives in exactly one call with `final` true.
 */
typedef void (*HttpWsSink)(void *ctx, HttpWsEvent ev, const unsigned char *data,
                           long len, int final);

typedef struct HttpWsIn
{
    unsigned char   state;
    unsigned char   hdr[14];        /* the frame header as it arrives      */
    unsigned char   hdr_n;
    unsigned char   hdr_need;

    unsigned char   opcode;         /* of the frame being read             */
    unsigned char   fin;
    unsigned char   mask[4];
    unsigned long   left;           /* payload bytes of it still to come   */
    unsigned char   maskpos;

    unsigned char   msg;            /* HttpWsEvent of the message in flight */
    unsigned long   msg_len;        /* bytes across all of its fragments    */
    unsigned char   utf8_need;      /* continuation bytes still required    */
    unsigned char   utf8_lo;        /* bounds for the next continuation     */
    unsigned char   utf8_hi;
    unsigned char   ctl_n;
    unsigned char   ctl[HTTP_WS_CTL_MAX];

    /* Non-zero once the stream has failed; the value is the close code to send.
       Nothing after it is decoded -- lost framing cannot be resynchronised. */
    unsigned short  failed;
} HttpWsIn;

/* A ceiling on one assembled message.  It does not bound a frame, since frames
   are streamed to the sink as they arrive.  It bounds a client that opens a
   fragmented message and never finishes it. */
#define HTTP_WS_MSG_MAX     65536UL

void http_ws_reset(HttpWsIn *in);

/*
 * Feed bytes.  Returns how many were consumed, which is `len` unless the
 * stream failed inside them.  When it failed, the caller must stop reading and
 * close, and `in->failed` is the code that says why.
 */
long http_ws_feed(HttpWsIn *in, const unsigned char *data, long len,
                  HttpWsSink sink, void *ctx);

/* A sentence for the log.  Never NULL. */
const char *http_ws_close_reason(unsigned short code);

/* ------------------------------------------------------------ the encoder --- */

/*
 * The header of one frame this server sends, into `out`. Server frames are
 * never masked (RFC 6455 5.1), so the header is 2, 4 or 10 bytes and the
 * payload follows it unchanged. Returns the header length, or 0 if it will not fit.
 */
unsigned long http_ws_head(unsigned char *out, unsigned long outlen,
                           HttpWsEvent ev, unsigned long len, int final);

/*
 * A whole close frame, header and payload, into `out`.  `reason` can be NULL.
 * Returns the total length, or 0 when it does not fit.
 */
unsigned long http_ws_close_frame(unsigned char *out, unsigned long outlen,
                                  unsigned short code, const char *reason);

/* --------------------------------------------------------- peer liveness --- */

/*
 * When a quiet peer counts as gone. Hearing from the peer means anything the
 * peer sent and nothing this end wrote. The budget is split in half: the ping
 * goes out at half the timeout and the answer is due by the end of it.
 */

/* TRUE when a ping has gone unanswered for its half of the budget. */
int http_ws_live_stale(unsigned long progress, int pinged,
                       unsigned long now, unsigned long timeout);

/* TRUE when the peer has been quiet long enough to be asked. */
int http_ws_live_ping_due(unsigned long progress, int pinged,
                          unsigned long now, unsigned long timeout);

/* SHA-1, which the accept is built on. Implemented in httpsha1.c over NetX
   Duo's own SHA-1, because this file must build with nothing but a C compiler. */
void http_ws_sha1(const unsigned char *data, unsigned long len,
                  unsigned char out[20]);

#endif /* AMINETXDUO_HTTPWS_H */
