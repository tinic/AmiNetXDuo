/*
 * httpframe, how long a request body is, and where it ends.
 *
 * Separate from httpd.c, and including nothing, for the reason httppath.c is:
 * it is one of the parts that must not be wrong, and splitting it is what lets
 * a host test and a fuzzer compile it.  httpd.c reaches proto/dos.h and
 * tx_api.h, and neither builds on a host.
 *
 * Getting the length wrong is not a parse error, it is a framing error, and a
 * framing error puts bytes the client meant as a body where the parser looks
 * for the next request line.  That is request smuggling, and on this server it
 * needs no attacker: a Content-Length that overflowed, or a chunk size that
 * did, leaves the rest of an upload sitting in the buffer to be read as
 * methods.  So everything here refuses rather than guesses, and the caller
 * closes the connection when it does, there is no way to resynchronise a
 * stream whose framing is already lost.
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
 * "chunkedX" chunked and missed "gzip, chunked" entirely, which are the two
 * halves of the same mistake.
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
    HTTP_CHUNK_ERROR            /* the framing is lost; close              */
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

/* Everything a sink is handed, with whatever the caller wants to know. */
typedef void (*HttpChunkSink)(void *ctx, const unsigned char *data, long len);

/* Ready to read a body.  `off` leaves it saying "this body is not chunked". */
void http_chunk_start(HttpChunk *ch);
void http_chunk_off(HttpChunk *ch);

/*
 * Decode as much of `data` as belongs to the body, handing the bytes to
 * `sink`.  Survives being called one byte at a time: everything it is in the
 * middle of is in the HttpChunk.
 *
 * Returns how much of `data` was consumed.  Anything after that is the next
 * request, or, when the state is HTTP_CHUNK_ERROR, nothing the caller may
 * use.
 */
long http_chunk_feed(HttpChunk *ch, const unsigned char *data, long len,
                     HttpChunkSink sink, void *ctx);

#endif /* AMINETXDUO_HTTPFRAME_H */
