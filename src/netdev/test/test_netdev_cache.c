/*
 * anxnet.device: the 68030 data-cache guard for a Zorro III board, off the
 * machine.
 *
 * netdev_cache.c has seven seams -- the CPU, the two transparent-translation
 * operations, the cache flush, data-cache switch and query, and Exec's memory list --
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
static int   m_cacr_reads;

/* What the core answers, in order; the last entry repeats. */
static BOOL  m_answers[8];
static int   m_answer_count;
static int   m_asked;
static int   m_mutate_on_ask = -1;
static UWORD m_mutate_which;
static ULONG m_mutate_value;

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

ULONG nd_cache_cacr_read(VOID)
{
    m_cacr_reads++;
    return m_cacr;
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
    if (m_asked == m_mutate_on_ask)
        m_tt[m_mutate_which] = m_mutate_value;
    m_asked++;

    return m_answers[i];
}

static const struct NetdevNicOps m_ops_asking =
{
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, m_coherent, NULL
};
static const struct NetdevNicOps m_ops_mute =
{
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
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
    m_cacr_reads = 0;
    m_asked   = 0;
    m_mutate_on_ask = -1;
    memset(nd_tt_lease, 0, sizeof(nd_tt_lease));
    nd_dc_saved = 0;
    nd_dc_refs = 0;
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
    static const BOOL yes[] = { TRUE, TRUE };
    UBYTE r;

    m_reset(yes, 2);
    r = netdev_cache_guard(&m_nic, BOARD, SIZE);
    t_check(r == NETDEV_CACHE_TT0 && m_nic.cache_mode == NETDEV_CACHE_TT0,
            "coherent-as-found still gets a private conservative guard");
    t_check(m_tt_writes == 1 && m_dcache_calls == 0 && m_asked == 2,
            "attach proves the board, then proves its own TT protection");
    netdev_cache_release(&m_nic);
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
    t_check(nd_tt_lease[0].saved == 0UL && nd_tt_lease[0].refs == 1,
            "the CPU-global TT0 lease keeps its old value and one owner");

    netdev_cache_release(&m_nic);
    t_check(m_tt_writes == 2 && m_last_which == 0 && m_last_value == 0UL,
            "release puts TT0 back");
    t_check(m_nic.cache_guard == NETDEV_CACHE_NONE, "and forgets the guard");
    t_check(m_nic.cache_mode == NETDEV_CACHE_TT0,
            "release retains the strategy for first Open");
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
    t_check(nd_dc_saved == 0x100UL && nd_dc_refs == 1,
            "the CPU-global CACR lease keeps its old value and one owner");

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
    t_check(nd_tt_lease[0].refs == 0 && nd_tt_lease[1].refs == 0 &&
            nd_dc_refs == 0,
            "an attach failure leaks no CPU-global lease");

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
    t_check(m_dcache_calls == 1 && m_cacr_reads == 1 &&
            (m_cacr & CACR_ED) == 0,
            "an unchanged originally-off cache stays off without a write");

    m_reset(then_yes, 2);
    m_tt[0] = m_tt[1] = TT_E;
    m_cacr = 0;
    r = netdev_cache_guard(&m_nic, BOARD, SIZE);
    m_cacr |= CACR_ED;                      /* cache utility changed it */
    netdev_cache_release(&m_nic);
    t_check(r == NETDEV_CACHE_DCACHE && m_dcache_calls == 1 &&
            m_cacr_reads == 1 && (m_cacr & CACR_ED) != 0,
            "release preserves an external enable instead of clobbering it");
}

/* ---------------------------------------------------------- the lifetime -- */

static void k_attach_open_close_lifecycle(void)
{
    static const BOOL answers[] = { FALSE, TRUE, TRUE };

    m_reset(answers, 4);
    t_check(netdev_cache_guard(&m_nic, BOARD, SIZE) == NETDEV_CACHE_TT0,
            "attach acquires TT0");
    t_check(m_nic.cache_board == BOARD && m_nic.cache_board_size == SIZE,
            "attach records the board address and size");
    netdev_cache_release(&m_nic);
    t_check(m_tt[0] == 0 && nd_tt_lease[0].refs == 0,
            "successful attach releases the CPU lease");

    t_check(netdev_cache_acquire(&m_nic) == NETDEV_CACHE_TT0,
            "first Open reacquires a guard before board access");
    t_check(m_tt[0] == 0x40008507UL && nd_tt_lease[0].refs == 1,
            "the recorded board is protected again");
    t_check(m_asked == 3,
            "reacquire probes once, after TT0 is installed, never unguarded");
    netdev_cache_release(&m_nic);
    netdev_cache_release(&m_nic);
    t_check(m_tt[0] == 0 && nd_tt_lease[0].refs == 0,
            "last Close releases once and a retry is idempotent");
}

