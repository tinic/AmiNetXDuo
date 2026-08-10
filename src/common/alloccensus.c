/*
 * AmiNetXDuo, the allocation census.
 *
 * A side table, not a header in front of the block.  Wrapping every
 * allocation in a header would move every pointer the product hands around,
 * change what fits in a cache line and what shares one, and give the census
 * build a different memory layout from the one being investigated.  An
 * instrument that changes the thing it measures answers a different question.
 * The cost is a hash probe per alloc and per free, which nothing here is
 * timed on.
 *
 * The table is open-addressed on the pointer with linear probing and never
 * deletes by tombstone: a drop clears the slot and then re-inserts whatever
 * follows in the same probe run, so a full table degrades into a refusal to
 * record rather than into wrong answers.  A refusal is COUNTED, and the
 * harness fails the run on it, because a census that silently stopped
 * recording reads exactly like a clean sheet.
 *
 * Everything in this file is compiled only when AMINETXDUO_ALLOCCENSUS is
 * defined.  With the flag off the object is empty and the shipping binaries
 * are byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/alloccensus.h"

#ifdef AMINETXDUO_ALLOCCENSUS

#include <exec/execbase.h>
#include <proto/exec.h>

#include "aminetxduo/compat.h"

/* compat.h has just pointed both names at this file's own wrappers. */
#undef ami_alloc
#undef ami_alloc_flags

/*
 * Slots, a power of two so the probe start is a mask.  2048 x 16 bytes = 32 KB
 * of BSS in every image that links src/common, which in a census build is the
 * library and every command.  AMINETXDUO_ALLOCCENSUS_SLOTS moves it; the live
 * count on a working stack is in the low hundreds, so this has three binary
 * orders of headroom, and `lost=` in the report says when it did not.
 */
#ifndef AMINETXDUO_ALLOCCENSUS_SLOTS
#  define AMINETXDUO_ALLOCCENSUS_SLOTS 2048
#endif

#define CENSUS_SLOTS  AMINETXDUO_ALLOCCENSUS_SLOTS
#define CENSUS_MASK   (CENSUS_SLOTS - 1)

/* Sites printed, most bytes first, before the report gives up and says so. */
#define CENSUS_REPORT_SITES 48

typedef struct
{
    APTR        cs_Ptr;      /* NULL means free                            */
    ULONG       cs_Size;
    const char *cs_Site;     /* the literal from AMI_CENSUS_SITE           */
} CensusSlot;

static CensusSlot census_slot[CENSUS_SLOTS];

static ULONG census_live;        /* slots occupied right now                */
static ULONG census_live_peak;
static ULONG census_bytes;       /* bytes those slots account for           */
static ULONG census_lost;        /* allocations the table had no room for   */
static ULONG census_unknown;     /* frees of a pointer the table never had  */

/* --------------------------------------------------------------- the table */

/*
 * Pointer to slot.  AllocVec() returns 8-byte-aligned blocks, so the low three
 * bits are always zero and hashing them in would waste a quarter of the table;
 * shift them out first, then a Knuth multiply to spread the rest.
 */
static ULONG census_hash(APTR ptr)
{
    return (((ULONG)ptr >> 3) * 2654435761UL) & CENSUS_MASK;
}

static ULONG census_find(APTR ptr)
{
    ULONG i = census_hash(ptr);
    ULONG n;

    for (n = 0; n < (ULONG)CENSUS_SLOTS; n++)
    {
        if (census_slot[i].cs_Ptr == ptr)
            return i;
        if (census_slot[i].cs_Ptr == NULL)
            break;
        i = (i + 1) & CENSUS_MASK;
    }

    return (ULONG)CENSUS_SLOTS;   /* not present */
}

VOID ami_census_add(APTR ptr, ULONG size, const char *site)
{
    ULONG i;
    ULONG n;

    if (ptr == NULL)
        return;

    Forbid();

    i = census_hash(ptr);
    for (n = 0; n < (ULONG)CENSUS_SLOTS; n++)
    {
        if (census_slot[i].cs_Ptr == NULL)
        {
            census_slot[i].cs_Ptr  = ptr;
            census_slot[i].cs_Size = size;
            census_slot[i].cs_Site = site;

            census_live++;
            if (census_live > census_live_peak)
                census_live_peak = census_live;
            census_bytes += size;

            Permit();
            return;
        }
        i = (i + 1) & CENSUS_MASK;
    }

    census_lost++;
    Permit();
}

