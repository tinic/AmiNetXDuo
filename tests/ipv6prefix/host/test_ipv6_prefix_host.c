/*
 * Audit N-021 and N-022: the IPv6 prefix list and the prefix compare.
 *
 * canonical: the list stores a prefix with the bits after its length
 *            cleared, so adding or deleting the same prefix with any of
 *            those bits set must find that entry: a repeat add is
 *            NX_DUPLICATED_ENTRY (lifetime and on-link updates intact),
 *            four such adds do not fill the four-entry table, and the
 *            delete removes it.  A /48 delete must not remove a /64 entry.
 * range:     a prefix length above 128 is refused by the add, and
 *            CHECK_IP_ADDRESSES_BY_PREFIX gives the same answer (no match)
 *            whatever lies in memory after the two addresses.
 *
 * Linked for real: the prefix-list add, delete and delete-entry, the table
 * init and nx_ipv6_util.c.  Stubbed: _nx_ipv6_multicast_leave.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ipv6.h"

#include <stdio.h>
#include <string.h>

VOID _nxd_ipv6_default_router_table_init(NX_IP *ip_ptr);

UINT _nx_ipv6_multicast_leave(NX_IP *ip_ptr, ULONG *address, NX_INTERFACE *nx_interface)
{
    (void) ip_ptr; (void) address; (void) nx_interface;
    return NX_SUCCESS;
}

static NX_IP ip;
static int   bad;

static void check(const char *what, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    bad |= !cond;
}

static UINT entries(void)
{
    NX_IPV6_PREFIX_ENTRY *e;
    UINT                  n = 0;

    for (e = ip.nx_ipv6_prefix_list_ptr; e; e = e -> nx_ipv6_prefix_entry_next)
        n++;
    return n;
}

static NX_IPV6_PREFIX_ENTRY *find(ULONG w0, ULONG w1, ULONG len)
{
    NX_IPV6_PREFIX_ENTRY *e;

    for (e = ip.nx_ipv6_prefix_list_ptr; e; e = e -> nx_ipv6_prefix_entry_next)
        if (e -> nx_ipv6_prefix_entry_network_address[0] == w0
            && e -> nx_ipv6_prefix_entry_network_address[1] == w1
            && e -> nx_ipv6_prefix_entry_prefix_length == len)
            return e;
    return NX_NULL;
}

static void arm_canonical(void)
{
    /* 2001:db8:1::/64, each with different bits set below the length.  */
    ULONG junk[4][4] = {
        { 0x20010db8UL, 0x00010000UL, 0xdeadbeefUL, 0x00000001UL },
        { 0x20010db8UL, 0x00010000UL, 0x00000000UL, 0x00000002UL },
        { 0x20010db8UL, 0x00010000UL, 0x12345678UL, 0x00000000UL },
        { 0x20010db8UL, 0x00010000UL, 0xffffffffUL, 0xffffffffUL },
    };
    ULONG clean[4] = { 0x20010db8UL, 0x00010000UL, 0, 0 };
    ULONG other[4] = { 0x20010db8UL, 0x00020000UL, 0, 0 };
    ULONG p48[4]   = { 0x20010db8UL, 0x00010000UL, 0, 0 };
    NX_IPV6_PREFIX_ENTRY *e;
    UINT  status, i, dups = 0;

    _nxd_ipv6_default_router_table_init(&ip);

    status = _nx_ipv6_prefix_list_add_entry(&ip, junk[0], 64, 1000, 0);
    e = find(0x20010db8UL, 0x00010000UL, 64);
    check("first add succeeds and stores the prefix masked",
          status == NX_SUCCESS && e
          && e -> nx_ipv6_prefix_entry_network_address[2] == 0
          && e -> nx_ipv6_prefix_entry_network_address[3] == 0);

    for (i = 0; i < 4; i++)
        dups += _nx_ipv6_prefix_list_add_entry(&ip, junk[i], 64, 1000, 0) == NX_DUPLICATED_ENTRY;
    dups += _nx_ipv6_prefix_list_add_entry(&ip, clean, 64, 1000, 0) == NX_DUPLICATED_ENTRY;
    printf("     repeat adds answered NX_DUPLICATED_ENTRY: %u of 5, entries %u\n", dups, entries());
    check("repeat adds with ignored bits set are duplicates, one entry", dups == 5 && entries() == 1);

    status = _nx_ipv6_prefix_list_add_entry(&ip, other, 64, 1000, 0);
    check("a different prefix still finds a free slot", status == NX_SUCCESS && entries() == 2);

    /* The duplicate path's lifetime and on-link updates still apply.  */
    (void) _nx_ipv6_prefix_list_add_entry(&ip, junk[1], 64, 5000, 1);
    e = find(0x20010db8UL, 0x00010000UL, 64);
    check("duplicate add raises the lifetime and sets on-link",
          e && e -> nx_ipv6_prefix_entry_valid_lifetime == 5000
          && e -> nx_ipv6_prefix_entry_onlink == 1);

    _nx_ipv6_prefix_list_delete(&ip, p48, 48);
    check("a /48 delete leaves the /64 entry", find(0x20010db8UL, 0x00010000UL, 64) != NX_NULL);

    _nx_ipv6_prefix_list_delete(&ip, junk[2], 64);
    check("delete with ignored bits set removes the entry, and only it",
          find(0x20010db8UL, 0x00010000UL, 64) == NX_NULL && entries() == 1);
}

/* Two addresses, each followed by memory that is not part of it.  */
struct guarded
{
    ULONG addr[4];
    ULONG after[4];
};

static void arm_range(void)
{
    static const ULONG lens[] = { 129, 160, 255 };
    ULONG  p[4] = { 0x20010db8UL, 0x00010000UL, 0, 0 };
    struct guarded a, b;
    UINT   i, refused = 0;
    INT    same_after, diff_after;

    _nxd_ipv6_default_router_table_init(&ip);
    for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
    {
        UINT status = _nx_ipv6_prefix_list_add_entry(&ip, p, lens[i], 1000, 1);

        refused += (status != NX_SUCCESS) && (status != NX_DUPLICATED_ENTRY);
    }
    printf("     lengths 129, 160, 255 refused: %u of 3, entries %u\n", refused, entries());
    check("prefix lengths above 128 are refused by the add", refused == 3 && entries() == 0);

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memcpy(a.addr, p, sizeof(p));
    memcpy(b.addr, p, sizeof(p));
    same_after = CHECK_IP_ADDRESSES_BY_PREFIX(a.addr, b.addr, 160);
    b.after[0] = 0x5a5a5a5aUL;
    diff_after = CHECK_IP_ADDRESSES_BY_PREFIX(a.addr, b.addr, 160);
    printf("     length 160: %d with equal memory after the addresses, %d with different\n",
           (int) same_after, (int) diff_after);
    check("length 160 compares nothing past the addresses (no match either way)",
          same_after == 0 && diff_after == 0);

    check("length 128 still compares the whole address",
          CHECK_IP_ADDRESSES_BY_PREFIX(a.addr, b.addr, 128) == 1);
    b.addr[3] = 1;
    check("length 64 still ignores the low 64 bits",
          CHECK_IP_ADDRESSES_BY_PREFIX(a.addr, b.addr, 64) == 1);
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s canonical|range\n", argv[0]);
        return 2;
    }
    memset(&ip, 0, sizeof(ip));
    if (strcmp(argv[1], "canonical") == 0)
        arm_canonical();
    else if (strcmp(argv[1], "range") == 0)
        arm_range();
    else
        return 2;
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad;
}
