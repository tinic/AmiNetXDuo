/*
 * <net/route.h> for the bsdsocket host tests.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_BSD_TEST_NET_ROUTE_H
#define AMINETXDUO_BSD_TEST_NET_ROUTE_H

#include <exec/types.h>
#include <netinet/in.h>

#define sin_len         sin_zero[0]

struct rt_metrics
{
    ULONG   rmx_locks;
    ULONG   rmx_mtu;
    ULONG   rmx_hopcount;
    ULONG   rmx_expire;
    ULONG   rmx_recvpipe;
    ULONG   rmx_sendpipe;
    ULONG   rmx_ssthresh;
    ULONG   rmx_rtt;
    ULONG   rmx_rttvar;
    ULONG   rmx_pksent;
};

struct rt_msghdr
{
    UWORD   rtm_msglen;
    UBYTE   rtm_version;
    UBYTE   rtm_type;
    UWORD   rtm_index;
    LONG    rtm_flags;
    LONG    rtm_addrs;
    LONG    rtm_pid;
    LONG    rtm_seq;
    LONG    rtm_errno;
    LONG    rtm_use;
    ULONG   rtm_inits;
    struct rt_metrics rtm_rmx;
};

#define RTM_VERSION     3

#define RTM_ADD         0x1
#define RTM_DELETE      0x2
#define RTM_CHANGE      0x3     /* Change Metrics or flags */
#define RTM_GET         0x4

#define RTV_MTU         0x1
#define RTV_HOPCOUNT    0x2
#define RTV_EXPIRE      0x4

#define RTF_UP          0x1
#define RTF_GATEWAY     0x2
#define RTF_HOST        0x4
#define RTF_REJECT      0x8
#define RTF_DYNAMIC     0x10
#define RTF_MODIFIED    0x20
#define RTF_DONE        0x40
#define RTF_MASK        0x80
#define RTF_CLONING     0x100
#define RTF_XRESOLVE    0x200
#define RTF_LLINFO      0x400
#define RTF_STATIC      0x800
#define RTF_BLACKHOLE   0x1000

#define RTA_DST         0x1
#define RTA_GATEWAY     0x2
#define RTA_NETMASK     0x4
#define RTA_GENMASK     0x8
#define RTA_IFP         0x10
#define RTA_IFA         0x20
#define RTA_AUTHOR      0x40
#define RTA_BRD         0x80

#endif
