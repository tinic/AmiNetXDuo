/*
 * The derived station address, on the host.
 *
 * This path cannot be reached under an emulator: Amiberry's NE2000 presents a
 * valid PROM address, so the card never asks for one.  What is asserted here
 * is what an emulator could not show: that the address is the same on every
 * boot of one machine, that it is a locally-administered unicast address, and
 * that two machines that differ in anything get two different addresses.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "netdev_macgen.h"

static int failures;

static void ok(const char *what, int cond)
{
    if (cond)
    {
        printf("ok   %s\n", what);
        return;
    }

    printf("FAIL %s\n", what);
    failures++;
}

static void show(const char *tag, const UBYTE *mac)
{
    printf("     %s %02x:%02x:%02x:%02x:%02x:%02x\n", tag,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* --------------------------------------------------------------- usable -- */

static void test_usable(void)
{
    static const UBYTE zero[6]  = { 0, 0, 0, 0, 0, 0 };
    static const UBYTE ones[6]  = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const UBYTE group[6] = { 0x01, 0xd4, 0xff, 0x03, 0x00, 0x20 };
    static const UBYTE good[6]  = { 0x00, 0xd4, 0xff, 0x03, 0x00, 0x20 };
    static const UBYTE thin[6]  = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };

    ok("all-zero is not usable",  !netdev_mac_usable(zero));
    ok("all-ones is not usable",  !netdev_mac_usable(ones));
    /* The D-Link DFE-670TXD's PROM, verbatim: the group bit is the reason
       ne2000_attach() clears it before this is asked. */
    ok("group bit is not usable", !netdev_mac_usable(group));
    ok("the same address with the group bit clear is usable",
       netdev_mac_usable(good));
    ok("one bit set anywhere is usable", netdev_mac_usable(thin));
}

/* --------------------------------------------------------------- derive -- */

/* Two fingerprints that differ in one byte, which is the case that matters:
   two machines identical but for one field must not share an address. */
static void test_derive(void)
{
    UBYTE fp_a[16];
    UBYTE fp_b[16];
    UBYTE a[6], a2[6], b[6];
    UWORD i;

    for (i = 0; i < sizeof(fp_a); i++)
    {
        fp_a[i] = (UBYTE)(0x40u + i);
        fp_b[i] = (UBYTE)(0x40u + i);
    }
    fp_b[9] ^= 0x01u;

    netdev_mac_derive(fp_a, (UWORD)sizeof(fp_a), a);
    netdev_mac_derive(fp_a, (UWORD)sizeof(fp_a), a2);
    netdev_mac_derive(fp_b, (UWORD)sizeof(fp_b), b);

    show("machine A:", a);
    show("machine B:", b);

    ok("the same fingerprint derives the same address, every time",
       memcmp(a, a2, 6) == 0);
    ok("one bit of difference derives a different address",
       memcmp(a, b, 6) != 0);

    ok("octet 0 has the locally-administered bit set", (a[0] & 0x02u) != 0);
    ok("octet 0 has the group bit clear",              (a[0] & 0x01u) == 0);
    ok("the derived address is usable",                netdev_mac_usable(a));
    ok("B is usable too",                              netdev_mac_usable(b));
    ok("octet 1 is the marker",       a[1] == (UBYTE)NETDEV_MAC_TAG);

    /*
     * Pinned, not computed from the code under test: a change to the mixing
     * changes every address in the field, which for anyone whose DHCP server
     * has a reservation is a machine that loses its address on an upgrade.
     * If this value has to move, that is the cost being accepted.
     */
    {
        static const UBYTE want[6] = { 0x02, 0xad, 0xd3, 0x46, 0xa9, 0x1b };

        show("pinned:   ", want);
        ok("the derivation has not moved", memcmp(a, want, 6) == 0);
    }
}

/*
 * A short fingerprint is not a special case in the arithmetic, but it is the
 * one a machine with no boards and no CIS produces, so it is asserted rather
 * than assumed.
 */
static void test_short(void)
{
    static const UBYTE one[1] = { 0x00 };
    static const UBYTE two[1] = { 0x01 };
    UBYTE a[6], b[6];

    netdev_mac_derive(one, 1, a);
    netdev_mac_derive(two, 1, b);

    ok("a one-byte fingerprint still derives a usable address",
       netdev_mac_usable(a) && netdev_mac_usable(b));
    ok("and two of them differ", memcmp(a, b, 6) != 0);
}

int main(void)
{
    test_usable();
    test_derive();
    test_short();

    printf("%s\n", failures == 0 ? "PASS" : "FAIL");

    return failures == 0 ? 0 : 1;
}
