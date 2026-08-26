/*
 * AmiNetXDuo, the ThreadX priority ladder, in one place.
 *
 * ThreadX counts DOWN: lowest number wins. The ordering is declared once here
 * and asserted below; both internal headers include this and neither defines a
 * priority.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_THREAD_PRIORITIES_H
#define AMINETXDUO_THREAD_PRIORITIES_H

/*
 * The SANA-II readers sit at the top. The device has no buffers of its own and
 * drops any frame arriving with no CMD_READ outstanding, so a reader that
 * cannot get the CPU during a burst loses packets on the wire and the far end
 * retransmits. Nothing may preempt them, least of all the IP thread, which
 * has continuous work during a bulk transfer and would otherwise starve the
 * threads feeding it.
 */
#define AMI_SANA2_RX_PRIORITY       1

/* The IP thread outranks everything that CONSUMES packets, and nothing else. */
#define AMI_IP_THREAD_PRIORITY      2

#define AMI_AUTOIP_PRIORITY         3

/*
 * The mDNS thread wakes on a query, walks two small caches and answers. Below
 * the SANA-II readers and the IP thread so an inbound burst still drains, but
 * above an adopted application task: a responder that ran only when nothing
 * else wanted the CPU would answer after the querier gave up. Next to AutoIP,
 * which has the same urgency.
 */
#define AMI_MDNS_PRIORITY           4

/*
 * DHCPv6, two threads.
 *
 * The client's own thread is next to AutoIP and mDNS: it wakes on a tick,
 * decides whether a retransmission is due and usually goes back to sleep, and
 * it must not preempt the IP thread it depends on. NX_DHCPV6_THREAD_PRIORITY
 * in the vendored header defaults to 2, which IS the IP thread's priority --
 * see the override in port/netxduo-amiga/inc/nx_user.h, which is where the
 * number the client is created with comes from.
 *
 * The deferred-work thread is below it and does nothing but wait on an event
 * flag. A router advertisement arrives on the IP thread, and every call that
 * moves the DHCPv6 client's state machine blocks -- _nx_dhcpv6_request()
 * sleeps a tick at a time until the client thread is idle, and creating the
 * client binds a UDP socket with TX_WAIT_FOREVER. Doing either on the IP
 * thread stalls the thread the DHCPv6 client needs in order to become idle,
 * which is a deadlock, not a slow path.
 */
#define AMI_DHCPV6_PRIORITY         4
#define AMI_DHCPV6_WORK_PRIORITY    5

#define AMI_CALLER_PRIORITY        16      /* adopted application tasks      */

/*
 * The invariants, so that the next edit to the numbers above fails to compile
 * instead of costing another 6.6%.
 */
#if AMI_SANA2_RX_PRIORITY >= AMI_IP_THREAD_PRIORITY
#error "SANA-II readers must outrank the IP thread (ThreadX: lower number wins)"
#endif
#if AMI_IP_THREAD_PRIORITY >= AMI_CALLER_PRIORITY
#error "the IP thread must outrank adopted callers"
#endif
#if AMI_IP_THREAD_PRIORITY >= AMI_AUTOIP_PRIORITY
#error "the IP thread must outrank AutoIP"
#endif
#if AMI_IP_THREAD_PRIORITY >= AMI_MDNS_PRIORITY
#error "the IP thread must outrank the mDNS responder"
#endif
#if AMI_MDNS_PRIORITY >= AMI_CALLER_PRIORITY
#error "the mDNS responder must outrank adopted callers"
#endif
#if AMI_MDNS_PRIORITY < AMI_AUTOIP_PRIORITY
#error "the mDNS responder must not outrank AutoIP"
#endif
#if AMI_DHCPV6_PRIORITY <= AMI_IP_THREAD_PRIORITY
#error "the IP thread must outrank the DHCPv6 client"
#endif
#if AMI_DHCPV6_WORK_PRIORITY <= AMI_DHCPV6_PRIORITY
#error "the DHCPv6 client must outrank the thread that drives it"
#endif
#if AMI_DHCPV6_WORK_PRIORITY >= AMI_CALLER_PRIORITY
#error "the DHCPv6 deferred-work thread must outrank adopted callers"
#endif

#endif /* AMINETXDUO_THREAD_PRIORITIES_H */
