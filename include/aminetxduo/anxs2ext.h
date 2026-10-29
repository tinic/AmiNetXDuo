/*
 * AmiNetXDuo, private SANA-II buffer-management extensions.
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_ANXS2EXT_H
#define AMINETXDUO_ANXS2EXT_H

#include <exec/types.h>
#include <stddef.h>      /* offsetof */

/* RxFilled flags.
 *
 * ANXD_S2_RXF_SUMMED (ANX-011): `sum` is an uncomplemented ones-complement
 * sum of the payload -- the `len` bytes written at the RxDirect pointer,
 * Ethernet padding included, link header and FCS excluded -- taken over
 * big-endian 16-bit words, an odd trailing byte padded with a zero low
 * byte.  Any 32-bit value whose ones-complement fold to 16 bits is that sum
 * is valid: a 16-bit result zero-extended, or a wider accumulation with
 * end-around carry.  Without the flag `sum` means nothing.
 *
 * ANXD_S2_RXF_VERIFIED: the driver has proved, by hardware or software, that
 * the payload is EITHER IPv4 with no options, not a fragment, total length
 * equal to `len`, a correct header checksum, and TCP or UDP (UDP with a
 * non-zero checksum) whose transport checksum is correct; OR IPv6 whose next
 * header is TCP or UDP directly, payload length matching `len`, with a
 * correct transport checksum.  It is checksum evidence only, not a promise
 * that the TCP or UDP header is otherwise sane.  IPv4 UDP checksum 0 means
 * "absent" and gets no flag; IPv6 UDP checksum 0 is invalid.  A frame the
 * driver could not verify is delivered without the flag -- no flag means
 * unverified, not corrupt.  Hardware that discards frames whose checksum it
 * found bad (the ZZ9000's GEM with its receive engine on) never delivers
 * them at all. */
#define ANXD_S2_RXF_SUMMED      0x01
#define ANXD_S2_RXF_VERIFIED    0x02

/* TxFlags results.  ANXD_S2_TXF_TCP / _UDP: the opener has written the
 * pseudo-header sum (source and destination address, protocol, transport
 * length; ones-complement, folded to 16 bits, NOT complemented) into the
 * frame's TCP/UDP checksum field instead of a checksum, and the driver must
 * put a correct checksum on the wire -- by its hardware, which may consume
 * the seed or want the field zeroed, or in software.  An opener sets a flag
 * only for a frame that is IPv4, not a fragment, whose total length equals
 * the frame's payload length and whose transport header is complete, and
 * only for a flag the driver accepted (ANXD_S2F_TX_CSUM_*) (ANX-012).  For
 * every frame flagged within those rules the driver puts a correct checksum
 * on the wire; a UDP checksum that computes to 0 must go out as 0xFFFF.  A
 * write without the flag is sent exactly as given. */
#define ANXD_S2_TXF_TCP         0x01
#define ANXD_S2_TXF_UDP         0x02
/* This write is one of a run the opener is sending back to back: the driver
   may hold the hardware's start until the run ends (ANXD_CMD_TX_FLUSH), a
   few more frames arrive, or its own backstop, so the frames leave the
   wire together.  Negotiated as ANXD_S2F_TX_MORE. */
#define ANXD_S2_TXF_MORE        0x40
/* This write is a batch (ANXD_S2F_TX_BATCH, TxBatch and TxChild below):
   one request, several wire frames.  Never set without the bit accepted.
   0x20 is not assigned: an experimental build used it. */
#define ANXD_S2_TXF_BATCH       0x10

/*
 * The callbacks below cross between separately built binaries, a driver from
 * one release calling a library from another, so their arguments go on the
 * stack whatever the compiler was told.  A GCC for AmigaOS predefines
 * __stdargs as that pin; a compiler without it already passes on the stack.
 * Unpinned under -mregparm, a library read a0/d0 for what an older driver had
 * pushed, and RxDirect answered an address made of its own code bytes.
 */
