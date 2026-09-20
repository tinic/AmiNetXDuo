/*
 * Small, portable HTTP request-value policies used by httpd.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPREQUEST_H
#define AMINETXDUO_HTTPREQUEST_H

/* The interactive endpoints' one query switch. */
int http_request_query_take(const char *target);

/* Parse Infinite or Second-N without ever overflowing; values above cap are
   returned as cap.  Zero means no usable timeout was present. */
unsigned long http_request_timeout(const char *value, unsigned long cap);

/* Whether an Accept-Encoding list offers gzip at a non-zero quality. */
int http_request_accepts_gzip(const char *value);

#endif /* AMINETXDUO_HTTPREQUEST_H */
