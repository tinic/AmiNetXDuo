/*
 * httppath -- the request target a client sent, turned into an AmigaOS path
 * or refused, plus the escaping the answer needs.
 *
 * Separate from httpd.c, and including nothing, for two reasons.  It is the
 * one part of a file server that must not be wrong -- everything a client can
 * reach outside the document root, it reaches through here -- so it is written
 * to be tested rather than reviewed, and tests/tools/httppath_test.c compiles
 * this file natively and drives it directly.  And it is written to the standard
 * a writing server needs, not the one a reading server would get away with: a
 * mistake here leaks a file today and destroys one the day DELETE lands.
 *
 * There are three ways out of a document root on this machine and a ported
 * Unix server guards against one of them:
 *
 *   ..          the one everybody checks.
 *   %2E%2E      the same thing, and invisible to a check made before decoding.
 *               So the decode happens first and the validation second, which
 *               is also why %2F becoming a separator is safe: it can only
 *               produce more segments, and every segment is checked.
 *   RAM:        AmigaOS's own.  A colon anywhere in a path makes everything
 *               before it a device or assign name, so "GET /RAM:foo" is not a
 *               file called "RAM:foo" under the root -- it is RAM DISK, and no
 *               amount of ../ checking sees it.
 *
 * A fourth is AmigaOS-specific and does not come from the client at all: a
 * doubled slash means the PARENT directory here, not an empty segment, so
 * "a//b" walks up.  Empty segments are dropped rather than passed on, and the
 * result is joined by this file rather than pasted together by the caller, so
 * no path this returns can contain one.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPPATH_H
#define AMINETXDUO_HTTPPATH_H

/*
 * 255 is what AmigaDOS packets carry a path in and 108 is the FileInfoBlock
 * name.  Both forms here are decoded, so neither has to hold the expansion
 * escaping causes; that happens in the caller's scratch on the way out.
 *
 * These are per-connection, so they are also a memory decision: 632 bytes of
 * HttpPath times the connection limit, on a machine that may have 1 MB.
 */
#define HTTP_PATH_MAX       256
#define HTTP_URL_MAX        256
#define HTTP_NAME_MAX       112

/* Deep enough for any real tree; a bound so a client cannot make us walk. */
#define HTTP_PATH_SEGMENTS  32

typedef enum HttpPathResult
{
    HTTP_PATH_OK = 0,
    HTTP_PATH_BAD_ESCAPE,       /* % not followed by two hex digits       */
    HTTP_PATH_CONTROL,          /* a control character, before or after   */
    HTTP_PATH_NOT_ABSOLUTE,     /* the target did not begin with /        */
    HTTP_PATH_PARENT,           /* a ".." segment                         */
    HTTP_PATH_DEVICE,           /* a ':' -- an AmigaOS device reference   */
    HTTP_PATH_BACKSLASH,        /* a '\' -- a separator to the client     */
    HTTP_PATH_TOO_LONG,
    HTTP_PATH_TOO_DEEP
} HttpPathResult;

typedef struct HttpPath
{
    char path[HTTP_PATH_MAX];   /* "Work:Docs/readme.txt", for Open()     */
    char url[HTTP_URL_MAX];     /* "/Docs/readme.txt", decoded, normalised */
    char name[HTTP_NAME_MAX];   /* "readme.txt", or "" at the root        */
    int  segments;              /* 0 means the document root itself       */
    int  trailing_slash;        /* the client wrote it as a collection    */
} HttpPath;

/*
 * Decode `target`, check it, and join it under `root`.  `target` is the
 * request line's second word exactly as it arrived: an absolute path, or the
 * absolute form ("http://host/path") that a proxy-aware client may send, with
 * or without a query string.
 *
 * Nothing is written to *out on failure.
 */
HttpPathResult http_path_resolve(const char *root, const char *target,
                                 HttpPath *out);

/* A sentence for the log.  Never NULL. */
const char *http_path_error(HttpPathResult why);

/*
 * Percent-encode a decoded path for an href.  '/' is kept as a separator;
 * everything outside the unreserved set is escaped, which on this machine
 * matters mostly for the spaces AmigaOS filenames are full of.
 *
 * Returns the length written, or 0 if it would not fit (and writes "").
 */
unsigned long http_url_escape(const char *path, char *out, unsigned long outlen);

/* &, < and > for XML text.  Same contract. */
unsigned long http_xml_escape(const char *text, char *out, unsigned long outlen);

/*
 * The Content-Type for a file name, from its suffix.  Never NULL; anything
 * unrecognised is application/octet-stream, which is what a client downloads
 * rather than tries to display.
 */
const char *http_content_type(const char *name);

#endif /* AMINETXDUO_HTTPPATH_H */
