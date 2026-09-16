/*
 * anxnet.device: keeping a 68030's data cache off a Zorro III board.
 * netdev_cache.h says why.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_cache.h"
#include "netdev_nic.h"

/* TT0/TT1 (MC68030 User's Manual 9.7.3, figure 9-37; NetBSD's mmu_30.h has
   the bits): logical address base 31-24, mask 23-16, E 15, CI 10, R/W 9,
   RWM 8, FC base 6-4, FC mask 2-0.  The first cut had CI at bit 8 and the
   A3000 reported "TT0 set did not help, TT1 set did not help": the block
   was translated, and still cached. */
#define TT_E        0x00008000UL
#define TT_CI       0x00000400UL
#define TT_RWM      0x00000100UL
#define TT_FCMASK   0x00000007UL

#define CACR_ED     0x00000100UL    /* CACRF_EnableD */

ULONG netdev_cache_tt_value(ULONG board, ULONG size, ULONG *lo, ULONG *hi)
{
    ULONG first = board >> 24;
    /* board + (size - 1), never board + size: the top block's end is the
       top of memory, and that sum wraps. */
    ULONG last  = (size != 0) ? ((board + (ULONG)(size - 1UL)) >> 24) : first;
    ULONG mask  = 0;

    /* Widen the ignored address bits until one block holds both ends. */
    while ((first & ~mask) != (last & ~mask))
        mask = (mask << 1) | 1UL;

    first &= ~mask;
    *lo = first << 24;
    *hi = ((first + mask + 1UL) << 24) & 0xffffffffUL;  /* 0 past the top */

    return (first << 24) | (mask << 16) | TT_E | TT_RWM | TT_CI | TT_FCMASK;
}

/*
 * The machine.  A host test supplies these six; the device below them
 * runs the real thing.
 */
#ifdef NETDEV_CACHE_TEST
BOOL  nd_cache_is_030(VOID);
VOID  nd_cache_tt_read(ULONG *tt);              /* tt[0] = TT0, tt[1] = TT1 */
VOID  nd_cache_tt_write(UWORD which, ULONG v);
VOID  nd_cache_flush(VOID);
ULONG nd_cache_dcache(BOOL on);                 /* returns the old CACR */
BOOL  nd_cache_ram_in(ULONG lo, ULONG hi);
#else

#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>

extern struct ExecBase *SysBase;

/*
 * The four instructions a 68000 assembler will not take, as words: the
 * device is built for every CPU and only ever runs these on a 68030.  All
 * from Supervisor(), so each ends in RTE.  a0 carries the operand address.
 */
__asm__(
"    .text\n"
"    .globl _nd_sup_tt_read\n"
"_nd_sup_tt_read:\n"
"    .short 0xf010,0x0a00\n"     /* pmove tt0,(a0) */
"    addq.l #4,%a0\n"
"    .short 0xf010,0x0e00\n"     /* pmove tt1,(a0) */
"    rte\n"
"    .globl _nd_sup_tt0_write\n"
"_nd_sup_tt0_write:\n"
"    .short 0xf010,0x0800\n"     /* pmove (a0),tt0 */
"    rte\n"
"    .globl _nd_sup_tt1_write\n"
"_nd_sup_tt1_write:\n"
"    .short 0xf010,0x0c00\n"     /* pmove (a0),tt1 */
"    rte\n"
);
extern VOID nd_sup_tt_read(VOID);
extern VOID nd_sup_tt0_write(VOID);
extern VOID nd_sup_tt1_write(VOID);

static VOID nd_super(VOID (*fn)(VOID), APTR operand)
{
    register APTR             _a0 __asm("a0") = operand;
    register VOID           (*_a5)(VOID) __asm("a5") = fn;
    register struct ExecBase *_a6 __asm("a6") = SysBase;

    /* Supervisor(): the routine runs in supervisor mode and ends in RTE. */
    __asm__ __volatile__ ("jsr a6@(-30:W)"
                          : "+r" (_a0)
                          : "r" (_a5), "r" (_a6)
                          : "cc", "memory", "d0", "d1", "a1");
}

static BOOL nd_cache_is_030(VOID)
{
    /* A 68040 or 68060 sets the 68030 flag too. */
    return (BOOL)((SysBase->AttnFlags & (AFF_68030 | AFF_68040 | AFF_68060))
                  == AFF_68030);
}

