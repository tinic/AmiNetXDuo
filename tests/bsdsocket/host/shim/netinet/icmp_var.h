/* <netinet/icmp_var.h> for the bsdsocket host tests.  struct icmpstat, the
   companion to ipstat, and ABI for the same reason.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_NETINET_ICMP_VAR_H
#define AMINETXDUO_BSD_TEST_NETINET_ICMP_VAR_H
#include <exec/types.h>
#define ICMP_MAXTYPE 18
struct icmpstat {
    ULONG icps_error, icps_oldshort, icps_oldicmp;
    ULONG icps_outhist[ICMP_MAXTYPE + 1];
    ULONG icps_badcode, icps_tooshort, icps_checksum, icps_badlen;
    ULONG icps_reflect;
    ULONG icps_inhist[ICMP_MAXTYPE + 1];
};
#endif