static void l_two_units_share_tt(void)
{
    static const BOOL answers[] = { FALSE, TRUE, TRUE };
    NetdevNic a, b;

    m_reset(answers, 3);
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.ops = b.ops = &m_ops_asking;

    t_check(netdev_cache_guard(&a, BOARD, SIZE) == NETDEV_CACHE_TT0,
            "unit A owns TT0");
    t_check(netdev_cache_guard(&b, BOARD + 0x00100000UL, SIZE) ==
                NETDEV_CACHE_TT0,
            "a second unit in the covered block shares TT0");
    t_check(nd_tt_lease[0].refs == 2 && m_tt_writes == 1,
            "sharing increments the lease without rewriting the CPU");
    netdev_cache_release(&a);
    t_check(m_tt[0] == 0x40008507UL && nd_tt_lease[0].refs == 1,
            "closing A cannot restore TT0 under B");
    netdev_cache_release(&b);
    t_check(m_tt[0] == 0 && nd_tt_lease[0].refs == 0,
            "the last TT user restores the original value");
}

static void m_nonoverlap_uses_other_tt(void)
{
    static const BOOL answers[] = { FALSE, TRUE, FALSE, TRUE };
    NetdevNic a, b;

    m_reset(answers, 4);
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.ops = b.ops = &m_ops_asking;

    t_check(netdev_cache_guard(&a, BOARD, SIZE) == NETDEV_CACHE_TT0,
            "the first non-overlap test unit owns TT0");
    t_check(netdev_cache_guard(&b, 0x50000000UL, SIZE) == NETDEV_CACHE_TT1,
            "a unit outside TT0's block uses TT1");
    t_check(nd_tt_lease[0].refs == 1 && nd_tt_lease[1].refs == 1,
            "the independent TT leases coexist");
    netdev_cache_release(&b);
    netdev_cache_release(&a);
    t_check(m_tt[0] == 0 && m_tt[1] == 0,
            "either release order restores both registers");
}

static void n_two_units_share_dcache(void)
{
    static const BOOL answers[] = { FALSE, TRUE, TRUE };
    NetdevNic a, b;

    m_reset(answers, 3);
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.ops = b.ops = &m_ops_asking;
    m_tt[0] = m_tt[1] = TT_E;

    t_check(netdev_cache_guard(&a, BOARD, SIZE) == NETDEV_CACHE_DCACHE,
            "unit A owns the global data-cache lease");
    t_check(netdev_cache_guard(&b, 0x50000000UL, SIZE) ==
                NETDEV_CACHE_DCACHE,
            "unit B joins that lease before its first coherence probe");
    t_check(nd_dc_refs == 2 && m_dcache_calls == 1,
            "the second unit does not toggle CACR again");
    netdev_cache_release(&a);
    t_check(nd_dc_refs == 1 && (m_cacr & CACR_ED) == 0,
            "closing A leaves the cache off for B");
    netdev_cache_release(&b);
    t_check(nd_dc_refs == 0 && (m_cacr & CACR_ED) != 0,
            "the final global user restores CACR");
}

static void o_external_tt_changes_are_not_clobbered(void)
{
    static const BOOL then_yes[] = { FALSE, TRUE };
    static const BOOL never[] = { FALSE, FALSE, FALSE, FALSE };
    const ULONG external = 0x7f008507UL;

    m_reset(then_yes, 2);
    t_check(netdev_cache_guard(&m_nic, BOARD, SIZE) == NETDEV_CACHE_TT0,
            "external-change test owns TT0");
    m_tt[0] = external;
    netdev_cache_release(&m_nic);
    t_check(m_tt[0] == external,
            "final release does not overwrite an externally changed TT0");

    m_reset(never, 4);
    m_tt[1] = TT_E;                  /* leave TT0 as the only trial rung */
    m_mutate_on_ask = 1;             /* the probe after installing TT0 */
    m_mutate_which = 0;
    m_mutate_value = external;
    (VOID)netdev_cache_guard(&m_nic, BOARD, SIZE);
    t_check(m_tt[0] == external,
            "a failed TT trial also preserves an intervening owner change");
    netdev_cache_release(&m_nic);
}

