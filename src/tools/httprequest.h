/*
 * Small, portable HTTP request-value policies used by httpd.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPREQUEST_H
#define AMINETXDUO_HTTPREQUEST_H

/* The interactive endpoints' one query switch. */
int http_request_query_take(const char *target);

/* /shell's bounded slot selector: 0 if absent, 1 for session=1, -1 for a
   malformed, repeated or out-of-range session value. */
int http_request_query_session(const char *target);

/* Parse Infinite or Second-N without ever overflowing; values above cap are
   returned as cap.  Zero means no usable timeout was present. */
unsigned long http_request_timeout(const char *value, unsigned long cap);

/* Whether an Accept-Encoding list offers gzip at a non-zero quality. */
int http_request_accepts_gzip(const char *value);

/* Extract opaque lock tokens from an If: value into `count` fixed-width
   records.  Returns the number written; every record is NUL-terminated. */
unsigned long http_request_lock_tokens(const char *value, char *out,
                                       unsigned long stride,
                                       unsigned long count);

/* One decimal path segment of at most nine digits, or -1. */
long http_request_decimal(const char *s, unsigned long len);

/* A dotted quad, and nothing else.  Nonzero when `out` was filled. */
int http_request_dotted(const char *s, unsigned long *out);

#endif /* AMINETXDUO_HTTPREQUEST_H */
