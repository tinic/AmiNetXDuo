/* RFC 3542 ICMPv6 filter API. SPDX-License-Identifier: MIT */
#ifndef _NETINET_ICMP6_H
#define _NETINET_ICMP6_H
#include <netinet/in.h>
#define ICMP6_FILTER 1
struct icmp6_filter
{
    unsigned int           icmp6_filt[8];  /* 8*32 = 256 bits, one per ICMPv6 type */
};

#define ICMP6_FILTER_WILLPASS(type, filterp) \
    ((((filterp)->icmp6_filt[((type) & 0xFF) >> 5]) & \
      (1UL << ((type) & 31))) != 0)
#define ICMP6_FILTER_WILLBLOCK(type, filterp) \
    ((((filterp)->icmp6_filt[((type) & 0xFF) >> 5]) & \
      (1UL << ((type) & 31))) == 0)
#define ICMP6_FILTER_SETPASS(type, filterp) \
    ((((filterp)->icmp6_filt[((type) & 0xFF) >> 5]) |= (1UL << ((type) & 31))))
#define ICMP6_FILTER_SETBLOCK(type, filterp) \
    ((((filterp)->icmp6_filt[((type) & 0xFF) >> 5]) &= ~(1UL << ((type) & 31))))

/* Spelled out rather than memset(), so <string.h> is not dragged in. */
#define ICMP6_FILTER_SETPASSALL(filterp)                        \
    do {                                                        \
        int _i_;                                                \
        for (_i_ = 0; _i_ < 8; _i_++)                           \
            (filterp)->icmp6_filt[_i_] = 0xFFFFFFFFUL;          \
    } while (0)
#define ICMP6_FILTER_SETBLOCKALL(filterp)                       \
    do {                                                        \
        int _i_;                                                \
        for (_i_ = 0; _i_ < 8; _i_++)                           \
            (filterp)->icmp6_filt[_i_] = 0UL;                   \
    } while (0)


#endif
