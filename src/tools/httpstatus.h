/*
 * httpstatus, the status-code to reason-phrase table for httpd.
 *
 * Split out of httpd.c on the rule httphead.c is: a code in and the standard
 * reason phrase out, a table whose wrong case or a phrase that lost its
 * second half never fails to compile but answers a status line wrongly.  As
 * its own translation unit src/tools/test/test_httpstatus.c can assert every
 * mapped code and the unknown fallback string-for-string, without compiling
 * httpd.c, which reaches proto/dos.h and tx_api.h and builds nowhere but the
 * target.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPSTATUS_H
#define AMINETXDUO_HTTPSTATUS_H

/* The reason phrase that follows a status code in a status line, or "Unknown"
   when the server has no phrase for the code.  Static storage. */
const char *http_status_reason(unsigned long status);

#endif /* AMINETXDUO_HTTPSTATUS_H */
