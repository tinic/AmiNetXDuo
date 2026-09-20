/*
 * AmiNetXDuo, private SANA-II buffer-management extensions.
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_ANXS2EXT_H
#define AMINETXDUO_ANXS2EXT_H

#include <exec/types.h>

#define ANXD_S2_RXF_SUMMED      0x01
#define ANXD_S2_RXF_VERIFIED    0x02
#define ANXD_S2_RXF_CONTINUES   0x04    /* legacy; GRO is stack-side */

#define ANXD_S2_TXF_TCP         0x01
#define ANXD_S2_TXF_UDP         0x02

#define ANXD_S2IOF_L4_CSUM      0x10

typedef UBYTE *(*AnxdS2RxDirect)(APTR ios2_data, ULONG len);
typedef VOID   (*AnxdS2RxFilled)(APTR ios2_data, ULONG len, ULONG sum,
                                 UBYTE flags);

/*
 * ONE VERSIONED NEGOTIATION TAG.
 *
 * This is a private extension between an opener and a driver, not a SANA-II
 * allocation.  Its tag therefore has an AmiNetXDuo-owned TAG_USER value and
 * does not borrow S2_Dummy or any of Commodore's buffer-hook offsets.
 *
 * The opener zeroes the record, writes VERSION, sizeof(record), Request and
 * the two receive callbacks, then supplies a pointer to it as ti_Data.  A
 * driver accepts only a version and size it understands and writes Accepted
 * as the intersection it can honour for the selected unit.  An ordinary
 * driver ignores the tag and leaves Accepted zero.  Future versions append
 * fields; neither side may read past Size.
 */
#define ANXD_S2_EXTENSION       (0x80000000UL | 0x00414e58UL) /* TAG_USER|'ANX' */
#define ANXD_S2_ABI_VERSION     1u

#define ANXD_S2F_RX_DIRECT      (1UL << 0)
#define ANXD_S2F_RX_LINK_HDR    (1UL << 1)
#define ANXD_S2F_RX_VERIFIED    (1UL << 2)
#define ANXD_S2F_TX_CSUM_TCP    (1UL << 3)
#define ANXD_S2F_TX_CSUM_UDP    (1UL << 4)
#define ANXD_S2F_RX_POLL        (1UL << 5)
#define ANXD_S2F_RX_CAPACITY    (1UL << 6)
#define ANXD_S2F_TX_QUICK       (1UL << 7)

typedef struct AnxdS2Extension
{
    UWORD           Version;
    UWORD           Size;
    ULONG           Request;
    ULONG           Accepted;
    AnxdS2RxDirect  RxDirect;
    AnxdS2RxFilled  RxFilled;
} AnxdS2Extension;

/* ANXD_CMD_RX_POLL: "hand over what you are holding for my reads".
 *
 * A driver that empties a deep hardware ring in one interrupt can find the
 * opener's posted reads run out part way through a burst.  Rather than drop
 * the rest, a driver that knows this command leaves those frames where they
 * are and delivers them the next time reads are posted: at its next
 * interrupt, or when the opener sends this command, which an opener does
 * once at the end of each pass over its completed reads, after it has
 * re-posted them and before it sleeps.  Quick (IOF_QUICK honoured, nothing
 * to wait for), no arguments, io_Error 0; a driver that does not know it
 * answers IOERR_NOCMD like any other unknown command, a unit whose card
 * cannot hold a frame for a late read answers S2ERR_NOT_SUPPORTED, and on
 * either the opener stops sending it.  Private commands use the NSD
 * third-party block ($8000-$BFFF); $4000-$7FFF is reserved for the OS team. */
#define ANXD_CMD_RX_POLL        0x8190

/* ANXD_CMD_RX_CAPACITY: "how much can your hardware hold from the wire?"
 *
 * Answered in ios2_DataLength: the bytes of received frames the unit's own
 * receive memory holds, at line rate, while nobody drains it -- the ring
 * or FIFO after which the next frame is lost.  A card that can pause the
 * wire instead may answer 0, which means "no limit worth stating", and so
 * does a driver that cannot say.  Quick, no arguments, io_Error 0;
 * IOERR_NOCMD from a driver that does not know it.
 *
 * What an opener does with it: keep the TCP window it advertises on that
 * interface inside the number, so a peer on the same LAN cannot put more
 * on the wire at once than the card can take.  Measured 2026-09-16 on an
 * A3000 (25 MHz 68030) with an X-Surf 100: a 100,352-byte window against
 * a 13 KB ring was 42 overruns and 42 chip resets in ten seconds and
 * 2.8 Mbit/s. */
#define ANXD_CMD_RX_CAPACITY    0x8192

#endif /* AMINETXDUO_ANXS2EXT_H */