VOID ami_census_drop(APTR ptr)
{
    ULONG i;
    ULONG j;
    ULONG n;

    if (ptr == NULL)
        return;

    Forbid();

    i = census_find(ptr);
    if (i == (ULONG)CENSUS_SLOTS)
    {
        /* Either the table was full when this was allocated, or the block was
           never ours.  Both are worth a number; neither is worth a guess. */
        census_unknown++;
        Permit();
        return;
    }

    census_bytes -= census_slot[i].cs_Size;
    census_live--;
    census_slot[i].cs_Ptr  = NULL;
    census_slot[i].cs_Size = 0;
    census_slot[i].cs_Site = NULL;

    /*
     * Close the hole.  Everything after it in this probe run has to be lifted
     * back, or a lookup that walks to the first empty slot stops short of it.
     *
     * Bounded by the table size as well as by the first empty slot: on a table
     * with no empty slot at all the walk would have nothing to stop it, and an
     * instrument that can hang the machine it is instrumenting is worse than
     * no instrument.
     */
    j = (i + 1) & CENSUS_MASK;
    for (n = 0; n < (ULONG)CENSUS_SLOTS && census_slot[j].cs_Ptr != NULL; n++)
    {
        CensusSlot moved = census_slot[j];
        ULONG      k;
        ULONG      m;

        census_slot[j].cs_Ptr = NULL;

        k = census_hash(moved.cs_Ptr);
        for (m = 0; m < (ULONG)CENSUS_SLOTS; m++)
        {
            if (census_slot[k].cs_Ptr == NULL)
            {
                census_slot[k] = moved;
                break;
            }
            k = (k + 1) & CENSUS_MASK;
        }

        j = (j + 1) & CENSUS_MASK;
    }

    Permit();
}

/* ------------------------------------------------------- the tagged allocs */

APTR ami_alloc_flags_tagged(ULONG size, ULONG memf, const char *site)
{
    APTR p = ami_alloc_flags(size, memf);

    if (p != NULL)
        ami_census_add(p, size, site);

    return p;
}

APTR ami_alloc_tagged(ULONG size, const char *site)
{
    return ami_alloc_flags_tagged(size, MEMF_PUBLIC | MEMF_CLEAR, site);
}

/* ------------------------------------------------------------- the report */

/*
 * RawPutChar is an exec LVO (-516) the NDK declares only in its assembler
 * headers; src/common/compat.c wires it up the same way.  The serial port is
 * the one sink a shared library can reach without opening anything, which is
 * what makes one report format work for bsdsocket.library and for a Shell
 * command alike, into one log the harness reads.
 */
#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

static VOID census_puts(const char *s)
{
    if (s == NULL)
        s = "?";
    while (*s != '\0')
        RawPutChar((UBYTE)*s++);
}

static VOID census_putu(ULONG v)
{
    char  buf[12];
    UBYTE i = 0;

    if (v == 0)
    {
        RawPutChar('0');
        return;
    }
    while (v != 0 && i < (UBYTE)sizeof(buf))
    {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0)
        RawPutChar((UBYTE)buf[--i]);
}

/*
 * __FILE__ arrives as whatever path the compiler was given, which is the build
 * directory's idea of it and differs between one checkout and the next.  The
 * known-set file has to survive that, so only the last component is printed.
 * Every basename under src/ is unique, which is what makes this lossless here
 * and would stop being true the day two directories share a file name.
 */
static const char *census_basename(const char *site)
{
    const char *p;
    const char *base = site;

    if (site == NULL)
        return "?";

    /* '/' only.  The tag ends in ":123", so a rule that also broke at ':'
       would keep the line number and throw the file name away, which is what
       the first version of this did. */
    for (p = site; *p != '\0'; p++)
    {
        if (*p == '/')
            base = p + 1;
    }

    return base;
}

