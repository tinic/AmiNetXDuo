/*
 * Forbid()/Disable() section accounting, AMINETXDUO_FORBIDSTATS only.  No
 * AmigaOS calls: the clock and the task arrive as arguments, so the host
 * test runs this file as built.  See aminetxduo/forbidstats.h.
 * SPDX-License-Identifier: MIT
 */
#include "aminetxduo/forbidstats.h"

VOID ami_fs_core_init(AmiFsState *st, ULONG hz, ULONG ovh16)
{
    UBYTE *p = (UBYTE *)st;
    ULONG  i;

    for (i = 0; i < sizeof(*st); i++)
        p[i] = 0;

    st->st_Hz    = hz;
    st->st_Ovh16 = ovh16;
    /* 10 us, 100 us, 1 ms; hz * 16 / 1000 stays inside 32 bits. */
    st->st_Lim16[0] = (hz * 16UL / 1000UL) / 100UL;
    st->st_Lim16[1] = (hz * 16UL / 1000UL) / 10UL;
    st->st_Lim16[2] =  hz * 16UL / 1000UL;
}

static AmiFsSite *fs_site(AmiFsState *st, ULONG kind, const char *site)
{
    ULONG p = (ULONG)(unsigned long)site >> 2;
    ULONG h = (ULONG)(p * (ULONG)2654435761UL) >> 23;
    ULONG n;

    for (n = 0; n < AMI_FS_SITES; n++)
    {
        AmiFsSite *s = &st->st_Site[(h + n) & (AMI_FS_SITES - 1)];

        if (s->s_Site == site && s->s_Kind == kind)
            return s;
        if (s->s_Site == NULL)
        {
            s->s_Site = site;
            s->s_Kind = kind;
            return s;
        }
    }

    st->st_SiteOverflow++;
    return NULL;
}

static VOID fs_account(AmiFsState *st, AmiFsKind *k, ULONG kind, ULONG now,
                       ULONG disp)
{
    ULONG      d   = now - k->k_T0;            /* modulo 2^32 */
    ULONG      d16 = (d < 0x08000000UL) ? d * 16UL : 0xFFFFFFFFUL;
    ULONG      net = (d16 > st->st_Ovh16) ? d16 - st->st_Ovh16 : 0;
    ULONG      b;
    AmiFsSite *s;

    k->k_Open = 0;
    if (disp != k->k_Disp)
    {
        k->k_Broken++;
        k->k_BrokenHeld += d;
        s = fs_site(st, kind, k->k_Site);
        if (s != NULL)
            s->s_Broken++;
        return;
    }
    k->k_Sections++;
    k->k_HeldLo += d;
    if (k->k_HeldLo < d)
        k->k_HeldHi++;

    for (b = 0; b < AMI_FS_BUCKETS - 1; b++)
        if (net < st->st_Lim16[b])
            break;
    k->k_Hist[b]++;

    if (net > k->k_Max16)
    {
        k->k_Max16   = net;
        k->k_MaxSite = k->k_Site;
    }

    s = fs_site(st, kind, k->k_Site);
    if (s != NULL)
    {
        s->s_Count++;
        s->s_Held += d;
        if (net > s->s_Max16)
            s->s_Max16 = net;
    }
}

VOID ami_fs_core_open(AmiFsState *st, ULONG kind, APTR task, ULONG now,
                      ULONG disp, const char *site)
{
    AmiFsKind *k = &st->st_Kind[kind];

    /* Scheduling is off while one is open, so another task can only get here
       if the holder slept in a Wait() the shim did not see. */
    if (k->k_Open)
        k->k_Lost++;

    k->k_Open   = 1;
    k->k_Holder = task;
    k->k_T0     = now;
    k->k_Disp   = disp;
    k->k_Site   = site;
}

VOID ami_fs_core_close(AmiFsState *st, ULONG kind, APTR task, ULONG now,
                       ULONG disp)
{
    AmiFsKind *k = &st->st_Kind[kind];

    if (k->k_Open && k->k_Holder == task)
    {
        fs_account(st, k, kind, now, disp);
        return;
    }

    /* Raised by someone we did not see (Exec's OpenLibrary(), a caller's
       own Forbid()), or the far side of a sleep we did not see. */
    k->k_Unmatched++;
    if (k->k_Open)
    {
        k->k_Lost++;
        k->k_Open = 0;
    }
}

const char *ami_fs_core_sleep(AmiFsState *st, ULONG kind, APTR task,
                              ULONG now, ULONG disp)
{
    AmiFsKind  *k = &st->st_Kind[kind];
    const char *site;

    if (!k->k_Open || k->k_Holder != task)
        return NULL;

    site = k->k_Site;
    fs_account(st, k, kind, now, disp);
    k->k_Sleeps++;
    return site;
}

VOID ami_fs_core_reset_max(AmiFsState *st)
{
    ULONG i;

    for (i = 0; i < AMI_FS_KINDS; i++)
    {
        st->st_Kind[i].k_Max16   = 0;
        st->st_Kind[i].k_MaxSite = NULL;
    }
    for (i = 0; i < AMI_FS_SITES; i++)
        st->st_Site[i].s_Max16 = 0;
}