/* The callbacks' ABI, for any compiler or assembler: every argument is
 * passed on the stack in C order, each widened to 32 bits (a UBYTE flags
 * argument occupies a longword); the result is returned in D0; D0, D1, A0
 * and A1 are scratch and every other register is preserved.  The caller's
 * A4 is undefined: a callback that uses small data sets up its own (ANX-009).
 * A GCC for AmigaOS gets the rest from __stdargs below.
 *
 * Every callback can run in interrupt, server or vertical-blank context
 * (ANX-010): it must not block, Wait(), call a library function that may,
 * or re-enter the same device. */
#if defined(__GNUC__) && defined(__stdargs)
#define ANXD_S2_STDARGS __stdargs
#else
#define ANXD_S2_STDARGS
#endif

typedef ANXD_S2_STDARGS UBYTE *(*AnxdS2RxDirect)(APTR ios2_data, ULONG len);
typedef ANXD_S2_STDARGS VOID   (*AnxdS2RxFilled)(APTR ios2_data, ULONG len,
                                                 ULONG sum, UBYTE flags);
typedef ANXD_S2_STDARGS UBYTE  (*AnxdS2TxFlags)(APTR ios2_data);

/*
 * TX_BATCH: one CMD_WRITE, several wire frames.
 *
 * A write whose TxFlags carries ANXD_S2_TXF_BATCH is `Count` frames, 2 to
 * ANXD_S2_TX_BATCH_MAX, that leave the wire in order and back to back.  The
 * driver asks TxBatch(ios2_data, info) for the shape: it fills *info and
 * returns Count (0: not a batch after all, and the write is the ordinary
 * one frame of ios2_DataLength).  Frames 0 .. Count-2 are SegLen bytes and
 * the last is LastLen, each the payload of one frame as an ordinary
 * cooked write's ios2_DataLength would be: link header excluded, Ethernet
 * padding included, so neither is under 46.  ios2_DataLength is SegLen.
 * Every frame has the request's ios2_DstAddr and ios2_PacketType.
 *
 * TxChild(ios2_data, index, dst, len) copies frame `index` (len = its
 * length above) to `dst` and returns non-zero, or 0 when it cannot; it
 * takes the place of S2_CopyFromBuff for a batch.  Any index of the
 * request, in any order, any number of times: a second call for an index
 * copies the same bytes again.  The frames' checksums are as TxFlags
 * says, for every frame.
 *
 * Per-write state is the driver's and the request's, never the record's:
 * the driver keeps the request, with its own cursor, until every frame is
 * copied, then answers it once.  A ring that cannot take them all keeps the
 * request where it is, at the head of the unit's write queue, so it
 * resumes ahead of any later write.  AbortIO() and CloseDevice() answer it
 * IOERR_ABORTED with the frames copied so far gone to the wire; the driver
 * holds nothing of the opener's after its answer.  Statistics count frames,
 * not batches.
 */
typedef struct AnxdS2TxBatchInfo
{
    UWORD   Count;
    UWORD   SegLen;
    UWORD   LastLen;
    UWORD   Reserved;       /* 0 */
} AnxdS2TxBatchInfo;

#define ANXD_S2_TX_BATCH_MAX    16u

typedef ANXD_S2_STDARGS ULONG  (*AnxdS2TxBatch)(APTR ios2_data,
                                                AnxdS2TxBatchInfo *info);
typedef ANXD_S2_STDARGS ULONG  (*AnxdS2TxChild)(APTR ios2_data, ULONG index,
                                                UBYTE *dst, ULONG len);

