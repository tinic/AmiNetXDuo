/*
 * DONTROUTE's rule, for ping and traceroute (tool_on_link() in tool_diag.c):
 * a destination is reached without a router when it is on the loopback
 * network or inside the subnet of an interface that holds an address.
 * Plain C, so the host test runs the rule the commands run.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_ONLINK_H
#define AMINETXDUO_ONLINK_H

static inline int onlink_loopback(unsigned long addr)
{
    return (addr & 0xFF000000UL) == 0x7F000000UL;
}

static inline int onlink_subnet(unsigned long addr, unsigned long ifaddr,
                                unsigned long mask)
{
    return ifaddr != 0UL && ((ifaddr ^ addr) & mask) == 0UL;
}

#endif /* AMINETXDUO_ONLINK_H */
