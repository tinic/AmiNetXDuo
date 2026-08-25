/*
 * RFC 6724 source address selection, driven directly against a hand-built
 * NX_IP with no stack running.  Every rule has a fixture that fails if the
 * rule is deleted; that is why several fixtures look contrived.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_ipv6.h"

#include "ipv6_srcsel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ------------------------------------------------------------- harness ---- */

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;

    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}


/* --------------------------------------------------------------- stubs ---- */

/* NX_ASSERT's failure arm, an endless sleep on a target: it must end the run
   here rather than hang it. */
UINT _tx_thread_sleep(ULONG timer_ticks)
{
    NX_PARAMETER_NOT_USED(timer_ticks);

    printf("FAIL an NX_ASSERT fired\n");
    exit(1);
}


/* ----------------------------------------------------------- the fixture -- */

static NX_IP h_ip;

#define H_ETH0          0
#define H_ETH1          1

#define H_SLOT0         0
#define H_SLOT1         1
#define H_SLOT2         2

static NX_INTERFACE *h_if(UINT index)
{
    return &h_ip.nx_ip_interface[index];
}

static void h_reset(void)
{
UINT i;

    memset(&h_ip, 0, sizeof(h_ip));

    for (i = 0; i < NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        h_ip.nx_ip_interface[i].nx_interface_valid   = NX_TRUE;
        h_ip.nx_ip_interface[i].nx_interface_link_up = NX_TRUE;
        h_ip.nx_ip_interface[i].nx_interface_index   = (UCHAR)i;
    }

    h_ip.nx_ip_interface[H_ETH0].nx_interface_name = "eth0";
    h_ip.nx_ip_interface[H_ETH1].nx_interface_name = "eth1";

#ifndef NX_DISABLE_LOOPBACK_INTERFACE
    h_ip.nx_ip_interface[NX_LOOPBACK_INTERFACE].nx_interface_valid   = NX_TRUE;
    h_ip.nx_ip_interface[NX_LOOPBACK_INTERFACE].nx_interface_link_up = NX_TRUE;
    h_ip.nx_ip_interface[NX_LOOPBACK_INTERFACE].nx_interface_name    = "loopback";
    h_ip.nx_ip_interface[NX_LOOPBACK_INTERFACE].nx_interface_index   =
        NX_LOOPBACK_INTERFACE;

    h_ip.nx_ipv6_address[NX_LOOPBACK_IPV6_SOURCE_INDEX].nxd_ipv6_address_valid = NX_TRUE;
    h_ip.nx_ipv6_address[NX_LOOPBACK_IPV6_SOURCE_INDEX].nxd_ipv6_address_state =
        NX_IPV6_ADDR_STATE_VALID;
    h_ip.nx_ipv6_address[NX_LOOPBACK_IPV6_SOURCE_INDEX].nxd_ipv6_address_prefix_length = 128;
    h_ip.nx_ipv6_address[NX_LOOPBACK_IPV6_SOURCE_INDEX].nxd_ipv6_address_attached =
        &h_ip.nx_ip_interface[NX_LOOPBACK_INTERFACE];
    h_ip.nx_ipv6_address[NX_LOOPBACK_IPV6_SOURCE_INDEX].nxd_ipv6_address[3] = 1;
#endif /* NX_DISABLE_LOOPBACK_INTERFACE */
}

static void h_addr(UINT slot, UINT iface,
                   ULONG w0, ULONG w1, ULONG w2, ULONG w3,
                   UINT prefix_length, UINT state)
{
NXD_IPV6_ADDRESS *a = &h_ip.nx_ipv6_address[slot];

    a -> nxd_ipv6_address_valid         = NX_TRUE;
    a -> nxd_ipv6_address_type          = NX_IP_VERSION_V6;
    a -> nxd_ipv6_address_state         = (UCHAR)state;
    a -> nxd_ipv6_address_prefix_length = (UCHAR)prefix_length;
    a -> nxd_ipv6_address_index         = (UCHAR)slot;
    a -> nxd_ipv6_address_attached      = h_if(iface);
    a -> nxd_ipv6_address[0]            = w0;
    a -> nxd_ipv6_address[1]            = w1;
    a -> nxd_ipv6_address[2]            = w2;
    a -> nxd_ipv6_address[3]            = w3;
}

static void h_router(UINT iface)
{
    h_ip.nx_ipv6_default_router_table[0].nx_ipv6_default_router_entry_flag = 1;
    h_ip.nx_ipv6_default_router_table[0].nx_ipv6_default_router_entry_interface_ptr =
        h_if(iface);
}