/*
 * ONE VERSIONED NEGOTIATION TAG.
 *
 * This is a private extension between an opener and a driver, not a SANA-II
 * allocation.  Its tag therefore has an AmiNetXDuo-owned TAG_USER value and
 * does not borrow S2_Dummy or any of Commodore's buffer-hook offsets.
 *
 * The opener zeroes the record, writes VERSION, sizeof(record), Request and
 * the two receive callbacks, then supplies a pointer to it as ti_Data.  A
 * driver accepts only a Version it knows -- exactly 3 -- and a Size of at
 * least ANXD_S2_EXTENSION_SIZE (ANX-008, ANX-028), and writes Accepted as
 * the intersection it can honour for the selected unit.  An ordinary driver
 * ignores the tag and leaves Accepted zero.  Only the first valid record in
 * a taglist is taken; a later ANXD_S2_EXTENSION tag is ignored (ANX-006).
 *
 * Version 3 is frozen: the fields below, their offsets and their meaning do
 * not change.  Later fields may be appended after TxFlags under the same
 * Version; a driver reads one only when Size covers it, and never past
 * Size.  A change of meaning to any field below gets a new Version, which a
 * driver that does not know it ignores like an unknown tag.
 *
 * The record, callback code and every object the callbacks inspect remain
 * valid until CloseDevice().  Receive callbacks can run from the driver's
 * interrupt/server/vertical-blank service context and must not block.  The
 * transmit callback can also run while a queued write is advanced from such
 * a context.  All fields are native big-endian m68k ABI values; this is an
 * in-process driver interface, not a wire format.
 */
#define ANXD_S2_EXTENSION       (0x80000000UL | 0x00414e58UL) /* TAG_USER|'ANX' */
/* One version.  Earlier ones never left this tree, so a driver accepts 3
 * only; MIN is kept, equal, for drivers that range-check. */
#define ANXD_S2_ABI_VERSION_MIN 3u
#define ANXD_S2_ABI_VERSION     3u

#define ANXD_S2F_RX_DIRECT      (1UL << 0)
#define ANXD_S2F_RX_LINK_HDR    (1UL << 1)
#define ANXD_S2F_RX_VERIFIED    (1UL << 2)
#define ANXD_S2F_TX_CSUM_TCP    (1UL << 3)
#define ANXD_S2F_TX_CSUM_UDP    (1UL << 4)
#define ANXD_S2F_RX_POLL        (1UL << 5)
#define ANXD_S2F_RX_CAPACITY    (1UL << 6)
/* TX_QUICK: the driver honours IOF_QUICK on CMD_WRITE with ordinary Exec
 * semantics -- it may complete the request without replying, and does not
 * promise that a quick completion means the frame has left the wire. */
#define ANXD_S2F_TX_QUICK       (1UL << 7)
#define ANXD_S2F_RX_BATCH       (1UL << 8)
#define ANXD_S2F_TX_MORE        (1UL << 9)
/* Bit 10 is reserved and never assigned: an experimental build used it
   with a different record.  TX_BATCH needs a record whose Size covers
   TxChild (AnxdS2ExtensionTx below). */
#define ANXD_S2F_TX_BATCH       (1UL << 11)
#define ANXD_S2F_ALL            (ANXD_S2F_RX_DIRECT | \
                                 ANXD_S2F_RX_LINK_HDR | \
                                 ANXD_S2F_RX_VERIFIED | \
                                 ANXD_S2F_TX_CSUM_TCP | \
                                 ANXD_S2F_TX_CSUM_UDP | \
                                 ANXD_S2F_RX_POLL | \
                                 ANXD_S2F_RX_CAPACITY | \
                                 ANXD_S2F_TX_QUICK | \
                                 ANXD_S2F_RX_BATCH | \
                                 ANXD_S2F_TX_MORE | \
                                 ANXD_S2F_TX_BATCH)

