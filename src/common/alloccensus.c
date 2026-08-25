/*
 * AmiNetXDuo, the allocation census.
 *
 * A side table, not a header in front of the block, so the census build keeps
 * the memory layout of the build under investigation.  Open-addressed on the
 * pointer, linear probing, no tombstones: a drop re-inserts the rest of its
 * probe run, and a full table refuses to record rather than answering wrongly.
 *
 * Compiled only when AMINETXDUO_ALLOCCENSUS is defined.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/alloccensus.h"

#ifdef AMINETXDUO_ALLOCCENSUS

#include <exec/execbase.h>
#include <proto/exec.h>
#include <inline/macros.h>

#include "aminetxduo/compat.h"

/* compat.h points both names at the wrappers in this file. */
#undef ami_alloc
#undef ami_alloc_flags

/* Slots, a POWER OF TWO so the probe start is a mask.
   AMINETXDUO_ALLOCCENSUS_SLOTS moves it; `lost=` reports an overflow. */
#ifndef AMINETXDUO_ALLOCCENSUS_SLOTS
#  define AMINETXDUO_ALLOCCENSUS_SLOTS 2048
#endif

#define CENSUS_SLOTS  AMINETXDUO_ALLOCCENSUS_SLOTS
#define CENSUS_MASK   (CENSUS_SLOTS - 1)

/* Sites printed, most bytes first, before the report stops and says so. */
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

/* AllocVec() blocks are 8-byte aligned, so the low three bits carry nothing:
   shift them out before the Knuth multiply. */
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
        /* Either the table was full at the allocation, or the block came from
           elsewhere. Both are counted, and neither is guessed at. */
        census_unknown++;
        Permit();
        return;
    }

    census_bytes -= census_slot[i].cs_Size;
    census_live--;
    census_slot[i].cs_Ptr  = NULL;
    census_slot[i].cs_Size = 0;
    census_slot[i].cs_Site = NULL;

    /* Close the hole: everything after it in the probe run must move back.
       Bounded by the table size too -- a table with no empty slot at all would
       otherwise hang the walk. */
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

/* RawPutChar is exec LVO -516, declared only in the NDK assembler headers.
   The serial port is the one sink a shared library reaches with no open. */
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

/* Only the last component of __FILE__ is printed, so the known-set file
   survives a different build directory.  Assumes every basename under src/ is
   unique. */
static const char *census_basename(const char *site)
{
    const char *p;
    const char *base = site;

    if (site == NULL)
        return "?";

    /* '/' only: the tag ends in ":123", so breaking at ':' too would keep the
       line number and lose the file name. */
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

/* File-scope static, NOT on the stack: this runs inside the expunge of
   bsdsocket.library, reachable from a 4 KB Shell stack. */
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
