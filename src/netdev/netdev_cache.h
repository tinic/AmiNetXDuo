/*
 * anxnet.device: keeping a 68030's data cache off a Zorro III board.
 *
 * A Zorro II board is cache-inhibited by the machine (Buster asserts CIIN
 * for that space), a 68040 or 68060's library maps every board non-cacheable
 * in its MMU tables, and a 68020 has no data cache.  A 68030 driving a Zorro
 * III board has none of that: every register read fills a cache entry and
 * the next read of the same register returns the entry, so a status bit
 * never changes and a port read repeats its first word.  The X-Surf 100 in
 * Zorro III mode on an A3000 failed its buffer test exactly so: "$0002: $49
 * written, $54 read back" is the port's first word served again.  iComp's
 * documentation states the requirement: with a 68(EC)030 the data cache
 * must be off while the chip is accessed.
 *
 * The 68030 has two transparent-translation registers, TT0 and TT1, each of
 * which marks an address block cache-inhibited without page tables and
 * whether or not the MMU is enabled, so the first choice is a free one over
 * the board's 16 MB block.  Both in use (an MMU manager's), or RAM sharing
 * the block: the data cache is turned off for as long as this driver holds
 * the board, which is what the vendor asks for and costs everything else on
 * the machine a little.
 *
 * Applied only when the core's coherence probe says it is needed, and
 * proven by the same probe afterwards; netdev_cache_release() undoes it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_CACHE_H
#define AMINETXDUO_NETDEV_CACHE_H

#include <exec/types.h>

struct NetdevNic;

#define NETDEV_CACHE_NONE       0   /* coherent as found, or not a 68030   */
#define NETDEV_CACHE_TT0        1   /* TT0 marks the board's block          */
#define NETDEV_CACHE_TT1        2   /* TT1 does                             */
#define NETDEV_CACHE_DCACHE     3   /* the data cache is off while held     */
#define NETDEV_CACHE_FAILED     4   /* nothing made the board coherent      */

/* Why the transparent-translation rungs were passed over, as bits in
   NetdevNic.cache_why: the answer CheckNetDevice gives beside the verdict. */
#define NETDEV_CACHE_WHY_RAM    0x01    /* Exec memory shares the block      */
#define NETDEV_CACHE_WHY_TT0_BUSY 0x02  /* TT0 enabled by somebody else      */
#define NETDEV_CACHE_WHY_TT1_BUSY 0x04  /* TT1 enabled by somebody else      */
#define NETDEV_CACHE_WHY_TT0_NOHELP 0x08 /* TT0 set, board still stale       */
#define NETDEV_CACHE_WHY_TT1_NOHELP 0x10 /* TT1 set, board still stale       */

/*
 * The transparent-translation value for the smallest 16 MB-aligned block
 * holding [board, board + size): base and mask fields, enabled, both read
 * and write, cache-inhibited, every function code.  *lo and *hi receive the
 * block's bounds, hi exclusive, for the RAM test.
 */
ULONG netdev_cache_tt_value(ULONG board, ULONG size, ULONG *lo, ULONG *hi);

/* Before attach.  Returns and records the NETDEV_CACHE_* it settled on. */
UBYTE netdev_cache_guard(struct NetdevNic *nic, ULONG board, ULONG size);

/* At expunge, or after an attach that failed anyway. */
VOID  netdev_cache_release(struct NetdevNic *nic);

#endif /* AMINETXDUO_NETDEV_CACHE_H */
