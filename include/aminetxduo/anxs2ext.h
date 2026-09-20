/*
 * AmiNetXDuo, private SANA-II buffer-management extensions.
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_ANXS2EXT_H
#define AMINETXDUO_ANXS2EXT_H

#include <exec/types.h>

#define ANXD_S2_RXF_SUMMED      0x01
#define ANXD_S2_RXF_VERIFIED    0x02

#define ANXD_S2_TXF_TCP         0x01
#define ANXD_S2_TXF_UDP         0x02
/* This write is one of a run the opener is sending back to back: the driver
   may hold the hardware's start until the run ends (ANXD_CMD_TX_FLUSH), a
   few more frames arrive, or its own backstop, so the frames leave the
   wire together.  Negotiated as ANXD_S2F_TX_MORE. */
#define ANXD_S2_TXF_MORE        0x40

typedef UBYTE *(*AnxdS2RxDirect)(APTR ios2_data, ULONG len);
typedef VOID   (*AnxdS2RxFilled)(APTR ios2_data, ULONG len, ULONG sum,
                                 UBYTE flags);
typedef UBYTE  (*AnxdS2TxFlags)(APTR ios2_data);

/*
 * ONE VERSIONED NEGOTIATION TAG.
 *
 * This is a private extension between an opener and a driver, not a SANA-II
 * allocation.  Its tag therefore has an AmiNetXDuo-owned TAG_USER value and
 * does not borrow S2_Dummy or any of Commodore's buffer-hook offsets.
 *
 * The opener zeroes the record, writes VERSION, sizeof(record), Request and
 * the two receive callbacks, then supplies a pointer to it as ti_Data.  A
 * driver accepts only a version and prefix size it understands and writes
 * Accepted as the intersection it can honour for the selected unit.  An
 * ordinary driver ignores the tag and leaves Accepted zero.  Future versions
 * append fields; neither side may read past Size.  Appending fields without
 * changing the meaning of this prefix keeps VERSION unchanged.  VERSION
 * changes only for an incompatible interpretation, which an older peer must
 * ignore.
 *
 * The record, callback code and every object the callbacks inspect remain
 * valid until CloseDevice().  Receive callbacks can run from the driver's
 * interrupt/server/vertical-blank service context and must not block.  The
 * transmit callback can also run while a queued write is advanced from such
 * a context.  All fields are native big-endian m68k ABI values; this is an
 * in-process driver interface, not a wire format.
 */
#define ANXD_S2_EXTENSION       (0x80000000UL | 0x00414e58UL) /* TAG_USER|'ANX' */
#define ANXD_S2_ABI_VERSION     2u

#define ANXD_S2F_RX_DIRECT      (1UL << 0)
#define ANXD_S2F_RX_LINK_HDR    (1UL << 1)
#define ANXD_S2F_RX_VERIFIED    (1UL << 2)
#define ANXD_S2F_TX_CSUM_TCP    (1UL << 3)
#define ANXD_S2F_TX_CSUM_UDP    (1UL << 4)
#define ANXD_S2F_RX_POLL        (1UL << 5)
#define ANXD_S2F_RX_CAPACITY    (1UL << 6)
#define ANXD_S2F_TX_QUICK       (1UL << 7)
#define ANXD_S2F_RX_BATCH       (1UL << 8)
#define ANXD_S2F_TX_MORE        (1UL << 9)
#define ANXD_S2F_ALL            (ANXD_S2F_RX_DIRECT | \
                                 ANXD_S2F_RX_LINK_HDR | \
                                 ANXD_S2F_RX_VERIFIED | \
                                 ANXD_S2F_TX_CSUM_TCP | \
                                 ANXD_S2F_TX_CSUM_UDP | \
                                 ANXD_S2F_RX_POLL | \
                                 ANXD_S2F_RX_CAPACITY | \
                                 ANXD_S2F_TX_QUICK | \
                                 ANXD_S2F_RX_BATCH | \
                                 ANXD_S2F_TX_MORE)

