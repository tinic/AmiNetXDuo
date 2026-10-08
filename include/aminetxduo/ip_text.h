/* Internal IP text formatters shared by config and bsdsocket.
 * SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_IP_TEXT_H
#define AMINETXDUO_IP_TEXT_H

#include <exec/types.h>

/* buf must hold 16 bytes. addr is in host order. Returns the text length,
   excluding the terminating NUL. The longest spelling is 15 bytes. */
ULONG ami_format_ip4(char *buf, ULONG addr);

/* buf must hold AMI_CFG_IP6_STRLEN (46) bytes and addr four non-NULL
   host-order words. Returns the text length, excluding the terminating NUL. */
ULONG ami_format_ip6(char *buf, const ULONG addr[4]);

#endif
