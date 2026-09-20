/* HTTP and WebDAV date conversion for httpd.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPDATE_H
#define AMINETXDUO_HTTPDATE_H

#include <exec/types.h>
#include <dos/dos.h>

VOID  httpd_read_gmt_offset(VOID);
ULONG httpd_stamp_secs(const struct DateStamp *ds);
ULONG httpd_now(VOID);
VOID  httpd_rfc1123(ULONG secs, char *out);
VOID  httpd_iso8601(ULONG secs, char *out);
BOOL  httpd_parse_rfc1123(const char *text, struct DateStamp *ds);

#endif
