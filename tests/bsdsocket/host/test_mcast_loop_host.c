/* Sender-side IPv4 multicast loopback against the shipping mcast.c helper. */
#include <stdio.h>
#include <string.h>
#include "bsdsocket_vectors.h"

static unsigned locks;
static unsigned unlocks;

UINT _txe_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (VOID)mutex_ptr;
    (VOID)wait_option;
    locks++;
    return TX_SUCCESS;
}

UINT _txe_mutex_put(TX_MUTEX *mutex_ptr)
{
    (VOID)mutex_ptr;
    unlocks++;
    return TX_SUCCESS;
}

/* Unused mcast entry points disappear with section garbage collection. */
#include "../../../src/bsdsocket/mcast.c"

static unsigned checks;
static unsigned failures;

static void check(int condition, const char *name)
{
    checks++;
    if (!condition)
    {
        failures++;
        printf("FAIL %s\n", name);
    }
}

int main(void)
{
    static NX_IP ip;
    AmiSocket sender;
    NXD_ADDRESS addr;
    BsdMcastLoopGuard guard;
    NX_IPV4_MULTICAST_ENTRY *entry = &ip.nx_ipv4_multicast_entry[0];

    memset(&ip, 0, sizeof ip);
    memset(&sender, 0, sizeof sender);
    memset(&addr, 0, sizeof addr);
    addr.nxd_ip_version = NX_IP_VERSION_V4;
    addr.nxd_ip_address.v4 = 0xefff2a63UL;
    entry->nx_ipv4_multicast_join_list = addr.nxd_ip_address.v4;
    entry->nx_ipv4_multicast_loopback_enable = NX_FALSE;

    /* This sender did not join; its option still governs delivery to the
       receiver whose join supplied the entry. */
    sender.as_McastLoop = 1;
    bsd_mcast_loop_begin(&ip, &sender, &addr, &guard);
    check(entry->nx_ipv4_multicast_loopback_enable == NX_TRUE,
          "nonjoining sender enables local loopback");
    check(locks == 1 && unlocks == 0,
          "IP mutex stays held across synchronous send");
    bsd_mcast_loop_end(&guard);
    check(entry->nx_ipv4_multicast_loopback_enable == NX_FALSE,
          "first sender restores receiver group setting");
    check(locks == unlocks, "first sender releases IP mutex");

    /* setsockopt after join has to take effect on the very next send. */
    entry->nx_ipv4_multicast_loopback_enable = NX_TRUE;
    sender.as_McastLoop = 0;
    bsd_mcast_loop_begin(&ip, &sender, &addr, &guard);
    check(entry->nx_ipv4_multicast_loopback_enable == NX_FALSE,
          "later sender option disables local loopback");
    bsd_mcast_loop_end(&guard);
    check(entry->nx_ipv4_multicast_loopback_enable == NX_TRUE,
          "later send restores group setting");

    /* NetX's global join default is not touched by a BSD sender. */
    check(ip.nx_ip_igmp_global_loopback_enable == NX_FALSE,
          "sender does not pollute future mDNS joins");

    addr.nxd_ip_address.v4 = 0xefff2a64UL;
    bsd_mcast_loop_begin(&ip, &sender, &addr, &guard);
    check(guard.flag == NULL && locks == unlocks,
          "unjoined destination needs no lingering mutex");
    bsd_mcast_loop_end(&guard);

    addr.nxd_ip_version = NX_IP_VERSION_V6;
    bsd_mcast_loop_begin(&ip, &sender, &addr, &guard);
    check(guard.flag == NULL && locks == unlocks,
          "IPv6 send does not touch IPv4 group state");
    bsd_mcast_loop_end(&guard);

    printf("mcast sender loop: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
