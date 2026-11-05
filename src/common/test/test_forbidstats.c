/*
 * AmiNetXDuo, host-side test for the Forbid()/Disable() census accounting
 * (src/common/forbidstats_core.c, AMINETXDUO_FORBIDSTATS).  The clock and the
 * task are arguments there, so this drives the shipped file with made-up
 * ones.  Output is key=value; exit 0 when every check holds.
 *
 *   cc -std=c99 -Wall -Wextra -I../../../include -I../../config/test/shim \
 *      test_forbidstats.c ../forbidstats_core.c -o test_forbidstats
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/forbidstats.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(name, cond)                                               \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("fail=%s line=%d\n", name, __LINE__);                \
            failures++;                                                 \
        }                                                               \
    } while (0)

static AmiFsState st;

static const AmiFsSite *find(const char *site, ULONG kind)
{
    ULONG i;

    for (i = 0; i < AMI_FS_SITES; i++)
        if (st.st_Site[i].s_Site == site && st.st_Site[i].s_Kind == kind)
            return &st.st_Site[i];
    return NULL;
}

int main(void)
{
    /* 1 MHz, 2 ticks of inner overhead: a tick is a microsecond. */
    static const char a[] = "a.c:1";
    static const char b[] = "b.c:2";
    static char       many[700][8];
    int               x = 0, y = 0;
    APTR              t1 = &x, t2 = &y;
    const char       *s;
    const AmiFsSite  *e;
    ULONG             i;

    ami_fs_core_init(&st, 1000000UL, 2UL * 16UL);
    CHECK("lim10us",  st.st_Lim16[0] == 160);
    CHECK("lim100us", st.st_Lim16[1] == 1600);
    CHECK("lim1ms",   st.st_Lim16[2] == 16000);

    /* One section per bucket, net of the 2-tick overhead. */
    ami_fs_core_open(&st, AMI_FS_FORBID, t1, 100, 0, a);
    ami_fs_core_close(&st, AMI_FS_FORBID, t1, 105, 0);         /* 3 us    */
    ami_fs_core_open(&st, AMI_FS_FORBID, t1, 200, 0, a);
    ami_fs_core_close(&st, AMI_FS_FORBID, t1, 252, 0);         /* 50 us   */
    ami_fs_core_open(&st, AMI_FS_FORBID, t1, 1000, 0, b);
    ami_fs_core_close(&st, AMI_FS_FORBID, t1, 1502, 0);        /* 500 us  */
    ami_fs_core_open(&st, AMI_FS_FORBID, t1, 5000, 0, b);
    ami_fs_core_close(&st, AMI_FS_FORBID, t1, 7002, 0);        /* 2000 us */

    CHECK("sections", st.st_Kind[0].k_Sections == 4);
    CHECK("held_raw", st.st_Kind[0].k_HeldLo == 5 + 52 + 502 + 2002);
    CHECK("hist0", st.st_Kind[0].k_Hist[0] == 1);
    CHECK("hist1", st.st_Kind[0].k_Hist[1] == 1);
    CHECK("hist2", st.st_Kind[0].k_Hist[2] == 1);
    CHECK("hist3", st.st_Kind[0].k_Hist[3] == 1);
    CHECK("max", st.st_Kind[0].k_Max16 == 2000 * 16);
    CHECK("max_site", st.st_Kind[0].k_MaxSite == b);
    e = find(a, AMI_FS_FORBID);
    CHECK("site_a", e != NULL && e->s_Count == 2 && e->s_Held == 57 &&
                    e->s_Max16 == 50 * 16);
    e = find(b, AMI_FS_FORBID);
    CHECK("site_b", e != NULL && e->s_Count == 2 && e->s_Held == 2504);

    /* Shorter than the overhead: clamps to zero, never wraps. */
    ami_fs_core_open(&st, AMI_FS_FORBID, t1, 9000, 0, a);
    ami_fs_core_close(&st, AMI_FS_FORBID, t1, 9001, 0);
    CHECK("clamp", st.st_Kind[0].k_Hist[0] == 2);

    /* The clock wraps inside a section. */
    ami_fs_core_open(&st, AMI_FS_DISABLE, t1, 0xFFFFFFF0UL, 0, a);
    ami_fs_core_close(&st, AMI_FS_DISABLE, t1, 0x10UL, 0);
    CHECK("wrap", st.st_Kind[1].k_HeldLo == 0x20 &&
                  st.st_Kind[1].k_Max16 == (0x20 - 2) * 16);
    CHECK("kinds_apart", find(a, AMI_FS_DISABLE) != NULL &&
                         find(a, AMI_FS_FORBID)->s_Count == 3);

    /* A Wait() ends the section; the return opens a new one at its site. */
    ami_fs_core_open(&st, AMI_FS_FORBID, t1, 20000, 0, b);
    s = ami_fs_core_sleep(&st, AMI_FS_FORBID, t1, 20012, 0);
    CHECK("sleep_site", s == b);
    CHECK("sleep_count", st.st_Kind[0].k_Sleeps == 1 &&
                         st.st_Kind[0].k_Sections == 6);
    /* Someone else runs meanwhile, which is legal: nothing is open. */
    ami_fs_core_open(&st, AMI_FS_FORBID, t2, 20100, 0, a);
    ami_fs_core_close(&st, AMI_FS_FORBID, t2, 20110, 0);
    CHECK("no_lost", st.st_Kind[0].k_Lost == 0);
    ami_fs_core_open(&st, AMI_FS_FORBID, t1, 30000, 0, s);
    ami_fs_core_close(&st, AMI_FS_FORBID, t1, 30030, 0);
    CHECK("resumed", find(b, AMI_FS_FORBID)->s_Count == 4);
    CHECK("sleep_other", ami_fs_core_sleep(&st, AMI_FS_FORBID, t2, 1, 0) == NULL);

    /* A sleep we did not see: another task's enter finds the section open,
       and the sleeper's own exit then has nothing to close. */
    ami_fs_core_open(&st, AMI_FS_FORBID, t1, 40000, 0, a);
    ami_fs_core_open(&st, AMI_FS_FORBID, t2, 40500, 0, b);
    CHECK("lost", st.st_Kind[0].k_Lost == 1);
    ami_fs_core_close(&st, AMI_FS_FORBID, t2, 40510, 0);
    ami_fs_core_close(&st, AMI_FS_FORBID, t1, 41000, 0);
    CHECK("unmatched", st.st_Kind[0].k_Unmatched == 1);
    CHECK("lost_not_timed", st.st_Kind[0].k_Max16 == 2000 * 16);

    /* Raised by someone we did not see, dropped by us. */
    ami_fs_core_close(&st, AMI_FS_FORBID, t1, 50000, 0);
    CHECK("unmatched2", st.st_Kind[0].k_Unmatched == 2);

    /* The read resets the max fields and nothing else. */
    ami_fs_core_reset_max(&st);
    CHECK("reset", st.st_Kind[0].k_Max16 == 0 &&
                   st.st_Kind[0].k_MaxSite == NULL &&
                   find(b, AMI_FS_FORBID)->s_Max16 == 0 &&
                   st.st_Kind[0].k_Sections == 9);

    /* Exec dispatched inside the section: slept where the shim cannot see.
       Counted apart, kept out of the held time, the histogram and the max. */
    ami_fs_core_open(&st, AMI_FS_FORBID, t1, 60000, 7, a);
    ami_fs_core_close(&st, AMI_FS_FORBID, t1, 90000, 8);
    CHECK("broken", st.st_Kind[0].k_Broken == 1 &&
                    st.st_Kind[0].k_BrokenHeld == 30000 &&
                    st.st_Kind[0].k_Sections == 9 &&
                    st.st_Kind[0].k_Max16 == 0 &&
                    find(a, AMI_FS_FORBID)->s_Broken == 1);

    /* 32-bit held total carries. */
    st.st_Kind[0].k_HeldLo = 0xFFFFFFF0UL;
    ami_fs_core_open(&st, AMI_FS_FORBID, t1, 0, 0, a);
    ami_fs_core_close(&st, AMI_FS_FORBID, t1, 0x20, 0);
    CHECK("carry", st.st_Kind[0].k_HeldHi == 1 &&
                   st.st_Kind[0].k_HeldLo == 0x10);

    /* More sites than slots: the table fills and counts the rest. */
    ami_fs_core_init(&st, 709379UL, 0);
    for (i = 0; i < 700; i++)
    {
        snprintf(many[i], sizeof(many[i]), "s%lu", (unsigned long)i);
        ami_fs_core_open(&st, AMI_FS_FORBID, t1, i * 10, 0, many[i]);
        ami_fs_core_close(&st, AMI_FS_FORBID, t1, i * 10 + 3, 0);
    }
    CHECK("overflow", st.st_SiteOverflow == 700 - AMI_FS_SITES);
    CHECK("overflow_still_counted", st.st_Kind[0].k_Sections == 700);
    CHECK("pal_lim", st.st_Lim16[2] == 709379UL * 16UL / 1000UL);

    printf("forbidstats_core=%s failures=%d\n", failures ? "fail" : "ok",
           failures);
    return failures ? 1 : 0;
}