static void p_dcache_attach_open_close_lifecycle(void)
{
    static const BOOL answers[] = { FALSE, TRUE, TRUE };

    m_reset(answers, 3);
    m_tt[0] = m_tt[1] = TT_E;
    t_check(netdev_cache_guard(&m_nic, BOARD, SIZE) == NETDEV_CACHE_DCACHE,
            "attach selects the global-cache fallback");
    netdev_cache_release(&m_nic);
    t_check(nd_dc_refs == 0 && (m_cacr & CACR_ED) != 0,
            "attach does not leave the machine cache disabled");
    t_check(netdev_cache_acquire(&m_nic) == NETDEV_CACHE_DCACHE,
            "first Open disables the cache before its only probe");
    t_check(m_asked == 3 && nd_dc_refs == 1,
            "the global-cache reacquire made no unguarded board access");
    netdev_cache_release(&m_nic);
    t_check(nd_dc_refs == 0 && (m_cacr & CACR_ED) != 0,
            "last Close restores the cache");
}

static void q_borrow_does_not_replace_preference(void)
{
    static const BOOL answers[] = { FALSE, TRUE, FALSE, TRUE, TRUE, TRUE };
    NetdevNic a, b;

    m_reset(answers, 6);
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.ops = b.ops = &m_ops_asking;

    t_check(netdev_cache_guard(&b, BOARD, SIZE) == NETDEV_CACHE_TT0,
            "unit B selects TT0 at attach");
    netdev_cache_release(&b);

    m_tt[0] = m_tt[1] = TT_E;
    t_check(netdev_cache_guard(&a, 0x50000000UL, SIZE) ==
                NETDEV_CACHE_DCACHE,
            "unit A holds the global-cache fallback");
    t_check(netdev_cache_acquire(&b) == NETDEV_CACHE_DCACHE,
            "B temporarily borrows A's active global lease");
    t_check(b.cache_mode == NETDEV_CACHE_TT0,
            "borrowing DC does not replace B's attach-selected TT0");
    netdev_cache_release(&b);
    netdev_cache_release(&a);

    m_tt[0] = m_tt[1] = 0;
    t_check(netdev_cache_acquire(&b) == NETDEV_CACHE_TT0,
            "after A closes, B reacquires its own TT preference");
    t_check(b.cache_mode == NETDEV_CACHE_TT0 && nd_tt_lease[0].refs == 1,
            "the preferred strategy and its lease remain consistent");
    netdev_cache_release(&b);
}

static void r_external_protection_can_disappear_before_open(void)
{
    static const BOOL yes[] = { TRUE, TRUE, TRUE };

    m_reset(yes, 3);
    m_cacr = 0;                    /* external utility supplied coherence */
    t_check(netdev_cache_guard(&m_nic, BOARD, SIZE) == NETDEV_CACHE_TT0,
            "attach classifies coherent-as-found conservatively as TT0");
    netdev_cache_release(&m_nic);

    m_cacr = CACR_ED;              /* external protection disappears */
    t_check(netdev_cache_acquire(&m_nic) == NETDEV_CACHE_TT0,
            "first Open installs TT0 before touching the now-cacheable board");
    t_check(m_asked == 3 && m_tt[0] == 0x40008507UL,
            "Open made exactly one probe, after its own guard was active");
    netdev_cache_release(&m_nic);
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
    k_attach_open_close_lifecycle();
    l_two_units_share_tt();
    m_nonoverlap_uses_other_tt();
    n_two_units_share_dcache();
    o_external_tt_changes_are_not_clobbered();
    p_dcache_attach_open_close_lifecycle();
    q_borrow_does_not_replace_preference();
    r_external_protection_can_disappear_before_open();

    printf("%lu checks, %lu failures, %s\n", t_checks, t_failures,
           (t_failures == 0UL) ? "PASS" : "FAIL");

    return (t_failures == 0UL) ? 0 : 1;
}
