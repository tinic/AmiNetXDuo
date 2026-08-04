/* <netinet/ip_var.h> for the bsdsocket host tests.  struct ipstat is what
   NETSTATUS_ip answers with and its shape is ABI; tests/sockopt pins the size
   on the guest.  SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_NETINET_IP_VAR_H
#define AMINETXDUO_BSD_TEST_NETINET_IP_VAR_H
#include <exec/types.h>
struct ipstat {
    ULONG ips_total, ips_badsum, ips_tooshort, ips_toosmall, ips_badhlen;
    ULONG ips_badlen, ips_fragments, ips_fragdropped, ips_fragtimeout;
    ULONG ips_forward, ips_cantforward, ips_redirectsent, ips_noproto;
    ULONG ips_delivered, ips_localout, ips_odropped, ips_reassembled;
    ULONG ips_fragmented, ips_ofragments, ips_cantfrag, ips_badoptions;
    ULONG ips_noroute, ips_badvers, ips_rawout;
};
#endif
