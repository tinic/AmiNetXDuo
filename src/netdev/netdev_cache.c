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

/*
 * TT registers and CACR belong to the CPU, not to a device unit.  A per-unit
 * saved value lets unit A restore a setting while unit B still depends on it.
 * Keep the ownership here instead.  Device Open()/Close() calls are task
 * serialized by NetdevDevice.nd_LifecycleLock; attach is serialized by the
 * resident initializer.
 */
typedef struct NdTtLease
{
    ULONG value;               /* the exact value installed in the register */
    ULONG saved;               /* the exact value that preceded it          */
    UWORD refs;
} NdTtLease;

static NdTtLease nd_tt_lease[2];
static ULONG     nd_dc_saved;
static UWORD     nd_dc_refs;

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
 * The machine.  A host test supplies these seven; the device below them
 * runs the real thing.
 */
#ifdef NETDEV_CACHE_TEST
BOOL  nd_cache_is_030(VOID);
VOID  nd_cache_tt_read(ULONG *tt);              /* tt[0] = TT0, tt[1] = TT1 */
VOID  nd_cache_tt_write(UWORD which, ULONG v);
VOID  nd_cache_flush(VOID);
ULONG nd_cache_dcache(BOOL on);                 /* returns the old CACR */
ULONG nd_cache_cacr_read(VOID);                  /* CacheControl(0, 0)  */
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
/*
 * VOID nd_super_call(fn, operand, sysbase): Supervisor(fn) with a0 = operand.
 * Out of line because Supervisor() takes the routine in a5, and a5 is the
 * frame pointer of any caller LTO chooses to frame: a register-asm variable
 * pinned to a5 let the inlined call overwrite the caller's frame pointer and
 * its unlk/rts returned into the stub's opcode words (A3000 Line-F reboot).
 * Here a5 and a6 are saved and restored around the call, whatever the caller.
 */
"    .globl _nd_super_call\n"
"_nd_super_call:\n"
"    movem.l %a5-%a6,-(%sp)\n"
"    move.l 12(%sp),%a5\n"          /* fn      */
"    move.l 16(%sp),%a0\n"          /* operand */
"    move.l 20(%sp),%a6\n"          /* sysbase */
"    jsr -30(%a6)\n"                /* Supervisor() */
"    movem.l (%sp)+,%a5-%a6\n"
"    rts\n"
);
extern VOID nd_sup_tt_read(VOID);
extern VOID nd_sup_tt0_write(VOID);
extern VOID nd_sup_tt1_write(VOID);

extern VOID nd_super_call(VOID (*fn)(VOID), APTR operand,
                          struct ExecBase *sysbase);

