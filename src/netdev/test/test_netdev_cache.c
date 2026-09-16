/*
 * anxnet.device: the 68030 data-cache guard for a Zorro III board, off the
 * machine.
 *
 * netdev_cache.c has six seams -- the CPU, the two transparent-translation
 * registers, the cache flush, the data-cache switch and Exec's memory list --
 * and one question it asks the core.  All are supplied here as recorders, so
 * what the ladder does at each rung is asserted step by step: which register
 * it took, what it wrote, that a register somebody else holds is left alone,
 * that a register that did not help is put back, that RAM in the block
 * sends it to the cache switch, and that release restores exactly what it
 * changed.  The transparent-translation value is checked against the
 * MC68030 User's Manual's field layout for a small board, a board that
 * spans two 16 MB blocks, and one at the top of memory.
 *
 * netdev_cache.c is #included whole under NETDEV_CACHE_TEST, which drops the
 * device's supervisor stubs and takes the seams from this file.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#define NETDEV_CACHE_TEST 1
#include "netdev_cache.c"

static unsigned long t_checks;
static unsigned long t_failures;

static void t_check(int ok, const char *what)
{
    t_checks++;
    if (!ok)
    {
        t_failures++;
        printf("FAIL %s\n", what);
    }
}

/* ------------------------------------------------------------ the seams -- */

static BOOL  m_is_030 = TRUE;
static ULONG m_tt[2];               /* the registers, as the CPU holds them */
static ULONG m_cacr = 0x00000100UL; /* CACR: data cache on                  */
static BOOL  m_ram_in;              /* RAM shares the block                 */
static int   m_flushes;
static int   m_tt_writes;
static UWORD m_last_which;
static ULONG m_last_value;
static int   m_dcache_calls;
static BOOL  m_dcache_last;

/* What the core answers, in order; the last entry repeats. */
static BOOL  m_answers[8];
static int   m_answer_count;
static int   m_asked;

BOOL nd_cache_is_030(VOID)
{
    return m_is_030;
}

VOID nd_cache_tt_read(ULONG *tt)
{
    tt[0] = m_tt[0];
    tt[1] = m_tt[1];
}

VOID nd_cache_tt_write(UWORD which, ULONG v)
{
    m_tt[which]  = v;
    m_last_which = which;
    m_last_value = v;
    m_tt_writes++;
}

VOID nd_cache_flush(VOID)
{
    m_flushes++;
}

ULONG nd_cache_dcache(BOOL on)
{
    ULONG old = m_cacr;

    m_cacr = on ? (m_cacr | 0x100UL) : (m_cacr & ~0x100UL);
    m_dcache_calls++;
    m_dcache_last = on;

    return old;
}

BOOL nd_cache_ram_in(ULONG lo, ULONG hi)
{
    (void)lo;
    (void)hi;
    return m_ram_in;
}

static BOOL m_coherent(NetdevNic *nic)
{
    int i = (m_asked < m_answer_count) ? m_asked : m_answer_count - 1;

    (void)nic;
    m_asked++;

    return m_answers[i];
}

