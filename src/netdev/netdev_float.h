/*
 * anxnet.device: the DP8390 command-register probe, guarded against a bus
 * that echoes the probe's own byte.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_FLOAT_H
#define AMINETXDUO_NETDEV_FLOAT_H

#include <exec/types.h>

#include <aminetxduo/anxdiag.h>

/* Redirected by src/netdev/test/test_netdev_float.c: a keeper is one latch for
   a whole window and cannot be a byte array. */
#ifndef NETDEV_CR_PUT
#define NETDEV_CR_PUT(p, v)     (*(p) = (UBYTE)(v))
#endif
#ifndef NETDEV_CR_GET
#define NETDEV_CR_GET(p)        (*(p))
#endif

#define NETDEV_CR_STOPPED       0x21u   /* ED_CR_STP | ED_CR_RD2 */

/* RBCR0, NIC register 10: write-only, even, and inert while STP is set, so a
   real chip is unchanged by it and Gayle's one-value keeper is not. */
#define NETDEV_CR_DECOY_REG     0x0au

/*
 * Is a DP8390 decoding at nic?  Writing CR and reading it back at the same
 * address cannot answer that: an empty Gayle socket keeps the last value
 * driven on the bus, so the read returns the write and every probe matches.
 * The decoy write drives a different byte last, which only a real CR latch
 * survives.  Rests on the keeper holding ONE value for the window, not one
 * per address -- there is no storage in an empty socket to hold more.
 */
static inline BOOL netdev_cr_answers(volatile UBYTE *nic, UWORD stride,
                                     UBYTE *cr_out)
{
    UBYTE v;

    NETDEV_CR_PUT(nic, NETDEV_CR_STOPPED);
    NETDEV_CR_PUT(nic + (ULONG)NETDEV_CR_DECOY_REG * stride, ANXDIAG_CR_DECOY);
    v = NETDEV_CR_GET(nic);

    if (cr_out != NULL)
        *cr_out = v;

    /* Mask START: clones (Netgear FA411) come out of reset with CR bit 1 stuck
       and read 0x23.  0xff, 0x00 and the decoy byte all still fail. */
    return (BOOL)((v & (UBYTE)~0x02u) == (UBYTE)NETDEV_CR_STOPPED);
}

#endif /* AMINETXDUO_NETDEV_FLOAT_H */