static VOID nd_super(VOID (*fn)(VOID), APTR operand)
{
    nd_super_call(fn, operand, SysBase);
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

static ULONG nd_cache_cacr_read(VOID)
{
    return CacheControl(0UL, 0UL);
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

static BOOL nd_tt_covers(ULONG tt, ULONG board, ULONG size)
{
    ULONG mask = (tt >> 16) & 0xffUL;
    ULONG base = (tt >> 24) & 0xffUL;
    ULONG first = board >> 24;
    ULONG last = (size != 0) ? ((board + size - 1UL) >> 24) : first;

    return (BOOL)(((first & ~mask) == base) && ((last & ~mask) == base));
}

/* Restore only a value that is still ours.  An MMU tool is entitled to have
   changed the register while anxnet.device was resident; overwriting its new
   value is worse than leaving our old mapping behind. */
static VOID nd_tt_restore_if_ours(UWORD which, ULONG ours, ULONG saved)
{
    ULONG tt[2];

    nd_cache_tt_read(tt);
    if (tt[which] == ours)
    {
        nd_cache_tt_write(which, saved);
        nd_cache_flush();
    }
}

static VOID nd_tt_release(UWORD which)
{
    NdTtLease *lease = &nd_tt_lease[which];

    if (lease->refs == 0)
        return;

    if (--lease->refs == 0)
    {
        nd_tt_restore_if_ours(which, lease->value, lease->saved);
        lease->value = 0;
        lease->saved = 0;
    }
}

static VOID nd_dc_claim(VOID)
{
    if (nd_dc_refs++ == 0)
        nd_dc_saved = nd_cache_dcache(FALSE);
}

static VOID nd_dc_release(VOID)
{
    if (nd_dc_refs == 0)
        return;

    if (--nd_dc_refs == 0)
    {
        ULONG current = nd_cache_cacr_read();

        /* We own ED=0, not the whole CACR.  Restore only while ED still has
           our value.  A cache utility that changed it during our lease wins,
           exactly as an external TT-register change does above. */
        if ((current & CACR_ED) == 0 && (nd_dc_saved & CACR_ED) != 0)
            (VOID)nd_cache_dcache(TRUE);
        nd_dc_saved = 0;
    }
}

static UBYTE nd_cache_select(NetdevNic *nic, BOOL reacquire)
{
    ULONG tt[2];
    ULONG want;
    ULONG lo;
    ULONG hi;
    UWORD which;
    UWORD step;
    ULONG board = nic->cache_board;
    ULONG size = nic->cache_board_size;

    nic->cache_guard = NETDEV_CACHE_NONE;

    /* Only a core that can ask its chip whether reads are honest takes part,
       and only a 68030 has the problem. */
    if (nic->ops->coherent == NULL || !nd_cache_is_030())
    {
        nic->cache_mode = NETDEV_CACHE_NONE;
        return NETDEV_CACHE_NONE;
    }

    want = netdev_cache_tt_value(board, size, &lo, &hi);

    /* A data-cache lease affects every address.  Join it before the first
       board read: probing first would perform the very stale access the guard
       exists to prevent, and a second unit must not later restore A's CACR. */
    if (nd_dc_refs != 0)
    {
        nd_dc_claim();
        if (nic->ops->coherent(nic))
        {
            nic->cache_guard = NETDEV_CACHE_DCACHE;
            if (!reacquire)
                nic->cache_mode = NETDEV_CACHE_DCACHE;
            return nic->cache_guard;
        }
        nd_dc_release();
    }

    /* Likewise share a TT mapping installed by another unit when it covers
       this board.  Confirm the CPU still contains our exact value before
       treating the software lease as protection. */
    nd_cache_tt_read(tt);
    for (which = 0; which < 2; which++)
    {
        NdTtLease *lease = &nd_tt_lease[which];

        if (lease->refs != 0 && tt[which] == lease->value &&
            nd_tt_covers(lease->value, board, size))
        {
            lease->refs++;
            if (nic->ops->coherent(nic))
            {
                nic->cache_guard = (UBYTE)(NETDEV_CACHE_TT0 + which);
                if (!reacquire)
                    nic->cache_mode = nic->cache_guard;
                return nic->cache_guard;
            }
            nd_tt_release(which);
        }
    }

    /* A reacquire must not begin by touching an unguarded board.  Reinstall
       the attach probe's global-cache fallback first, then prove it. */
    if (reacquire && nic->cache_mode == NETDEV_CACHE_DCACHE)
    {
        nd_dc_claim();
        if (nic->ops->coherent(nic))
        {
            nic->cache_guard = NETDEV_CACHE_DCACHE;
            return nic->cache_guard;
        }
        nd_dc_release();
    }

    /* The sole unguarded discovery read is attach-only.  Even if it answers
       coherent, the reason may be somebody else's disabled D-cache, TT entry
       or MMU mapping.  Those can disappear while the device is resident, so
       continue down the ladder and establish a guard we can reacquire before
       Open's first board access.  There is no safe way to turn off an unknown
       MMU mapping temporarily to prove the hardware coherent without it. */
    if (!reacquire)
        (VOID)nic->ops->coherent(nic);

    if (nd_cache_ram_in(lo, hi))
        nic->cache_why |= NETDEV_CACHE_WHY_RAM;
    else
    {
        nd_cache_tt_read(tt);
        for (step = 0; step < 2; step++)
        {
            NdTtLease *lease;

            /* Reacquire the TT attach selected before trying its sibling. */
            which = (UWORD)(((reacquire &&
                              nic->cache_mode == NETDEV_CACHE_TT1) ? 1 : 0) ^
                            step);
            lease = &nd_tt_lease[which];

            if (lease->refs != 0 || (tt[which] & TT_E) != 0)
            {
                /* Somebody's: leave it alone. */
                nic->cache_why |= (UBYTE)(NETDEV_CACHE_WHY_TT0_BUSY << which);
                continue;
            }

            nd_cache_tt_write(which, want);
            nd_cache_flush();
            if (nic->ops->coherent(nic))
            {
                lease->value = want;
                lease->saved = tt[which];
                lease->refs  = 1;
                nic->cache_guard = (UBYTE)(NETDEV_CACHE_TT0 + which);
                if (!reacquire)
                    nic->cache_mode = nic->cache_guard;
                return nic->cache_guard;
            }

            /* It did not help.  Put it back only if nobody changed it while
               the probe ran (the same rule used for the lasting lease). */
            nic->cache_why |= (UBYTE)(NETDEV_CACHE_WHY_TT0_NOHELP << which);
            nd_tt_restore_if_ours(which, want, tt[which]);
            nd_cache_tt_read(tt);
        }
    }

    /* The vendor's own prescription. */
    nd_dc_claim();
    if (nic->ops->coherent(nic))
    {
        nic->cache_guard = NETDEV_CACHE_DCACHE;
        if (!reacquire)
            nic->cache_mode = NETDEV_CACHE_DCACHE;
        return nic->cache_guard;
    }

    /* Not the cache, then.  Attach will find out what it is. */
    nd_dc_release();
    nic->cache_guard = NETDEV_CACHE_FAILED;
    if (!reacquire)
        nic->cache_mode = NETDEV_CACHE_FAILED;

    return nic->cache_guard;
}

UBYTE netdev_cache_guard(struct NetdevNic *nic, ULONG board, ULONG size)
{
    nic->cache_board      = board;
    nic->cache_board_size = size;
    nic->cache_guard      = NETDEV_CACHE_NONE;
    nic->cache_mode       = NETDEV_CACHE_NONE;
    nic->cache_why        = 0;

    return nd_cache_select(nic, FALSE);
}

UBYTE netdev_cache_acquire(struct NetdevNic *nic)
{
    if (nic->cache_guard != NETDEV_CACHE_NONE)
        return nic->cache_guard;
    if (nic->cache_mode == NETDEV_CACHE_NONE ||
        nic->cache_board_size == 0)
        return NETDEV_CACHE_NONE;
    if (nic->cache_mode == NETDEV_CACHE_FAILED)
        return NETDEV_CACHE_FAILED;

    /* Re-run the ladder.  It first shares a CPU-global lease, and otherwise
       reinstalls a free TT or the CACR fallback.  This also handles an MMU
       tool legitimately taking the TT register between attach and Open(). */
    return nd_cache_select(nic, TRUE);
}

VOID netdev_cache_release(struct NetdevNic *nic)
{
    switch (nic->cache_guard)
    {
    case NETDEV_CACHE_TT0:
    case NETDEV_CACHE_TT1:
        nd_tt_release((UWORD)(nic->cache_guard - NETDEV_CACHE_TT0));
        break;
    case NETDEV_CACHE_DCACHE:
        nd_dc_release();
        break;
    default:
        break;
    }

    nic->cache_guard = NETDEV_CACHE_NONE;
}
