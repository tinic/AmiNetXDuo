/*
 * httpws, RFC 6455 on the wire: the upgrade handshake and the frame codec.
 *
 * Separate from httpd.c, and including nothing, for the reason httpframe.c
 * and httppath.c are: it is one of the parts that must not be wrong, and
 * splitting it is what lets a host test compile it.  httpd.c reaches
 * proto/dos.h and tx_api.h and neither builds on a host.
 *
 * A decoder rather than a parser, because a frame boundary falls wherever TCP
 * put it.  The server reads 512 bytes at a time into a buffer it shares with
 * eleven other methods, so a two-byte frame header can arrive as two reads a
 * second apart and a paste of a kilobyte arrives as three frames in one read.
 * Everything the decoder is in the middle of therefore lives in HttpWsIn, and
 * http_ws_feed() takes whatever there is and says how much of it belonged to
 * the stream.
 *
 * Masking is not optional.  RFC 6455 5.1: every frame from a client is masked,
 * and a server that receives one that is not MUST fail the connection.  The
 * mask stops a hostile page from making a browser emit bytes that a
 * transparent proxy would read as a second HTTP request, which is the same
 * cache-poisoning shape httpframe.c exists to prevent on the other side.  So
 * an unmasked frame here is 1002 and the connection ends, never decoded.
 *
 * No extensions: RSV1..3 must be zero, so there is no permessage-deflate and
 * nothing to negotiate.  On a 68020 the compressor would cost more than the
 * LAN it saves, and an extension that is not implemented must be refused
 * rather than ignored, because ignoring one produces frames whose payload is
 * not what it says it is.
 *
 * Text payloads are UTF-8, including when one code point crosses a frame or
 * socket-read boundary.  The terminal behind this carries AmigaDOS output,
 * which is Latin-1 and not UTF-8, so the browser is sent binary frames in the
 * other direction.  Latin-1 in a text frame would not be the UTF-8 that a
 * text frame promises.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPWS_H
#define AMINETXDUO_HTTPWS_H

/* ---------------------------------------------------------- the handshake --- */

/*
 * The accept value for a Sec-WebSocket-Key, RFC 6455 4.2.2 step 5.
 *
 * `key` is the header value exactly as it arrived.  Leading and trailing
 * spaces are ignored and nothing else is.  It must be 24 characters of base64
 * decoding to exactly 16 bytes, which is the one thing about the key a server
 * can check.  The key is a nonce, not a credential, and it proves only that
 * whatever answered understood this protocol rather than echoing a cached 200.
 *
 * `out` receives 28 characters and a terminator, so it needs 29 bytes.
 * Returns 0 when the key is not one, having written nothing.
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
 * A piece of one message.  `ev` describes the message rather than the frame: a
 * continuation frame reports the opcode of the fragment that started it,
 * because that is the only thing a reader can act on.  `final` is true on the
 * last piece, which for a fragmented message is the last piece of the last
 * frame and not the end of each one.
 *
 * A control message is delivered in exactly one call with `final` true.
 * Control messages are never fragmented and never longer than
 * HTTP_WS_CTL_MAX.
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

    /* Non-zero once the stream has failed.  The value is the close code to
       send.  Nothing after it is decoded: a stream whose framing is lost
       cannot be resynchronised, exactly as httpframe.c says of a chunked
       body. */
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
 * The header of one frame this server sends, into `out`.  Server frames are
 * never masked (RFC 6455 5.1), so the header is 2, 4 or 10 bytes and the
 * payload follows it unchanged.
 *
 * Returns the header length, or 0 when `outlen` cannot hold it.
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
 * When a quiet peer counts as gone.  Both endpoints that hold a single-instance
 * resource, the Shell and the console screen, have to decide that, and they
 * had the same rule written out twice.  It is here once instead, as two pure
 * functions over the two fields a caller keeps: when it last heard from the
 * peer, and whether a ping is already outstanding.
 *
 * Hearing from the peer means anything the peer sent, and nothing this end
 * wrote.  A send that the local stack accepted says the socket buffer had
 * room.  It says nothing about whether anybody is on the other end.  The
 * console refreshed its progress on every successful send and so could
 * conclude nothing: a viewer that vanished without a close held the screen for
 * ever, measured at over 110 seconds and still counting.
 *
 * The budget is split in half.  The ping goes out at half the timeout and the
 * answer is due by the end of it, so the whole exchange is bounded by
 * `timeout` rather than the two timeouts in series it used to be.
 */

/* TRUE when a ping has gone unanswered for its half of the budget. */
int http_ws_live_stale(unsigned long progress, int pinged,
                       unsigned long now, unsigned long timeout);

/* TRUE when the peer has been quiet long enough to be asked. */
int http_ws_live_ping_due(unsigned long progress, int pinged,
                          unsigned long now, unsigned long timeout);

/*
 * SHA-1, which the accept is built on.  Declared here and implemented in
 * httpsha1.c over NetX Duo's own SHA-1.  This file must build on a host with
 * nothing but a C compiler, and httpsha1.c reaches the crypto library.
 */
void http_ws_sha1(const unsigned char *data, unsigned long len,
                  unsigned char out[20]);

#endif /* AMINETXDUO_HTTPWS_H */
