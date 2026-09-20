/* Small bounded string operations shared by the HTTP server.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPSTR_H
#define AMINETXDUO_HTTPSTR_H

#include <exec/types.h>

ULONG hs_len(const char *s);
BOOL  hs_append(char *dst, ULONG dstlen, ULONG *used, const char *src);
BOOL  hs_append_num(char *dst, ULONG dstlen, ULONG *used, ULONG value);
int   hs_nicmp(const char *a, const char *b, ULONG n);
BOOL  hs_equal(const char *a, const char *b);
VOID  hs_copy(char *dst, ULONG dstlen, const char *src);

#endif /* AMINETXDUO_HTTPSTR_H */