/*
 * RxDirect(cookie, len): where the payload of the frame now being received
 * goes.  `cookie` is the CMD_READ's ios2_Data (or a batch Cookie), `len` the
 * payload bytes, link header excluded.  The opener returns an EVEN address
 * with room for `len` bytes, and, when RX_LINK_HDR was accepted, 14 more
 * bytes immediately before it, where the driver writes the Ethernet header
 * (destination, source, type) verbatim.  Both ends are therefore at least
 * 2-byte aligned and a driver may store words at dst-14, dst-12 and dst-10;
 * it must not assume longword alignment (ANX-029).  NULL declines: the driver delivers that frame through the
 * ordinary S2_CopyToBuff path instead.  After a non-NULL answer the driver
 * writes the bytes and calls RxFilled(cookie, len, sum, flags) once, from
 * the same service context, to report them complete.  It does not hand back
 * the IORequest, which is replied as usual.  One exception (ANX-010): a
 * frame the direct path then cannot take is copied through S2_CopyToBuff
 * instead, and if that copy fails, RxFilled is never called for the claim;
 * the opener must tolerate a claim that ends without it.  The opener keeps
 * the buffer and cookie valid until the request completes.
 */
typedef struct AnxdS2Extension
{
    UWORD           Version;    /*  0 */
    UWORD           Size;       /*  2 */
    ULONG           Request;    /*  4 */
    ULONG           Accepted;   /*  8 */
    AnxdS2RxDirect  RxDirect;   /* 12 */
    AnxdS2RxFilled  RxFilled;   /* 16 */
    /* Called for a negotiated CMD_WRITE, possibly from interrupt context.
       It may only inspect ios2_Data and returns ANXD_S2_TXF_* for that one
       request.  Per-write metadata therefore never occupies io_Flags, whose
       unassigned bits belong to the SANA-II/Exec request ABI. */
    AnxdS2TxFlags   TxFlags;    /* 20 */
} AnxdS2Extension;

/* The frozen version-3 prefix: everything through TxFlags, 24 bytes on m68k.
 * Tied to TxFlags, not sizeof(), so appending a field never raises the
 * minimum a driver accepts. */
#define ANXD_S2_EXTENSION_SIZE \
    (offsetof(AnxdS2Extension, TxFlags) + sizeof(AnxdS2TxFlags))

/* The version-3 record with the fields appended after it, for an opener
 * that offers TX_BATCH: the frozen prefix unchanged, then two reserved
 * pointers the opener leaves NULL and a driver never reads (an
 * experimental build put other callbacks there), then the batch pair.  A
 * driver reads TxBatch and TxChild only when Size is at least
 * ANXD_S2_EXTENSION_BATCH_SIZE, 40 bytes on m68k. */
typedef struct AnxdS2ExtensionTx
{
    AnxdS2Extension Base;           /*  0 */
    APTR            Reserved[2];    /* 24 */
    AnxdS2TxBatch   TxBatch;        /* 32 */
    AnxdS2TxChild   TxChild;        /* 36 */
} AnxdS2ExtensionTx;