static void h_set(ULONG *addr, ULONG w0, ULONG w1, ULONG w2, ULONG w3)
{
    addr[0] = w0;
    addr[1] = w1;
    addr[2] = w2;
    addr[3] = w3;
}

/* Returns the slot selected, -1 when refused. */
static int h_select(ULONG *dest, NX_INTERFACE *if_ptr)
{
NXD_IPV6_ADDRESS *chosen = NX_NULL;

    if (_nxd_ipv6_interface_find(&h_ip, dest, &chosen, if_ptr) != NX_SUCCESS)
    {
        return -1;
    }

    if (chosen == NX_NULL)
    {
        return -2;
    }

    /* From the pointer, not nxd_ipv6_address_index: the loopback slot NetX Duo
       sets up itself does not carry one. */
    return (int)(chosen - &h_ip.nx_ipv6_address[0]);
}


/* ----------------------------------------------------------- the §2.1 table */

static void test_policy_table(void)
{
ULONG a[4];
UINT  length;
UINT  precedence;
UINT  label;
int   i;

    static const struct
    {
        ULONG       addr[4];
        UINT        length;
        UINT        precedence;
        UINT        label;
        const char *what;
    } rows[] =
    {
        { { 0, 0, 0, 1 },                          128, 50,  0, "::1/128"       },
        { { 0x20010DB8UL, 0, 0, 1 },                 0, 40,  1, "::/0"          },
        { { 0, 0, 0x0000FFFFUL, 0xC0000201UL },     96, 35,  4, "::ffff:0:0/96" },
        { { 0x2002C000UL, 0x02040000UL, 0, 1 },     16, 30,  2, "2002::/16"     },
        { { 0x20010000UL, 0x12340000UL, 0, 1 },     32,  5,  5, "2001::/32"     },
        { { 0xFD000000UL, 0, 0, 1 },                 7,  3, 13, "fc00::/7"      },
        { { 0, 0, 0, 5 },                           96,  1,  3, "::/96"         },
        { { 0xFEC00000UL, 0, 0, 1 },                10,  1, 11, "fec0::/10"     },
        { { 0x3FFE0001UL, 0, 0, 1 },                16,  1, 12, "3ffe::/16"     }
    };

    for (i = 0; i < (int)(sizeof(rows) / sizeof(rows[0])); i++)
    {
        h_set(a, rows[i].addr[0], rows[i].addr[1], rows[i].addr[2], rows[i].addr[3]);

        precedence = 0xFFFF;
        label      = 0xFFFF;
        length     = anx6_policy_lookup(a, &precedence, &label);

        h_check(length == rows[i].length, rows[i].what);
        h_check(precedence == rows[i].precedence, rows[i].what);
        h_check(label == rows[i].label, rows[i].what);
    }
}


/* -------------------------------------------------------------- RFC 4007 -- */

static void test_scope(void)
{
ULONG a[4];

    h_set(a, 0xFE800000UL, 0, 0, 1);
    h_check(anx6_scope(a) == 0x2, "fe80::1 is link scope");

    h_set(a, 0xFEC00000UL, 0, 0, 1);
    h_check(anx6_scope(a) == 0x5, "fec0::1 is site scope");

    h_set(a, 0x20010DB8UL, 0, 0, 1);
    h_check(anx6_scope(a) == 0xE, "2001:db8::1 is global scope");

    h_set(a, 0, 0, 0, 1);
    h_check(anx6_scope(a) == 0x2, "::1 is link scope, RFC 4007 4");

    h_set(a, 0xFF020000UL, 0, 0, 1);
    h_check(anx6_scope(a) == 0x2, "ff02::1 carries its scope");

    h_set(a, 0xFF050000UL, 0, 0, 1);
    h_check(anx6_scope(a) == 0x5, "ff05::1 carries its scope");

    h_set(a, 0xFF0E0000UL, 0, 0, 1);
    h_check(anx6_scope(a) == 0xE, "ff0e::1 carries its scope");

    /* RFC 6724 3.1: an IPv4-mapped address takes the scope of the address
       inside it. */
    h_set(a, 0, 0, 0x0000FFFFUL, 0x7F000001UL);
    h_check(anx6_scope(a) == 0x2, "::ffff:127.0.0.1 is link scope");

    h_set(a, 0, 0, 0x0000FFFFUL, 0xA9FE0101UL);
    h_check(anx6_scope(a) == 0x2, "::ffff:169.254.1.1 is link scope");

    h_set(a, 0, 0, 0x0000FFFFUL, 0xC0000201UL);
    h_check(anx6_scope(a) == 0xE, "::ffff:192.0.2.1 is global scope");
}


/* ------------------------------------------------------------ §5, Rule 1 -- */

