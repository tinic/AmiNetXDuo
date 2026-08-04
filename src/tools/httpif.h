/*
 * httpif, the If: request header, evaluated.
 *
 * Separate from httpd.c, and including nothing, for the reason httppath.c is
 * separate: what this decides is whether a write goes ahead against a lock or
 * an entity tag somebody else holds, the grammar has three shapes that all
 * look alike, and an evaluator that always says yes reads exactly like one
 * that works.  So it is compiled for the host and driven by
 * src/tools/test/test_httpif.c.
 *
 * RFC 4918 10.4: lists are OR'd, the conditions inside one are AND'd, and a
 * Resource-Tag says which resource the lists after it are about.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPIF_H
#define AMINETXDUO_HTTPIF_H

#define HTTP_IF_TOKEN_MAX   64
#define HTTP_IF_ETAG_MAX    64
#define HTTP_IF_TAG_MAX    256

/* What a condition can ask about a resource, and all of it. */
typedef struct HttpIfState
{
    char token[HTTP_IF_TOKEN_MAX];  /* the lock on it, "" for none        */
    char etag[HTTP_IF_ETAG_MAX];    /* its entity tag, "" when it has none */
} HttpIfState;

/*
 * Fill *out for `tag`, a Resource-Tag out of the header, or "" for an
 * untagged list, which is about the request target.  Both fields arrive
 * empty, so a resource the caller cannot find needs nothing written.
 */
typedef void (*HttpIfLookup)(void *ctx, const char *tag, HttpIfState *out);

/*
 * Non-zero when the header holds.  A header with no list in it is false, not
 * true: a caller that must not evaluate an absent header checks for one
 * before calling.
 */
int http_if_eval(const char *header, HttpIfLookup lookup, void *ctx);

#endif /* AMINETXDUO_HTTPIF_H */