#define ANXD_S2_EXTENSION_BATCH_SIZE \
    (offsetof(AnxdS2ExtensionTx, TxChild) + sizeof(AnxdS2TxChild))

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
 * either the opener stops sending it.  An accepted RX_POLL on an offline
 * unit succeeds and delivers nothing.
 *
 * Private commands use the NSD third-party block ($8000-$BFFF; $4000-$7FFF
 * is the OS team's).  That block is not an allocation: these numbers mean
 * what this header says only to an opener whose ANXD negotiation succeeded;
 * any other opener must not send them (ANX-017).  $8191 is reserved and
 * never assigned.  An unknown command answers IOERR_NOCMD; a known one the
 * opener did not get accepted answers S2ERR_NOT_SUPPORTED. */
#define ANXD_CMD_RX_POLL        0x8190

/* ANXD_CMD_RX_CAPACITY: "how much may a peer put on the wire at once?"
 *
 * Answered in ios2_DataLength: an ADVISORY receive window for this unit, in
 * bytes of received frames -- without flow control, what the unit's own
 * receive memory holds at line rate while nobody drains it (the ring or FIFO
 * after which the next frame is lost).  It is advice for the opener's
 * window, not a hardware register: a driver may answer a measured value
 * where that is what keeps the unit lossless (PAUSE, below).  0 means "no
 * advice", whether the limit is unknown or absent.  The value is per unit
 * and shared by every opener and every connection on it: an opener running
 * several flows should keep the sum of their windows within it rather than
 * grant it to each (ANX-018).
 * Quick, no arguments, io_Error 0; IOERR_NOCMD from a driver that does not
 * know it.
 *
 * The answer can change while the unit is online.  A card whose link has
 * IEEE 802.3x PAUSE agreed is not limited by its ring: the partner holds
 * what the ring cannot take.  It answers what it measured best with the
 * partner pausing for it, NOT 0 -- a paused X-Surf 100 was slower at 64
 * frames than at 32 -- and its ring again when a renegotiation loses PAUSE.
 * An opener that keeps a window to the answer asks again while traffic
 * flows (sana2_rx.c, at most once a second).  A smaller answer cannot take
 * back window already advertised: the opener stops offering more than the
 * new value from then on and lets the old right edge be consumed.
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
 * Version ANXD_S2_RX_BATCH_VERSION, Size covering Count cookies, Count > 0
 * and Filled 0.  An accepted batch is always queued (IOF_QUICK is cleared
 * as for CMD_READ) and answered on mn_ReplyPort like any read; a batch
 * refused at once (bad record, unsupported) may complete quick with its
 * error (ANX-016).  Each
 * cookie is what the opener's RxDirect/RxFilled pair receive as ios2_data
 * for that slot, exactly as they receive a CMD_READ's ios2_Data today; the
 * opener therefore needs ANXD_S2F_RX_DIRECT and ANXD_S2F_RX_LINK_HDR
 * accepted, because a batch has no per-frame ios2_SrcAddr/DstAddr/
 * PacketType: the 14-byte link header written in front of each payload is
 * the frame's whole identity.  The driver fills Cookie[0], Cookie[1], ...
 * in arrival order, one RxDirect() then one RxFilled() per frame (with the
 * failed-copy exception under RxDirect), and writes Filled.  A frame the direct path cannot take (a second opener's
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
 * its next periodic tick, so a held start waits one tick at most.  Holding
 * and flushing are per unit, not per opener: a flush starts every opener's
 * held writes, and a write without the flag starts what was held before it.
 * "Started" is not "on the wire" (ANX-014).  Quick, no arguments, io_Error
 * 0; IOERR_NOCMD from a driver that does not know it, S2ERR_NOT_SUPPORTED
 * from a unit that cannot hold a start. */
#define ANXD_CMD_TX_FLUSH       0x8194

typedef struct AnxdS2RxBatch
{
    UWORD   Version;        /* ANXD_S2_RX_BATCH_VERSION                  */
    UWORD   Size;           /* bytes allocated, including Cookie[]      */
    UWORD   Count;          /* cookies the opener supplies              */
    UWORD   Filled;         /* written by the driver: frames delivered  */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
    APTR    Cookie[];       /* Count of them; RxDirect/RxFilled cookies */
#else
    APTR    Cookie[1];      /* C89 (SAS/C): the same offset, 8 */
#endif
} AnxdS2RxBatch;

#define ANXD_S2_RX_BATCH_VERSION  1u

/* The size of a batch record holding n cookies. */
/* The size of a batch record holding n cookies, from the offset of Cookie
 * so a C89 compiler (Cookie[1]) and a C99 one (Cookie[]) agree; the header
 * is 8 bytes on m68k.  Never sizeof(AnxdS2RxBatch). */
#define ANXD_S2_RX_BATCH_SIZE(n) \
    (offsetof(AnxdS2RxBatch, Cookie) + (n) * sizeof(APTR))

#endif /* AMINETXDUO_ANXS2EXT_H */