/* One line, so a reader never has to correlate two. */
static VOID census_line_site(const char *scope, const char *site,
                             ULONG bytes, ULONG count)
{
    census_puts("ALLOCCENSUS site=");
    census_puts(census_basename(site));
    census_puts(" bytes=");
    census_putu(bytes);
    census_puts(" count=");
    census_putu(count);
    census_puts(" scope=");
    census_puts(scope);
    RawPutChar('\n');
}

/*
 * The aggregate, as a file-scope static rather than on the stack.  This runs
 * inside bsdsocket.library's expunge, which can be reached from a Shell
 * command's own 4 KB stack (src/bsdsocket/library.c says so about
 * netstack_shutdown for the same reason), and 48 entries is 576 bytes.
 */
typedef struct
{
    const char *ca_Site;
    ULONG       ca_Bytes;
    ULONG       ca_Count;
} CensusAgg;

static CensusAgg census_agg[CENSUS_REPORT_SITES];
static BOOL      census_reporting;

VOID ami_census_report(const char *scope)
{
    ULONG used = 0;
    ULONG dropped_sites = 0;
    ULONG live, bytes, peak, lost, unknown;
    ULONG i;

    if (scope == NULL)
        scope = "?";

    Forbid();

    /* One report at a time: census_agg is shared and this is not reentrant. */
    if (census_reporting)
    {
        Permit();
        return;
    }
    census_reporting = TRUE;

    for (i = 0; i < (ULONG)CENSUS_SLOTS; i++)
    {
        const char *site;
        ULONG       k;
        BOOL        merged = FALSE;

        if (census_slot[i].cs_Ptr == NULL)
            continue;

        site = census_slot[i].cs_Site;

        for (k = 0; k < used; k++)
        {
            if (census_agg[k].ca_Site == site)
            {
                census_agg[k].ca_Bytes += census_slot[i].cs_Size;
                census_agg[k].ca_Count++;
                merged = TRUE;
                break;
            }
        }
        if (merged)
            continue;

        if (used == (ULONG)CENSUS_REPORT_SITES)
        {
            dropped_sites++;
            continue;
        }

        census_agg[used].ca_Site  = site;
        census_agg[used].ca_Bytes = census_slot[i].cs_Size;
        census_agg[used].ca_Count = 1;
        used++;
    }

    live    = census_live;
    bytes   = census_bytes;
    peak    = census_live_peak;
    lost    = census_lost;
    unknown = census_unknown;

    Permit();

    /* Selection sort, bytes descending: at most 48 entries, once, at teardown. */
    for (i = 0; i + 1 < used; i++)
    {
        ULONG best = i;
        ULONG k;

        for (k = i + 1; k < used; k++)
        {
            if (census_agg[k].ca_Bytes > census_agg[best].ca_Bytes)
                best = k;
        }
        if (best != i)
        {
            CensusAgg t     = census_agg[i];
            census_agg[i]   = census_agg[best];
            census_agg[best] = t;
        }
    }

    census_puts("ALLOCCENSUS begin scope=");
    census_puts(scope);
    RawPutChar('\n');

    for (i = 0; i < used; i++)
    {
        census_line_site(scope, census_agg[i].ca_Site,
                         census_agg[i].ca_Bytes, census_agg[i].ca_Count);
    }

    census_puts("ALLOCCENSUS end scope=");
    census_puts(scope);
    census_puts(" live=");
    census_putu(live);
    census_puts(" bytes=");
    census_putu(bytes);
    census_puts(" sites=");
    census_putu(used);
    census_puts(" peak=");
    census_putu(peak);
    census_puts(" lost=");
    census_putu(lost);
    census_puts(" unknown_free=");
    census_putu(unknown);
    census_puts(" sites_dropped=");
    census_putu(dropped_sites);
    RawPutChar('\n');

    Forbid();
    census_reporting = FALSE;
    Permit();
}

#else   /* !AMINETXDUO_ALLOCCENSUS */

/* ISO C has no empty translation unit. */
typedef int ami_alloccensus_not_built;

#endif  /* AMINETXDUO_ALLOCCENSUS */