static VOID nd_cache_tt_read(ULONG *tt)
{
    nd_super(nd_sup_tt_read, tt);
}

static VOID nd_cache_tt_write(UWORD which, ULONG v)
{
    ULONG operand = v;

    nd_super((which == 0) ? nd_sup_tt0_write : nd_sup_tt1_write, &operand);
}

static VOID nd_cache_flush(VOID)
{
    CacheClearU();
}

static ULONG nd_cache_dcache(BOOL on)
{
    return CacheControl(on ? CACR_ED : 0UL, CACR_ED);
}

/* Any memory Exec hands out inside the block: the transparent translation
   would make that RAM as slow as chip RAM for every task on the machine. */
static BOOL nd_cache_ram_in(ULONG lo, ULONG hi)
{
    struct MemHeader *mh;
    BOOL              hit = FALSE;

    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ != NULL;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
    {
        ULONG mlo = (ULONG)mh->mh_Lower;
        ULONG mhi = (ULONG)mh->mh_Upper;

        if (mlo < (hi != 0 ? hi : 0xffffffffUL) && mhi > lo)
        {
            hit = TRUE;
            break;
        }
    }
    Permit();

    return hit;
}
#endif /* NETDEV_CACHE_TEST */

UBYTE netdev_cache_guard(struct NetdevNic *nic, ULONG board, ULONG size)
{
    ULONG tt[2];
    ULONG want;
    ULONG lo;
    ULONG hi;
    ULONG cacr;
    UWORD which;

    nic->cache_guard = NETDEV_CACHE_NONE;
    nic->cache_saved = 0;
    nic->cache_why   = 0;

    /* Only a core that can ask its chip whether reads are honest takes part,
       and only a 68030 has the problem. */
    if (nic->ops->coherent == NULL || !nd_cache_is_030())
        return NETDEV_CACHE_NONE;

    /* An MMU manager's tables, a data cache somebody already turned off:
       nothing to do, and nothing touched. */
    if (nic->ops->coherent(nic))
        return NETDEV_CACHE_NONE;

    want = netdev_cache_tt_value(board, size, &lo, &hi);

    if (nd_cache_ram_in(lo, hi))
        nic->cache_why |= NETDEV_CACHE_WHY_RAM;
    else
    {
        nd_cache_tt_read(tt);
        for (which = 0; which < 2; which++)
        {
            if ((tt[which] & TT_E) != 0)
            {
                /* Somebody's: leave it alone. */
                nic->cache_why |= (UBYTE)(NETDEV_CACHE_WHY_TT0_BUSY << which);
                continue;
            }

            nd_cache_tt_write(which, want);
            nd_cache_flush();
            if (nic->ops->coherent(nic))
            {
                nic->cache_guard = (UBYTE)(NETDEV_CACHE_TT0 + which);
                nic->cache_saved = tt[which];
                return nic->cache_guard;
            }

            /* It did not help, so it was not the answer: put it back. */
            nic->cache_why |= (UBYTE)(NETDEV_CACHE_WHY_TT0_NOHELP << which);
            nd_cache_tt_write(which, tt[which]);
            nd_cache_flush();
        }
    }

    /* The vendor's own prescription. */
    cacr = nd_cache_dcache(FALSE);
    if (nic->ops->coherent(nic))
    {
        nic->cache_guard = NETDEV_CACHE_DCACHE;
        nic->cache_saved = cacr;
        return nic->cache_guard;
    }

    /* Not the cache, then.  Attach will find out what it is. */
    if ((cacr & CACR_ED) != 0)
        (VOID)nd_cache_dcache(TRUE);
    nic->cache_guard = NETDEV_CACHE_FAILED;

    return nic->cache_guard;
}

VOID netdev_cache_release(struct NetdevNic *nic)
{
    switch (nic->cache_guard)
    {
    case NETDEV_CACHE_TT0:
    case NETDEV_CACHE_TT1:
        nd_cache_tt_write((UWORD)(nic->cache_guard - NETDEV_CACHE_TT0),
                          nic->cache_saved);
        nd_cache_flush();
        break;
    case NETDEV_CACHE_DCACHE:
        if ((nic->cache_saved & CACR_ED) != 0)
            (VOID)nd_cache_dcache(TRUE);
        break;
    default:
        break;
    }

    nic->cache_guard = NETDEV_CACHE_NONE;
}
