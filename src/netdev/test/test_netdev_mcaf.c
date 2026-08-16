/*
 * The multicast hash, on the host.
 *
 * This is the arithmetic that decides whether an IPv6 solicited-node frame is
 * ever delivered to the card.  Individual Computers' x-surf.device and
 * x-surf-100.device never reach it -- they reject the join first, on bit 7 of
 * the address where the group bit is bit 0 -- so the hash itself is not what
 * was broken there.  It is what must be right for the fix to mean anything,
 * and it is the one part of this driver that can be checked without hardware.
 *
 * The expected values are not this code's own output.  They were computed
 * separately from the definition -- CRC-32 with polynomial 0x04C11DB7,
 * initial value 0xFFFFFFFF, most significant bit first, no reflection and no
 * final inversion, top six bits of the result indexing a 64-bit filter -- and
 * pasted in.  A test that asks the implementation what it thinks proves only
 * that it is consistent.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "netdev_mcaf.h"

static int failures;

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    if (got == want)
    {
        printf("ok   %s = 0x%08lx\n", what, got);
        return;
    }

    printf("FAIL %s: got 0x%08lx, want 0x%08lx\n", what, got, want);
    failures++;
}

static void expect_int(const char *what, int got, int want)
{
    if (got == want)
    {
        printf("ok   %s = %d\n", what, got);
        return;
    }

    printf("FAIL %s: got %d, want %d\n", what, got, want);
    failures++;
}

struct vector
{
    const char *name;
    UBYTE       addr[6];
    ULONG       crc;
    int         index;      /* crc >> 26 */
    int         byte;       /* index >> 3 */
    UBYTE       bit;        /* 1 << (index & 7) */
};

/*
 * Six addresses that matter, and one that does not:
 *   01:00:5e:00:00:01   IPv4 all-hosts
 *   01:00:5e:7f:ff:fa   SSDP
 *   33:33:00:00:00:01   IPv6 all-nodes
 *   33:33:00:00:00:02   IPv6 all-routers
 *   33:33:ff:12:34:56   an IPv6 solicited-node address, the one whose loss
 *                       costs off-LAN IPv6 on every X-Surf
 *   01:80:c2:00:00:0e   LLDP
 */
static const struct vector vectors[] =
{
    { "01:00:5e:00:00:01", { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 },
      0x7fa32d9bUL, 31, 3, 0x80 },
    { "01:00:5e:7f:ff:fa", { 0x01, 0x00, 0x5e, 0x7f, 0xff, 0xfa },
      0xae3c4afcUL, 43, 5, 0x08 },
    { "33:33:00:00:00:01", { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 },
      0xf99baabaUL, 62, 7, 0x40 },
    { "33:33:00:00:00:02", { 0x33, 0x33, 0x00, 0x00, 0x00, 0x02 },
      0xa4113a23UL, 41, 5, 0x02 },
    { "33:33:ff:12:34:56", { 0x33, 0x33, 0xff, 0x12, 0x34, 0x56 },
      0x0bf9eb4bUL,  2, 0, 0x04 },
    { "01:80:c2:00:00:0e", { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e },
      0x876cdef0UL, 33, 4, 0x02 },
};

#define NVECTORS ((int)(sizeof(vectors) / sizeof(vectors[0])))

int main(void)
{
    UBYTE mar[8];
    int   i;
    int   j;

    for (i = 0; i < NVECTORS; i++)
    {
        char  label[64];
        ULONG crc = netdev_ether_crc32_be(vectors[i].addr, NETDEV_ADDR_LEN);

        snprintf(label, sizeof(label), "crc %s", vectors[i].name);
        expect_u32(label, crc, vectors[i].crc);

        snprintf(label, sizeof(label), "index %s", vectors[i].name);
        expect_int(label, (int)(crc >> 26), vectors[i].index);
    }

    /* One address at a time: exactly one bit, in exactly one byte. */
    for (i = 0; i < NVECTORS; i++)
    {
        char label[64];

        netdev_mar_clear(mar);
        netdev_mar_set(mar, vectors[i].addr);

        for (j = 0; j < 8; j++)
        {
            UBYTE want = (UBYTE)(j == vectors[i].byte ? vectors[i].bit : 0);

            snprintf(label, sizeof(label), "mar[%d] after %s",
                     j, vectors[i].name);
            expect_int(label, mar[j], want);
        }
    }

    /* All of them together: the union, and nothing else. */
    {
        UBYTE want[8];

        memset(want, 0, sizeof(want));
        for (i = 0; i < NVECTORS; i++)
            want[vectors[i].byte] |= vectors[i].bit;

        netdev_mar_clear(mar);
        for (i = 0; i < NVECTORS; i++)
            netdev_mar_set(mar, vectors[i].addr);

        for (j = 0; j < 8; j++)
        {
            char label[64];

            snprintf(label, sizeof(label), "mar[%d] union", j);
            expect_int(label, mar[j], want[j]);
        }
    }

    /* Adding an address twice must not clear it: the bit is idempotent. */
    netdev_mar_clear(mar);
    netdev_mar_set(mar, vectors[4].addr);
    netdev_mar_set(mar, vectors[4].addr);
    expect_int("mar solicited-node set twice", mar[vectors[4].byte],
               vectors[4].bit);

    /* Promiscuous is every bit, and clear is none. */
    netdev_mar_all(mar);
    for (j = 0; j < 8; j++)
    {
        char label[64];

        snprintf(label, sizeof(label), "mar[%d] all", j);
        expect_int(label, mar[j], 0xff);
    }

    netdev_mar_clear(mar);
    for (j = 0; j < 8; j++)
    {
        char label[64];

        snprintf(label, sizeof(label), "mar[%d] clear", j);
        expect_int(label, mar[j], 0);
    }

    /*
     * The group bit test the whole driver turns on.  Bit 0 of the first octet
     * is set on every one of these.  Bit 7, which both IC drivers test, is
     * clear on all but none.  That asymmetry is the defect, stated as an
     * assertion rather than as a comment.
     */
    for (i = 0; i < NVECTORS; i++)
    {
        char label[64];

        snprintf(label, sizeof(label), "group bit %s", vectors[i].name);
        expect_int(label, vectors[i].addr[0] & 0x01, 1);

        snprintf(label, sizeof(label), "bit 7 %s", vectors[i].name);
        expect_int(label, (vectors[i].addr[0] & 0x80) != 0, 0);
    }

    if (failures != 0)
    {
        printf("netdev_mcaf: %d failures\n", failures);
        return 1;
    }

    printf("netdev_mcaf: all ok\n");

    return 0;
}
