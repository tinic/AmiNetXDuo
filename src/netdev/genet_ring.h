/*
 * anxgenet.device: the arithmetic under the GENET's ring walk, inline.
 *
 * The chip keeps 16-bit producer and consumer counters that run past the
 * ring's size and wrap at 65536; the ring's slot is the low bits.  Every sum
 * here runs once per burst or once per frame in genet.c's receive and
 * transmit passes, so nothing here is a call: the register reads, the cache
 * operations and the descriptor writes stay where they were, and these are
 * the values between them.  The ring's configuration words are here too, so
 * that a constant ring size folds to a constant word as it did.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_GENET_RING_H
#define AMINETXDUO_GENET_RING_H

#include <exec/types.h>

#include "genetreg.h"
#include "aminetxduo/anxs2ext.h"   /* ANXD_S2_TXF_UDP */

/* Of `total` frames waiting from `cidx`, how many arrived since the cache
   operation that reached `clean`: a `clean` that is not between the two
   counters starts over with all of them. */
static inline UWORD genet_ring_fresh(UWORD total, UWORD cidx, UWORD clean)
{
    UWORD done = (UWORD)(clean - cidx);

    if (done > total)
        done = 0;
    return (UWORD)(total - done);
}

/* The slot a counter names.  `ring` is a power of two. */
static inline UWORD genet_ring_slot(UWORD counter, UWORD ring)
{
    return (UWORD)(counter & (UWORD)(ring - 1));
}

/* Slots from that one to the ring's end, so a run of n frames is one range
   when n <= room and two otherwise. */
static inline UWORD genet_ring_room(UWORD counter, UWORD ring)
{
    return (UWORD)(ring - genet_ring_slot(counter, ring));
}

/* The ring's configuration words, for ge_init_rings.  RING_BUF_SIZE: the
   descriptor count in the high half, the buffer length in the low. */
static inline ULONG genet_ring_size_word(UWORD ring, UWORD bufsz)
{
    return ((ULONG)ring << 16) | bufsz;
}

/* END_ADDR_LO: the last longword of the ring's descriptor block. */
static inline ULONG genet_ring_end_word(UWORD ring)
{
    return (ULONG)ring * GENET_DMA_DESC_SIZE / 4 - 1;
}

/* RX_DMA_XON_XOFF_THRES: pause the wire at five free buffers, resume at a
   sixteenth of the ring. */
static inline ULONG genet_ring_xon_xoff_word(UWORD ring)
{
    return (5UL << 16) | (ULONG)(ring >> 4);
}

/* RX_DMA_RING_TIMEOUT with its timeout field replaced. */
static inline ULONG genet_ring_timeout_word(ULONG v, ULONG ticks)
{
    return (v & ~GENET_DMA_RING_TIMEOUT_MASK) | ticks;
}

/* The pages a range touches, for the supervised cpushp loop.  `len` is at
   least 1 and addr + len does not wrap: a zero length or a range past the
   top of the address space reads one page too few or underflows. */
#define GENET_PAGE          4096UL

static inline ULONG genet_page_first(ULONG addr)
{
    return addr & ~(GENET_PAGE - 1UL);
}

static inline ULONG genet_page_count(ULONG addr, ULONG len)
{
    ULONG first = genet_page_first(addr);
    ULONG last  = (addr + len - 1UL) & ~(GENET_PAGE - 1UL);

    return (last - first) / GENET_PAGE + 1UL;
}

/* The transmit descriptor's status word for a frame of `len` bytes behind
   the 64-byte status block: one descriptor, the chip's CRC, the default
   queue tag. */
static inline ULONG genet_tx_desc_status(UWORD len)
{
    return GENET_TX_DESC_STATUS_SOP | GENET_TX_DESC_STATUS_EOP |
           GENET_TX_DESC_STATUS_CRC | GENET_TX_DESC_STATUS_QTAG |
           GENET_TX_DESC_STATUS_BUFLEN(len + GENET_TX_STATUS64_LEN);
}

/* The status block's checksum word, without LEN_VALID: where the transport
   header starts (the Ethernet header plus the IP header's IHL) and where
   the checksum field sits, both from the frame's start, and the UDP bit
   when the flag says UDP.  `ihl` is the IP header's first byte. */
static inline ULONG genet_tx_csum_info(UBYTE ihl, UWORD offset, UBYTE flag)
{
    ULONG start = 14UL + ((ULONG)(ihl & 0x0Fu) << 2);
    ULONG info  = (start << GENET_TX_CSUM_START_SHIFT) | (ULONG)offset;

    if (flag == ANXD_S2_TXF_UDP)
        info |= GENET_TX_CSUM_UDP;
    return info;
}

/*
 * Whether the checksum block's word worked out for one frame holds for the
 * next (genet_tx_run): every byte netdev_tx_csum4() reads to decide and
 * place the checksum, other than the lengths, is the same -- the EtherType,
 * the IP version and header length, the fragment word, the protocol and
 * the TCP data offset -- and the protocol is TCP, whose verdict reads no
 * length that differs between a batch's frames (UDP's reads its own length
 * field, so a UDP frame is always worked out afresh).  The length checks
 * there pass for every frame of a batch alike: each is at least as long as
 * its own IP datagram, and each datagram at least its two headers.
 */
#define GENET_TX_SHAPE_LEN  7

static inline UWORD genet_tx_thl_at(const UBYTE *frame)
{
    return (UWORD)(14u + ((UWORD)(frame[14] & 0x0Fu) << 2) + 12u);
}

static inline VOID genet_tx_csum_shape(const UBYTE *frame, UBYTE *shape)
{
    shape[0] = frame[12];
    shape[1] = frame[13];
    shape[2] = frame[14];       /* version, IHL     */
    shape[3] = frame[20];       /* flags, fragment  */
    shape[4] = frame[21];
    shape[5] = frame[23];       /* protocol         */
    shape[6] = frame[genet_tx_thl_at(frame)];
}

static inline BOOL genet_tx_csum_same(const UBYTE *frame, const UBYTE *shape)
{
    return (BOOL)(shape[5] == 6u &&
                  frame[12] == shape[0] && frame[13] == shape[1] &&
                  frame[14] == shape[2] && frame[20] == shape[3] &&
                  frame[21] == shape[4] && frame[23] == shape[5] &&
                  frame[genet_tx_thl_at(frame)] == shape[6]);
}

/*
 * The receive line's mask while a reader is behind.  A pass that stopped at
 * a frame whose reader had no read posted (`held`) consumed nothing, and an
 * unmasked RXDMA_DONE would fire again at once; so the bit is taken out of
 * `rearm`, remembered in *line_held and counted in *withheld.  The first
 * pass that gets past that frame puts the bit back.  Returns the bits to
 * unmask; the register write stays with the caller.
 */
static inline ULONG genet_rx_line_rearm(UBYTE held, UBYTE *line_held,
                                        ULONG rearm, ULONG *withheld)
{
    if (held)
    {
        if ((rearm & GENET_IRQ_RXDMA_DONE) != 0)
        {
            rearm &= ~GENET_IRQ_RXDMA_DONE;
            *line_held = 1;
            (*withheld)++;
        }
    }
    else if (*line_held)
    {
        *line_held = 0;
        rearm |= GENET_IRQ_RXDMA_DONE;
    }
    return rearm;
}

#endif /* AMINETXDUO_GENET_RING_H */
