/*
 * fetchurl -- the part of `fetch` that is string work: the URL, the redirect
 * target resolved against it, and the framing of a response's header block.
 *
 * Its own file for the reason httppath.c is: these are the pieces whose
 * mistakes do not look like mistakes.  A relative Location: resolved as an
 * absolute URL asks the resolver for a host called "about.html"; an interim
 * 1xx taken for a final response writes the real response into the user's
 * file and reports success.  Both read as working code.  So this translation
 * unit includes nothing, is compiled natively by
 * src/tools/test/test_fetchurl.c, and carries the RFC 3986 section 5.4
 * reference-resolution examples as a test rather than as a claim.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_FETCHURL_H
#define AMINETXDUO_FETCHURL_H

#define FETCH_HOST_MAX          128
#define FETCH_PATH_MAX          512
#define FETCH_URL_MAX           640

/* "host" or "host:65535", plus the terminator. */
#define FETCH_AUTHORITY_MAX     (FETCH_HOST_MAX + 8)

/*
 * 3072 was too small: some front ends answer a plain GET with more than that
 * in headers alone, so a good 301 arrived and was refused.  Running out is not
 * fatal -- see `truncated` below.
 */
#define FETCH_HEAD_MAX          16384

typedef enum FetchUrlResult
{
    FETCH_URL_OK = 0,
    FETCH_URL_SCHEME,           /* not an http: or https: URL             */
    FETCH_URL_NO_HOST,          /* no authority to connect to             */
    FETCH_URL_HOST_LONG,
    FETCH_URL_PATH_LONG,
    FETCH_URL_BRACKET,          /* an IPv6 literal with no closing ]      */
    FETCH_URL_PORT,             /* the port is not a number               */
    FETCH_URL_PORT_RANGE,
    FETCH_URL_USERINFO          /* "user@" -- RFC 9110 section 4.2.4      */
} FetchUrlResult;

typedef struct FetchUrl
{
    int             secure;
    unsigned short  port;
    char            host[FETCH_HOST_MAX];   /* no port, no userinfo; an
                                               IPv6 literal keeps its []  */
    char            path[FETCH_PATH_MAX];   /* origin-form, query and all,
                                               never a #fragment          */
} FetchUrl;

/*
 * An absolute http: or https: URL.  A bare "host/path" is taken as http, which
 * is what a user typing at the Shell means.  The fragment is dropped: RFC 9110
 * section 7.1 makes the target URI everything but the fragment.
 */
FetchUrlResult fetch_url_parse(const char *url, FetchUrl *out);

/*
 * RFC 3986 section 5.2.2, strict: resolve `ref` -- a Location: value, absolute
 * or relative -- against `base`.  `out` may alias neither argument.
 *
 * Strict means a reference that names its own scheme is not merged with the
 * base even when the two schemes match, so "http:g" resolves to the path-only
 * URI "http:g" and is then refused for having no authority, per the note in
 * section 5.4.2.  Nothing sends that; a browser's leniency there buys
 * compatibility this command has no user for.
 */
FetchUrlResult fetch_url_resolve(const FetchUrl *base, const char *ref,
                                 FetchUrl *out);

/*
 * The Host: field value: "host", or "host:port" when the port is not the
 * scheme's default.  RFC 9112 section 3.2 makes it identical to the target
 * URI's authority, so a non-default port is not optional.
 *
 * 0 when it would not fit.
 */
int fetch_url_authority(const FetchUrl *u, char *dst, unsigned long dstlen);

/* A sentence for the user.  Never NULL. */
const char *fetch_url_error(FetchUrlResult why);


/* ------------------------------------------------------- the header block --- */

/*
 * The status line and headers of one response, accumulated across reads.
 *
 * Running past `size` stops us keeping headers, not reading them: the end of
 * the block is found from the last four bytes seen, not the last four stored,
 * so a chatty server costs the tail of its headers instead of the whole
 * transfer.  The status line and Location: are at the front; a caller that
 * needs the tail checks `truncated`.
 */
typedef struct FetchHead
{
    char          *buf;
    unsigned long  size;
    unsigned long  len;
    int            truncated;
    int            complete;
    char           tail[4];
} FetchHead;

void fetch_head_start(FetchHead *h, char *buf, unsigned long size);

/*
 * Feed response bytes.  Returns how many were taken: all of them, or all of
 * them up to and including the blank line that ends the block, at which point
 * `complete` is set and what is left belongs to the caller.
 */
unsigned long fetch_head_feed(FetchHead *h, const unsigned char *data,
                              unsigned long len);

/* The number out of "HTTP/1.1 301 Moved Permanently", or 0. */
unsigned long fetch_head_status(const FetchHead *h);

/*
 * Non-zero for 100..199.  RFC 9110 section 15.2: "A client MUST be able to
 * parse one or more 1xx responses received prior to a final response, even if
 * the client does not expect one."  Restart the block and keep reading.
 */
int fetch_head_interim(unsigned long status);

/* The value of a header, case-insensitively, or NULL.  `name` carries the
   colon: "location:". */
const char *fetch_head_field(const FetchHead *h, const char *name);

/* Copy one header value, stopping at the end of its line.  0 when it would
   not fit, or when the value is empty. */
int fetch_head_value(const char *value, char *dst, unsigned long dstlen);

/* The status line on its own, for the summary. */
void fetch_head_first_line(const FetchHead *h, char *dst, unsigned long dstlen);

#endif /* AMINETXDUO_FETCHURL_H */