static void test_rule1_same_address(void)
{
ULONG dest[4];

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 2, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_addr(H_SLOT1, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);

    h_set(dest, 0x20010DB8UL, 0x00000001UL, 0, 1);

    h_check(h_select(dest, NX_NULL) == H_SLOT1,
            "Rule 1: a destination this node holds is its own source");
}


/* ------------------------------------------------------------ §5, Rule 2 -- */

static void test_rule2_scope(void)
{
ULONG dest[4];

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0xFE800000UL, 0, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_addr(H_SLOT1, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);

    h_set(dest, 0xFF050000UL, 0, 0, 1);

    h_check(h_select(dest, NX_NULL) == H_SLOT1,
            "Rule 2: a site-scoped destination does not take a link-local source");

    h_set(dest, 0xFE800000UL, 0, 0, 0x99);

    h_check(h_select(dest, NX_NULL) == H_SLOT0,
            "Rule 2: a link-local destination takes the link-local source");
}


/* ------------------------------------------------------------ §5, Rule 3 -- */

static void test_rule3_deprecated(void)
{
ULONG dest[4];

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_DEPRECATED);
    h_addr(H_SLOT1, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 2, 64,
           NX_IPV6_ADDR_STATE_VALID);

    h_set(dest, 0x20010DB8UL, 0x00000001UL, 0, 0x99);

    h_check(h_select(dest, NX_NULL) == H_SLOT1,
            "Rule 3: a deprecated address loses to one that is not");

    /* Rule 3 orders candidates, it does not remove them. */
    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_DEPRECATED);

    h_check(h_select(dest, NX_NULL) == H_SLOT0,
            "Rule 3: a deprecated address is still a candidate of last resort");
}


/* ------------------------------------------------------------ §5, Rule 5 -- */