static const struct NetdevNicOps m_ops_asking =
{
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, m_coherent
};
static const struct NetdevNicOps m_ops_mute =
{
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static NetdevNic m_nic;

static void m_reset(const BOOL *answers, int n)
{
    int i;

    memset(&m_nic, 0, sizeof(m_nic));
    m_nic.ops = &m_ops_asking;
    m_is_030  = TRUE;
    m_tt[0]   = 0;
    m_tt[1]   = 0;
    m_cacr    = 0x100UL;
    m_ram_in  = FALSE;
    m_flushes = 0;
    m_tt_writes = 0;
    m_dcache_calls = 0;
    m_asked   = 0;
    for (i = 0; i < n; i++)
        m_answers[i] = answers[i];
    m_answer_count = n;
}

#define BOARD   0x40000000UL
#define SIZE    0x00010000UL

/* ------------------------------------------------------------- the value -- */

static void a_the_value(void)
{
    ULONG lo, hi, v;

    v = netdev_cache_tt_value(BOARD, SIZE, &lo, &hi);
    t_check(v == 0x40008507UL,
            "a 64 KB board at $40000000: base $40, mask 0, E, CI, RWM, FC 7");
    t_check(lo == 0x40000000UL && hi == 0x41000000UL,
            "its block is $40000000-$40ffffff");

    v = netdev_cache_tt_value(0x42000000UL, 0x02000000UL, &lo, &hi);
    t_check(v == 0x42018507UL,
            "a 32 MB board at $42000000 spans two blocks: base $42, mask 1");
    t_check(lo == 0x42000000UL && hi == 0x44000000UL,
            "and covers $42000000-$43ffffff");

    v = netdev_cache_tt_value(0x41000000UL, 0x02000000UL, &lo, &hi);
    t_check(v == 0x40038507UL && lo == 0x40000000UL && hi == 0x44000000UL,
            "the same board misaligned takes four blocks: base $40, mask 3");

    v = netdev_cache_tt_value(0xff000000UL, 0x01000000UL, &lo, &hi);
    t_check(v == 0xff008507UL && lo == 0xff000000UL && hi == 0UL,
            "the top block's end wraps to 0");

    v = netdev_cache_tt_value(0x4000f000UL, 0x00002000UL, &lo, &hi);
    t_check(v == 0x40008507UL, "8 KB at $4000f000 stays in one block");

    printf("  value              $%08lx for $%08lx+$%lx\n",
           (unsigned long)netdev_cache_tt_value(BOARD, SIZE, &lo, &hi),
           (unsigned long)BOARD, (unsigned long)SIZE);
}

/* ------------------------------------------------------------ the ladder -- */

static void b_coherent_as_found(void)
{
    static const BOOL yes[] = { TRUE };
    UBYTE r;

    m_reset(yes, 1);
    r = netdev_cache_guard(&m_nic, BOARD, SIZE);
    t_check(r == NETDEV_CACHE_NONE && m_nic.cache_guard == NETDEV_CACHE_NONE,
            "a coherent board is left alone");
    t_check(m_tt_writes == 0 && m_dcache_calls == 0 && m_asked == 1,
            "and nothing was touched: one probe, no writes");
}

static void c_not_a_030(void)
{
    static const BOOL no[] = { FALSE };
    UBYTE r;

    m_reset(no, 1);
    m_is_030 = FALSE;
    r = netdev_cache_guard(&m_nic, BOARD, SIZE);
    t_check(r == NETDEV_CACHE_NONE && m_asked == 0,
            "a 68040 is not even probed");

    m_reset(no, 1);
    m_nic.ops = &m_ops_mute;
    r = netdev_cache_guard(&m_nic, BOARD, SIZE);
    t_check(r == NETDEV_CACHE_NONE && m_tt_writes == 0,
            "a core that cannot be asked is not guarded");
}

static void d_tt0_takes_it(void)
{
    static const BOOL then_yes[] = { FALSE, TRUE };
    UBYTE r;

    m_reset(then_yes, 2);
    r = netdev_cache_guard(&m_nic, BOARD, SIZE);
    t_check(r == NETDEV_CACHE_TT0, "TT0 free: TT0 marks the block");
    t_check(m_tt_writes == 1 && m_last_which == 0 &&
            m_last_value == 0x40008507UL,
            "one write, to TT0, of the block's value");
    t_check(m_flushes == 1, "the cache was flushed after it");
    t_check(m_asked == 2, "and the board was probed again to prove it");
    t_check(m_dcache_calls == 0, "the data cache was not touched");
    t_check(m_nic.cache_saved == 0UL, "what TT0 held is kept for release");

    netdev_cache_release(&m_nic);
    t_check(m_tt_writes == 2 && m_last_which == 0 && m_last_value == 0UL,
            "release puts TT0 back");
    t_check(m_nic.cache_guard == NETDEV_CACHE_NONE, "and forgets the guard");
}

static void e_tt0_is_taken(void)
{
    static const BOOL then_yes[] = { FALSE, TRUE };
    UBYTE r;

    m_reset(then_yes, 2);
    m_tt[0] = 0x0000ff00UL | 0x8000UL;      /* somebody's, enabled */
    r = netdev_cache_guard(&m_nic, BOARD, SIZE);
    t_check(r == NETDEV_CACHE_TT1, "TT0 in use: TT1 marks the block");
    t_check(m_tt_writes == 1 && m_last_which == 1,
            "TT0 was not written");
    t_check(m_tt[0] == (0x0000ff00UL | 0x8000UL), "and still holds its owner's value");

    netdev_cache_release(&m_nic);
    t_check(m_tt[1] == 0UL && m_tt[0] == (0x0000ff00UL | 0x8000UL),
            "release restores TT1 only");
}

static void f_both_taken(void)
{
    static const BOOL then_yes[] = { FALSE, TRUE };
    UBYTE r;

    m_reset(then_yes, 2);
    m_tt[0] = 0x8000UL;
    m_tt[1] = 0x8000UL;
    r = netdev_cache_guard(&m_nic, BOARD, SIZE);
    t_check(r == NETDEV_CACHE_DCACHE, "both in use: the data cache goes off");
    t_check(m_tt_writes == 0, "neither register was written");
    t_check(m_dcache_calls == 1 && m_dcache_last == FALSE &&
            (m_cacr & 0x100UL) == 0UL,
            "CacheControl(0, CACRF_EnableD)");
    t_check(m_nic.cache_saved == 0x100UL, "the old CACR is kept");

    netdev_cache_release(&m_nic);
    t_check(m_dcache_calls == 2 && m_dcache_last == TRUE &&
            (m_cacr & 0x100UL) != 0UL,
            "release turns the data cache back on");
}

static void g_ram_in_the_block(void)
{
    static const BOOL then_yes[] = { FALSE, TRUE };
    UBYTE r;

    m_reset(then_yes, 2);
    m_ram_in = TRUE;
    r = netdev_cache_guard(&m_nic, BOARD, SIZE);
    t_check(r == NETDEV_CACHE_DCACHE && m_tt_writes == 0,
            "RAM in the block: no transparent translation, the cache goes off");
}

static void h_tt_did_not_help(void)
{
    static const BOOL no_no_yes[] = { FALSE, FALSE, TRUE };
    UBYTE r;

    m_reset(no_no_yes, 3);
    m_tt[1] = 0x8000UL;                     /* only TT0 is free */
    r = netdev_cache_guard(&m_nic, BOARD, SIZE);
    t_check(r == NETDEV_CACHE_DCACHE,
            "TT0 set and the board still stale: the cache goes off");
    t_check(m_tt[0] == 0UL && m_tt_writes == 2,
            "TT0 was written and then put back");
    t_check(m_flushes == 2, "flushed after each of the two writes");
    t_check(m_asked == 3, "probed as found, after TT0, after the switch");
}

static void i_nothing_helps(void)
{
    static const BOOL no[] = { FALSE };
    UBYTE r;

    m_reset(no, 1);
    r = netdev_cache_guard(&m_nic, BOARD, SIZE);
    t_check(r == NETDEV_CACHE_FAILED, "still stale with the cache off: FAILED");
    t_check(m_tt[0] == 0UL && m_tt[1] == 0UL, "both registers back to free");
    t_check((m_cacr & 0x100UL) != 0UL && m_dcache_calls == 2,
            "the data cache is back on before attach reports");
    t_check(m_asked == 4, "probed as found, after TT0, after TT1, after the switch");

    netdev_cache_release(&m_nic);
    t_check(m_dcache_calls == 2 && m_tt_writes == 4,
            "release has nothing left to undo");
}

static void j_cache_was_already_off(void)
{
    static const BOOL then_yes[] = { FALSE, TRUE };
    UBYTE r;

    m_reset(then_yes, 2);
    m_tt[0] = 0x8000UL;
    m_tt[1] = 0x8000UL;
    m_cacr  = 0UL;                          /* somebody ran with it off */
    r = netdev_cache_guard(&m_nic, BOARD, SIZE);
    t_check(r == NETDEV_CACHE_DCACHE, "the switch is still the rung it took");

    netdev_cache_release(&m_nic);
    t_check(m_dcache_calls == 1 && (m_cacr & 0x100UL) == 0UL,
            "release does not turn on a cache that was off before");
}

int main(void)
{
    printf("netdev_cache: a 68030's data cache kept off a Zorro III board\n");

    a_the_value();
    b_coherent_as_found();
    c_not_a_030();
    d_tt0_takes_it();
    e_tt0_is_taken();
    f_both_taken();
    g_ram_in_the_block();
    h_tt_did_not_help();
    i_nothing_helps();
    j_cache_was_already_off();

    printf("%lu checks, %lu failures, %s\n", t_checks, t_failures,
           (t_failures == 0UL) ? "PASS" : "FAIL");

    return (t_failures == 0UL) ? 0 : 1;
}