typedef struct AnxdS2Extension
{
    UWORD           Version;
    UWORD           Size;
    ULONG           Request;
    ULONG           Accepted;
    AnxdS2RxDirect  RxDirect;
    AnxdS2RxFilled  RxFilled;
    /* Called for a negotiated CMD_WRITE, possibly from interrupt context.
       It may only inspect ios2_Data and returns ANXD_S2_TXF_* for that one
       request.  Per-write metadata therefore never occupies io_Flags, whose
       unassigned bits belong to the SANA-II/Exec request ABI. */
    AnxdS2TxFlags   TxFlags;
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

/* ANXD_CMD_RX_BATCH: many frames for one IORequest.
 *
 * What it is for.  A CMD_READ carries one frame, so at a gigabit every
 * frame is a ReplyMsg() and a Signal() from the driver, then a GetMsg() and
 * a BeginIO() from the reader: four Exec calls of list work per 1.5 KB, on
 * the receive path, with the driver's own pass still masked around the
 * first two.  This command makes the unit of I/O the driver's pass instead
 * of the frame, with nothing but the ordinary Exec request contract: one
 * queued IORequest, one ReplyMsg() when it is answered.
 *
 * The request.  io_Command ANXD_CMD_RX_BATCH, ios2_PacketType the type it
 * accepts, ios2_Data a pointer to an AnxdS2RxBatch the opener owns, with
 * Count > 0 cookies and Filled 0.  Always queued (IOF_QUICK is cleared as
 * for CMD_READ), so it is answered on mn_ReplyPort like any read.  Each
 * cookie is what the opener's RxDirect/RxFilled pair receive as ios2_data
 * for that slot, exactly as they receive a CMD_READ's ios2_Data today; the
 * opener therefore needs ANXD_S2F_RX_DIRECT and ANXD_S2F_RX_LINK_HDR
 * accepted, because a batch has no per-frame ios2_SrcAddr/DstAddr/
 * PacketType: the 14-byte link header written in front of each payload is
 * the frame's whole identity.  The driver fills Cookie[0], Cookie[1], ...
 * in arrival order, one RxDirect() then one RxFilled() per frame, and
 * writes Filled.  A frame the direct path cannot take (a second opener's
 * read of the same type, a core without a direct claim) is copied into
 * the slot through the opener's S2_CopyToBuff instead, then reported by
 * RxFilled() without ANXD_S2_RXF_SUMMED.  An opener with a filter hook, or
 * a raw one, is refused (S2ERR_NOT_SUPPORTED): neither has a request to
 * judge or fill.
 *
 * When it is answered.  The driver replies the request with io_Error 0 the
 * moment Filled reaches Count, and otherwise at the end of the service pass
 * (interrupt, poll or blank) that filled the first slot: no frame waits in
 * a batch for a later pass, and a pass that took a burst of N frames costs
 * one reply.  A batch with Filled 0 is not answered until a frame comes.
 * AbortIO(), CMD_FLUSH and CloseDevice() answer it IOERR_ABORTED and the
 * unit going offline S2ERR_OUTOFSERVICE, each with Filled as it stood:
 * those frames are complete and delivered.
 *
 * Posting.  Two batches per type keep the driver fed while the reader is
 * working through one; a batch counts as one posted read of its type for
 * "who takes this type", so a second opener's CMD_READ of the same type
 * still turns the direct path off for both, as it does today.  A driver
 * that does not know the command answers IOERR_NOCMD; an opener that did
 * not get ANXD_S2F_RX_BATCH accepted posts CMD_READs as before. */
#define ANXD_CMD_RX_BATCH       0x8193

/* The largest record a conforming driver has to accept.  A bounded public
 * limit keeps a corrupt or third-party opener from making the driver walk an
 * unbounded cookie array at interrupt level. */
#define ANXD_S2_RX_BATCH_MAX    32u

/* ANXD_CMD_TX_FLUSH: "start whatever you are holding for me".
 *
 * Why it exists.  A sender that produces one segment every 30 us onto a
 * wire that carries one in 12 hands the far end one lone segment at a
 * time, and Linux acknowledges a lone segment at once: measured 285,713
 * acknowledgements for 287,834 segments, each one a frame through this
 * machine's receive path.  Two segments arriving together draw one.  So a
 * write flagged ANXD_S2_TXF_MORE lets the driver defer the hardware start,
 * and the opener sends this command when its run is over -- at the end of
 * the send() call, before it could wait for anything.  The driver also
 * starts on its own once a few writes are pending and, as a backstop, on
 * its next tick.  Quick, no arguments, io_Error 0; IOERR_NOCMD from a
 * driver that does not know it, S2ERR_NOT_SUPPORTED from a unit that
 * cannot hold a start. */
#define ANXD_CMD_TX_FLUSH       0x8194

typedef struct AnxdS2RxBatch
{
    UWORD   Count;          /* cookies the opener supplies              */
    UWORD   Filled;         /* written by the driver: frames delivered  */
    APTR    Cookie[];       /* Count of them; RxDirect/RxFilled cookies */
} AnxdS2RxBatch;

/* The size of a batch record holding n cookies. */
#define ANXD_S2_RX_BATCH_SIZE(n) \
    (sizeof(AnxdS2RxBatch) + (n) * sizeof(APTR))

#endif /* AMINETXDUO_ANXS2EXT_H */
