/*
 * httpframe, how long a request body is, and where it ends.
 *
 * Separate from httpd.c, and including nothing, for the reason httppath.c is:
 * it is one of the parts that must not be wrong, and splitting it is what lets
 * a host test and a fuzzer compile it.  httpd.c reaches proto/dos.h and
 * tx_api.h, and neither builds on a host.
 *
 * A wrong length is a framing error.  It puts bytes the client meant as a body
 * where the parser looks for the next request line.  That is request
 * smuggling, and on this server it needs no attacker: a Content-Length that
 * overflowed, or a chunk size that did, leaves the rest of an upload sitting
 * in the buffer to be read as methods.  So everything here refuses rather than
 * guesses, and the caller closes the connection when it does.  A stream whose
 * framing is already lost cannot be resynchronised.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPFRAME_H
#define AMINETXDUO_HTTPFRAME_H

/* ------------------------------------------------------------ the length --- */

typedef enum HttpFrameResult
{
    HTTP_FRAME_OK = 0,
    HTTP_FRAME_EMPTY,           /* no digits at all                        */
    HTTP_FRAME_JUNK,            /* something after them that is not one    */
    HTTP_FRAME_OVERFLOW         /* more than a ULONG on the target holds   */
} HttpFrameResult;

/*
 * A Content-Length value.  Digits and nothing else: no sign, no space, no
 * trailing text, and refused rather than clamped when it will not fit in the
 * 32 bits the target counts a body in.
 *
 * "5abc" used to parse as 5 and "4294967306" as 10, and both of those leave
 * the remainder of the body in the socket.
 */
HttpFrameResult http_frame_length(const char *value, unsigned long *out);

/* A sentence for the log.  Never NULL. */
const char *http_frame_error(HttpFrameResult why);

/*
 * Whether a comma-separated HTTP token list contains `want`.  Tokens are
 * compared case-insensitively and as whole tokens, with optional whitespace
 * ignored at either end.  Connection is one such list: `close` has exactly
 * the same meaning in "keep-alive, close" as it has on its own.
 */
int http_frame_has_token(const char *value, const char *want);

/*
 * Validate the field-name at the start of one header line and return the
 * colon's offset.  Whitespace before the colon, a folded continuation, and a
 * line with no colon are malformed rather than headers the server can ignore.
 */
int http_frame_field_name(const char *line, unsigned long len,
                          unsigned long *colon);

/*
 * Whether an entity-tag is one member of a comma-separated validator list.
 * With `weak` nonzero, W/ and strong forms compare equal (If-None-Match).
 * With it zero, a weak member never matches a strong tag (If-Match).
 */
int http_frame_etag_listed(const char *list, const char *etag, int weak);

typedef enum HttpFrameVersion
{
    HTTP_VERSION_BAD = 0,
    HTTP_VERSION_10,
    HTTP_VERSION_11
} HttpFrameVersion;

/* An exact request-line HTTP-version token. */
HttpFrameVersion http_frame_version(const char *value, unsigned long len);

/* ---------------------------------------------------------- the encoding --- */

typedef enum HttpFrameCoding
{
    HTTP_TE_IDENTITY = 0,       /* the list was empty or said "identity"   */
    HTTP_TE_CHUNKED,            /* chunked, and chunked alone              */
    HTTP_TE_UNSUPPORTED         /* a coding this server cannot decode      */
} HttpFrameCoding;

/*
 * A Transfer-Encoding value, which is a comma-separated list.  Only a list
 * that is exactly "chunked" is chunked: RFC 7230 3.3.1 puts chunked last and
 * this server can undo nothing that would come before it, so "gzip, chunked"
 * is a body it cannot read and is refused rather than handed on half-decoded.
 *
 * The match is on the whole token.  A seven-character prefix test called
 * "chunkedX" chunked and missed "gzip, chunked" entirely.
 */
HttpFrameCoding http_frame_coding(const char *value);

/* ------------------------------------------------------------- the chunks --- */

enum
{
    HTTP_CHUNK_OFF = 0,         /* the body is a Content-Length one         */
    HTTP_CHUNK_SIZE,
    HTTP_CHUNK_DATA,
    HTTP_CHUNK_CRLF,
    HTTP_CHUNK_TRAILER,
    HTTP_CHUNK_DONE,
    HTTP_CHUNK_ERROR            /* the framing is lost, close              */
};

/*
 * 24 held a size line with room to spare and truncated anything longer into
 * something that still parsed.  Nothing legitimate is near it, eight hex
 * digits is the whole of a 32-bit count, so a line that does not fit is a
 * refusal now, and the buffer only has to be big enough to recognise one.
 */
#define HTTP_CHUNK_LINE     40

typedef struct HttpChunk
{
    unsigned char state;
    unsigned char n;            /* how much of `line` is filled            */
    unsigned long left;         /* still to come of the chunk being read   */
    unsigned long total;        /* body bytes handed to the sink so far    */
    char          line[HTTP_CHUNK_LINE];
} HttpChunk;

/* The decoded body bytes, with the caller's own context. */
typedef void (*HttpChunkSink)(void *ctx, const unsigned char *data, long len);

/* Ready to read a body.  http_chunk_off() leaves it not chunked. */
void http_chunk_start(HttpChunk *ch);
void http_chunk_off(HttpChunk *ch);

/*
 * Decode as much of `data` as belongs to the body, handing the bytes to
 * `sink`.  Survives being called one byte at a time: everything it is in the
 * middle of is in the HttpChunk.
 *
 * Returns how much of `data` was consumed.  Anything after that is the next
 * request, or, when the state is HTTP_CHUNK_ERROR, nothing the caller can
 * use.
 */
long http_chunk_feed(HttpChunk *ch, const unsigned char *data, long len,
                     HttpChunkSink sink, void *ctx);

#endif /* AMINETXDUO_HTTPFRAME_H */