static void test_rule5_outgoing_interface(void)
{
ULONG dest[4];

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_addr(H_SLOT1, H_ETH1, 0x20010DB8UL, 0x00000002UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_router(H_ETH1);

    h_set(dest, 0x20010DB8UL, 0x00010000UL, 0, 0x99);

    h_check(anx6_common_prefix_len(h_ip.nx_ipv6_address[H_SLOT0].nxd_ipv6_address,
                                   dest) ==
            anx6_common_prefix_len(h_ip.nx_ipv6_address[H_SLOT1].nxd_ipv6_address,
                                   dest),
            "the fixture leaves Rule 8 nothing to say");

    h_check(h_select(dest, NX_NULL) == H_SLOT1,
            "Rule 5: the source comes off the interface the packet leaves by");
}


/* ------------------------------------------------------------ §5, Rule 6 -- */

static void test_rule6_label(void)
{
ULONG dest[4];

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0x20010000UL, 0xFFFF0000UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_addr(H_SLOT1, H_ETH0, 0x20010002UL, 0, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_router(H_ETH0);

    h_set(dest, 0x20010001UL, 0, 0, 1);

    h_check(anx6_common_prefix_len(h_ip.nx_ipv6_address[H_SLOT0].nxd_ipv6_address,
                                   dest) >
            anx6_common_prefix_len(h_ip.nx_ipv6_address[H_SLOT1].nxd_ipv6_address,
                                   dest),
            "the fixture puts Rule 8 on the other side");

    h_check(h_select(dest, NX_NULL) == H_SLOT1,
            "Rule 6: a Teredo source is not used for a native destination");

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0x20020102UL, 0, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_addr(H_SLOT1, H_ETH0, 0x20040000UL, 0, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_router(H_ETH0);

    h_set(dest, 0x20030102UL, 0, 0, 1);

    h_check(h_select(dest, NX_NULL) == H_SLOT1,
            "Rule 6: a 6to4 source is not used for a native destination");
}


/* ------------------------------------------------------------ §5, Rule 8 -- */

static void test_rule8_longest_prefix(void)
{
ULONG dest[4];

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_addr(H_SLOT1, H_ETH0, 0x20010DB8UL, 0x00010001UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_router(H_ETH0);

    h_set(dest, 0x20010DB8UL, 0x00010000UL, 0, 0x99);

    h_check(h_select(dest, NX_NULL) == H_SLOT1,
            "Rule 8: the longest matching prefix wins when nothing above it speaks");
}


/* ---------------------------------------------------------------- §4 ------ */

static void test_candidate_set(void)
{
ULONG dest[4];

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_TENTATIVE);
    h_router(H_ETH0);

    h_set(dest, 0x20010DB8UL, 0x00010000UL, 0, 0x99);

    h_check(h_select(dest, NX_NULL) == -1,
            "a tentative address is not a candidate");

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_addr(H_SLOT1, H_ETH1, 0x20010DB8UL, 0x00010000UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_if(H_ETH1) -> nx_interface_link_up = NX_FALSE;
    h_router(H_ETH0);

    h_check(h_select(dest, NX_NULL) == H_SLOT0,
            "an address on a down interface is not a candidate");

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);

    h_check(h_select(dest, NX_NULL) == -1,
            "a destination with no route is refused");

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0xFE800000UL, 0, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_addr(H_SLOT1, H_ETH1, 0xFE800000UL, 0, 0, 2, 64,
           NX_IPV6_ADDR_STATE_VALID);

    h_set(dest, 0xFE800000UL, 0, 0, 0x99);

    h_check(h_select(dest, NX_NULL) == H_SLOT0,
            "a link-local destination takes an address from one link only");

    h_reset();
    h_addr(H_SLOT1, H_ETH1, 0xFE800000UL, 0, 0, 2, 64,
           NX_IPV6_ADDR_STATE_VALID);

    h_check(h_select(dest, NX_NULL) == H_SLOT1,
            "an interface that is up with no address is not the outgoing one");

#ifndef NX_DISABLE_LOOPBACK_INTERFACE
    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_router(H_ETH0);

    h_set(dest, 0, 0, 0, 1);

    h_check(h_select(dest, NX_NULL) == NX_LOOPBACK_IPV6_SOURCE_INDEX,
            "::1 is the source for ::1");

    h_set(dest, 0x20010DB8UL, 0x00010000UL, 0, 0x99);

    h_check(h_select(dest, NX_NULL) == H_SLOT0,
            "::1 is not the source for anything else");
#endif /* NX_DISABLE_LOOPBACK_INTERFACE */

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0x20010DB8UL, 0x00000001UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_addr(H_SLOT1, H_ETH1, 0x20010DB8UL, 0x00000002UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_router(H_ETH0);

    h_set(dest, 0x20010DB8UL, 0x00010000UL, 0, 0x99);

    h_check(h_select(dest, NX_NULL) == H_SLOT0,
            "unconstrained, the route decides");
    h_check(h_select(dest, h_if(H_ETH1)) == H_SLOT1,
            "a named interface is the only one searched");

    /* nx_icmpv6_send_rs() depends on the refusal: it sends the solicitation
       from the unspecified address when this fails. */
    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0xFE800000UL, 0, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);

    h_set(dest, 0xFF020000UL, 0, 0, 2);

    h_check(h_select(dest, h_if(H_ETH1)) == -1,
            "an interface with no usable address is refused, not substituted");
    h_check(h_select(dest, h_if(H_ETH0)) == H_SLOT0,
            "and the interface that has one answers with it");
}


#ifdef NX_ENABLE_IPV6_MULTICAST
static void test_multicast_join(void)
{
ULONG dest[4];

    h_reset();
    h_addr(H_SLOT0, H_ETH0, 0xFE800000UL, 0, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_addr(H_SLOT1, H_ETH1, 0xFE800000UL, 0, 0, 2, 64,
           NX_IPV6_ADDR_STATE_VALID);

    h_set(h_ip.nx_ipv6_multicast_entry[0].nx_ip_mld_join_list,
          0xFF020000UL, 0, 0, 0xFB);
    h_ip.nx_ipv6_multicast_entry[0].nx_ip_mld_join_interface_list = h_if(H_ETH1);

    h_set(dest, 0xFF020000UL, 0, 0, 0xFB);

    h_check(h_select(dest, NX_NULL) == H_SLOT1,
            "a joined group leaves by the interface it was joined on");

    h_reset();
    h_addr(H_SLOT0, H_ETH1, 0xFE800000UL, 0, 0, 2, 64,
           NX_IPV6_ADDR_STATE_VALID);
    h_addr(H_SLOT1, H_ETH1, 0x20010DB8UL, 0x00000002UL, 0, 1, 64,
           NX_IPV6_ADDR_STATE_VALID);

    h_set(dest, 0xFF050000UL, 0, 0, 0xFB);

    h_check(h_select(dest, h_if(H_ETH1)) == H_SLOT1,
            "a zoned site-scope group takes a wide-enough source");
}
#endif /* NX_ENABLE_IPV6_MULTICAST */


int main(void)
{
    test_policy_table();
    test_scope();
    test_rule1_same_address();
    test_rule2_scope();
    test_rule3_deprecated();
    test_rule5_outgoing_interface();
    test_rule6_label();
    test_rule8_longest_prefix();
    test_candidate_set();
#ifdef NX_ENABLE_IPV6_MULTICAST
    test_multicast_join();
#endif

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return h_failures ? 1 : 0;
}
