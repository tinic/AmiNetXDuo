/*
 * AmiNetXDuo, host-side test for the mbuf emulation.
 *
 * Builds and runs on the development host, not on the Amiga. mbuf_alloc.c and
 * mbuf_ops.c contain no AmigaOS calls, so all they need is the <exec/types.h>
 * shim in src/config/test/shim, the four stubs below, and the replica struct
 * layout that -DAMI_MBUF_REPLICA selects in include/aminetxduo/mbuf.h.
 *
 * What this cannot cover, and what covers it instead:
 *   - the exact 68k struct offsets: asserted at compile time against the real
 *     NDK <sys/mbuf.h> by src/mbuf/mbuf_abi_check.c.
 *   - MSIZE 128 and MLEN/MHLEN 108/100: the replica scales those with the
 *     pointer width, so on a 64-bit host they are 256/224/208. Every invariant
 *     the code relies on still holds, and every boundary case below is still a
 *     boundary case at a different number. The on-Amiga test in
 *     tests/mbuf_bpf/ runs the same battery at the real ones.
 *   - Forbid()/Permit(), and the 128-byte alignment AllocVec() does not
 *     promise: also tests/mbuf_bpf/.
 *   - the NX_PACKET bridge: needs a running packet pool, so tests/mbuf_bpf/.
 *
 *   cc -std=c11 -Wall -Wextra -DAMI_MBUF_REPLICA -I../../../include \
 *      -I../../config/test/shim -I.. test_mbuf.c ../mbuf_alloc.c \
 *      ../mbuf_ops.c -o test_mbuf
 *
 * SPDX-License-Identifier: MIT
 */

#include "mbuf_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ stubs */

static ULONG stub_outstanding;
static int   stub_verbose;

APTR ami_alloc(ULONG size)
{
    void *p;

    if (size == 0)
        return NULL;

    p = calloc(1, size);
    if (p != NULL)
        stub_outstanding++;

    return p;
}

APTR ami_alloc_flags(ULONG size, ULONG memf)
{
    (void)memf;
    return ami_alloc(size);
}

VOID ami_free(APTR ptr)
{
    if (ptr == NULL)
        return;

    free(ptr);
    stub_outstanding--;
}

ULONG ami_alloc_count(VOID)
{
    return stub_outstanding;
}

VOID ami_log(int level, const char *fmt, ...)
{
    va_list args;

    (void)level;
    if (!stub_verbose)
        return;

    va_start(args, fmt);
    fputs("  [log] ", stdout);
    vprintf(fmt, args);
    fputc('\n', stdout);
    va_end(args);
}

/* No preemption on the host. The real ones are Forbid()/Permit(). */
VOID ami_mbuf_lock(VOID)   { }
VOID ami_mbuf_unlock(VOID) { }

/* ----------------------------------------------------------- check harness */

static int checks;
static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                  \
    } while (0)

/* ---------------------------------------------------------------- helpers */

/* Fill a buffer with a recognisable ramp so a misplaced copy is obvious. */
static void ramp(UBYTE *p, ULONG len, UBYTE start)
{
    ULONG i;

    for (i = 0; i < len; i++)
        p[i] = (UBYTE)(start + i);
}

static int ramp_ok(const UBYTE *p, ULONG len, UBYTE start)
{
    ULONG i;

    for (i = 0; i < len; i++)
    {
        if (p[i] != (UBYTE)(start + i))
            return 0;
    }

    return 1;
}

/* A chain of `n` mbufs each holding `each` bytes of one continuous ramp. */
static struct mbuf *make_chain(ULONG n, ULONG each, UBYTE start)
{
    struct mbuf *head = NULL;
    struct mbuf **np  = &head;
    ULONG        i;

    for (i = 0; i < n; i++)
    {
        struct mbuf *m = ami_mbuf_get();

        if (m == NULL)
            return head;

        ramp((UBYTE *)m->m_data, each, (UBYTE)(start + i * each));
        m->m_len = (LONG)each;
        *np = m;
        np  = &m->m_next;
    }

    return head;
}

static void expect_empty(const char *where)
{
    if (ami_mbuf_outstanding() != 0 || ami_mbuf_clusters_outstanding() != 0)
    {
        failures++;
        printf("  FAIL %s: leak, %lu mbufs, %lu clusters outstanding\n",
               where, (unsigned long)ami_mbuf_outstanding(),
               (unsigned long)ami_mbuf_clusters_outstanding());
    }
    checks++;
}

/* ------------------------------------------------------------------ tests */

static void test_layout(void)
{
    struct mbuf *m;

    printf("mbuf: layout and allocation\n");

    /* The properties the ABI and dtom() depend on, whatever MSIZE is here. */
    CHECK(sizeof(struct mbuf) == MSIZE);
    CHECK((MSIZE & (MSIZE - 1)) == 0);
    CHECK((ULONG)(sizeof(struct m_hdr) + MLEN) == (ULONG)MSIZE);
    CHECK((ULONG)(sizeof(struct m_hdr) + sizeof(struct pkthdr) + MHLEN)
          == (ULONG)MSIZE);

    m = ami_mbuf_get();
    CHECK(m != NULL);
    if (m == NULL)
        return;

    /* MSIZE alignment is what makes dtom() work. */
    CHECK(((unsigned long)m & (MSIZE - 1)) == 0);
    CHECK(dtom((UBYTE *)m->m_data + 3) == m);
    CHECK(m->m_data == (APTR)m->m_dat);
    CHECK(m->m_len == 0);
    CHECK(m->m_type == MT_DATA);
    CHECK(m->m_flags == 0);
    CHECK(m->m_next == NULL);
    CHECK(m->m_nextpkt == NULL);

    CHECK(ami_mbuf_free(m) == NULL);

    m = ami_mbuf_gethdr();
    CHECK(m != NULL);
    if (m != NULL)
    {
        CHECK((m->m_flags & M_PKTHDR) != 0);
        CHECK(m->m_data == (APTR)m->m_pktdat);
        CHECK(m->m_pkthdr.len == 0);
        CHECK(m->m_pkthdr.rcvif == NULL);
        CHECK(m->m_type == MT_HEADER);
        (void)ami_mbuf_free(m);
    }

    expect_empty("test_layout");
}

static void test_alignment_bulk(void)
{
    struct mbuf *chain = NULL;
    ULONG        i;
    int          aligned = 1;

    printf("mbuf: every mbuf in a full slab sweep is MSIZE aligned\n");

    /* Enough to span several slabs, so the alignment slack is exercised more
       than once. */
    for (i = 0; i < 100; i++)
    {
        struct mbuf *m = ami_mbuf_get();

        if (m == NULL)
            break;

        if (((unsigned long)m & (MSIZE - 1)) != 0)
            aligned = 0;
        if (dtom((UBYTE *)m->m_dat + MLEN - 1) != m)
            aligned = 0;

        m->m_next = chain;
        chain     = m;
    }

    CHECK(i == 100);
    CHECK(aligned);

    ami_mbuf_freem(chain);
    expect_empty("test_alignment_bulk");
}

static void test_exhaustion(void)
{
    struct mbuf *chain = NULL;
    struct mbuf *m;
    ULONG        got = 0;
    struct mbstat st;

    printf("mbuf: pool ceiling and mbstat\n");

    ami_mbuf_cleanup();
    CHECK(ami_mbuf_init(8, 2) == 0);
    CHECK(ami_mbuf_init(8, 2) == 0);          /* idempotent with same limits */
    CHECK(ami_mbuf_init(9, 2) == -1);         /* and refuses to be changed   */

    while ((m = ami_mbuf_get()) != NULL)
    {
        m->m_next = chain;
        chain     = m;
        got++;
        if (got > 64)
            break;                            /* ceiling not honoured        */
    }

    CHECK(got == 8);

    ami_mbuf_stats(&st);
    CHECK(st.m_mbufs == 8);
    CHECK(st.m_drops > 0);                    /* the failed get is a drop    */
    CHECK(st.m_wait == 0);                    /* no blocking path exists     */

    ami_mbuf_freem(chain);
    expect_empty("test_exhaustion");

    ami_mbuf_cleanup();
    CHECK(ami_mbuf_init(0, 0) == 0);
}

static void test_length_and_free(void)
{
    struct mbuf *m;
    struct mbuf *second;

    printf("mbuf: length, free returns the successor\n");

    m = make_chain(3, 10, 0);
    CHECK(m != NULL);
    CHECK(ami_mbuf_length(m) == 30);

    second = m->m_next;
    CHECK(ami_mbuf_free(m) == second);
    ami_mbuf_freem(second);

    CHECK(ami_mbuf_free(NULL) == NULL);
    ami_mbuf_freem(NULL);
    CHECK(ami_mbuf_length(NULL) == 0);

    expect_empty("test_length_and_free");
}

static void test_adj(void)
{
    struct mbuf *m;

    printf("mbuf: adj trims head and tail without freeing\n");

    /* Trim from the front, across a whole mbuf and into the next. */
    m = make_chain(3, 10, 0);
    CHECK(ami_mbuf_adj(m, 15) == 0);
    CHECK(ami_mbuf_length(m) == 15);
    CHECK(m->m_len == 0);                     /* emptied, not freed          */
    CHECK(m->m_next->m_len == 5);
    CHECK(ramp_ok((const UBYTE *)m->m_next->m_data, 5, 15));
    ami_mbuf_freem(m);

    /* Trim from the tail, across a whole mbuf. */
    m = make_chain(3, 10, 0);
    CHECK(ami_mbuf_adj(m, -15) == 0);
    CHECK(ami_mbuf_length(m) == 15);
    CHECK(m->m_len == 10);
    CHECK(m->m_next->m_len == 5);
    CHECK(m->m_next->m_next->m_len == 0);
    CHECK(ramp_ok((const UBYTE *)m->m_data, 10, 0));
    ami_mbuf_freem(m);

    /* Over-trimming leaves an empty chain, not a corrupt one. */
    m = make_chain(2, 10, 0);
    CHECK(ami_mbuf_adj(m, 999) == 0);
    CHECK(ami_mbuf_length(m) == 0);
    ami_mbuf_freem(m);

    m = make_chain(2, 10, 0);
    CHECK(ami_mbuf_adj(m, -999) == 0);
    CHECK(ami_mbuf_length(m) == 0);
    ami_mbuf_freem(m);

    /* m_pkthdr.len follows, in both directions. */
    m = ami_mbuf_gethdr();
    ramp((UBYTE *)m->m_data, 20, 0);
    m->m_len        = 20;
    m->m_pkthdr.len = 20;
    CHECK(ami_mbuf_adj(m, 5) == 0);
    CHECK(m->m_pkthdr.len == 15);
    CHECK(m->m_len == 15);
    CHECK(ramp_ok((const UBYTE *)m->m_data, 15, 5));
    CHECK(ami_mbuf_adj(m, -5) == 0);
    CHECK(m->m_pkthdr.len == 10);
    CHECK(m->m_len == 10);
    ami_mbuf_freem(m);

    CHECK(ami_mbuf_adj(NULL, 1) == -1);

    expect_empty("test_adj");
}

static void test_cat(void)
{
    struct mbuf *a;
    struct mbuf *b;

    printf("mbuf: cat compacts what fits and links what does not\n");

    /* Small tail, small source: b is absorbed and freed. */
    a = make_chain(1, 10, 0);
    b = make_chain(1, 10, 10);
    CHECK(ami_mbuf_cat(a, b) == 0);
    CHECK(a->m_next == NULL);                 /* compacted, b gone           */
    CHECK(a->m_len == 20);
    CHECK(ramp_ok((const UBYTE *)a->m_data, 20, 0));
    ami_mbuf_freem(a);
    expect_empty("cat compaction");

    /* Full tail: the chains are spliced instead. */
    a = ami_mbuf_get();
    a->m_len = AMI_MLEN;
    ramp((UBYTE *)a->m_data, (ULONG)AMI_MLEN, 0);
    b = make_chain(1, 10, 0);
    CHECK(ami_mbuf_cat(a, b) == 0);
    CHECK(a->m_next == b);
    CHECK(ami_mbuf_length(a) == (ULONG)AMI_MLEN + 10);
    ami_mbuf_freem(a);
    expect_empty("cat splice");

    /* Multi-mbuf source, appended to a multi-mbuf destination. */
    a = make_chain(2, 10, 0);
    b = make_chain(2, 10, 20);
    CHECK(ami_mbuf_cat(a, b) == 0);
    CHECK(ami_mbuf_length(a) == 40);
    {
        UBYTE out[40];

        CHECK(ami_mbuf_copydata(a, 0, 40, out) == 0);
        CHECK(ramp_ok(out, 40, 0));
    }
    ami_mbuf_freem(a);

    CHECK(ami_mbuf_cat(NULL, NULL) == -1);

    expect_empty("test_cat");
}

static void test_copydata(void)
{
    struct mbuf *m;
    UBYTE        out[64];

    printf("mbuf: copydata, including the short-chain rejection\n");

    m = make_chain(3, 10, 0);

    memset(out, 0xAA, sizeof(out));
    CHECK(ami_mbuf_copydata(m, 0, 30, out) == 0);
    CHECK(ramp_ok(out, 30, 0));

    memset(out, 0xAA, sizeof(out));
    CHECK(ami_mbuf_copydata(m, 5, 20, out) == 0);
    CHECK(ramp_ok(out, 20, 5));

    /* Straddling every boundary. */
    memset(out, 0xAA, sizeof(out));
    CHECK(ami_mbuf_copydata(m, 9, 12, out) == 0);
    CHECK(ramp_ok(out, 12, 9));

    /* Past the end: rejected, and nothing is written. 4.4BSD panics here. */
    memset(out, 0xAA, sizeof(out));
    CHECK(ami_mbuf_copydata(m, 0, 31, out) == -1);
    CHECK(out[0] == 0xAA);
    CHECK(ami_mbuf_copydata(m, 25, 10, out) == -1);
    CHECK(out[0] == 0xAA);
    CHECK(ami_mbuf_copydata(m, 30, 1, out) == -1);

    CHECK(ami_mbuf_copydata(m, 0, 0, out) == 0);
    CHECK(ami_mbuf_copydata(m, -1, 1, out) == -1);
    CHECK(ami_mbuf_copydata(m, 0, -1, out) == -1);
    CHECK(ami_mbuf_copydata(NULL, 0, 1, out) == -1);
    CHECK(ami_mbuf_copydata(m, 0, 1, NULL) == -1);

    ami_mbuf_freem(m);
    expect_empty("test_copydata");
}

static void test_copyback(void)
{
    struct mbuf *m;
    UBYTE        src[40];
    UBYTE        out[200];

    printf("mbuf: copyback writes in place, extends, and zero-fills gaps\n");

    /* Overwrite inside the existing chain. */
    m = make_chain(3, 10, 0);
    ramp(src, 10, 100);
    CHECK(ami_mbuf_copyback(m, 5, 10, src) == 0);
    CHECK(ami_mbuf_copydata(m, 5, 10, out) == 0);
    CHECK(ramp_ok(out, 10, 100));
    CHECK(ami_mbuf_length(m) == 30);
    ami_mbuf_freem(m);

    /* Extend past the end of the chain. */
    m = make_chain(1, 10, 0);
    ramp(src, 20, 50);
    CHECK(ami_mbuf_copyback(m, 10, 20, src) == 0);
    CHECK(ami_mbuf_length(m) == 30);
    CHECK(ami_mbuf_copydata(m, 0, 10, out) == 0);
    CHECK(ramp_ok(out, 10, 0));
    CHECK(ami_mbuf_copydata(m, 10, 20, out) == 0);
    CHECK(ramp_ok(out, 20, 50));
    ami_mbuf_freem(m);

    /* A gap between the end of the chain and the offset is zero-filled. */
    m = make_chain(1, 10, 0);
    ramp(src, 4, 200);
    CHECK(ami_mbuf_copyback(m, 30, 4, src) == 0);
    CHECK(ami_mbuf_length(m) >= 34);
    CHECK(ami_mbuf_copydata(m, 10, 20, out) == 0);
    {
        int all_zero = 1;
        int i;

        for (i = 0; i < 20; i++)
        {
            if (out[i] != 0)
                all_zero = 0;
        }
        CHECK(all_zero);
    }
    CHECK(ami_mbuf_copydata(m, 30, 4, out) == 0);
    CHECK(ramp_ok(out, 4, 200));
    ami_mbuf_freem(m);

    /* m_pkthdr.len grows to cover what was written. */
    m = ami_mbuf_gethdr();
    m->m_len        = 4;
    m->m_pkthdr.len = 4;
    ramp(src, 8, 0);
    CHECK(ami_mbuf_copyback(m, 4, 8, src) == 0);
    CHECK(m->m_pkthdr.len == 12);
    ami_mbuf_freem(m);

    CHECK(ami_mbuf_copyback(NULL, 0, 1, src) == -1);

    expect_empty("test_copyback");
}

static void test_copym(void)
{
    struct mbuf *m;
    struct mbuf *c;
    UBYTE        out[64];

    printf("mbuf: copym, offsets, M_COPYALL and pkthdr propagation\n");

    m = make_chain(3, 10, 0);

    c = ami_mbuf_copym(m, 0, 30);
    CHECK(c != NULL);
    CHECK(ami_mbuf_length(c) == 30);
    CHECK(ami_mbuf_copydata(c, 0, 30, out) == 0);
    CHECK(ramp_ok(out, 30, 0));
    ami_mbuf_freem(c);

    /* A range that starts and ends inside different mbufs. */
    c = ami_mbuf_copym(m, 7, 16);
    CHECK(c != NULL);
    CHECK(ami_mbuf_length(c) == 16);
    CHECK(ami_mbuf_copydata(c, 0, 16, out) == 0);
    CHECK(ramp_ok(out, 16, 7));
    ami_mbuf_freem(c);

    c = ami_mbuf_copym(m, 5, M_COPYALL);
    CHECK(c != NULL);
    CHECK(ami_mbuf_length(c) == 25);
    CHECK(ami_mbuf_copydata(c, 0, 25, out) == 0);
    CHECK(ramp_ok(out, 25, 5));
    ami_mbuf_freem(c);

    /* Past the end: NULL, and the source is untouched. */
    CHECK(ami_mbuf_copym(m, 0, 31) == NULL);
    CHECK(ami_mbuf_copym(m, 31, 1) == NULL);
    CHECK(ami_mbuf_length(m) == 30);
    CHECK(ami_mbuf_copym(NULL, 0, 1) == NULL);
    CHECK(ami_mbuf_copym(m, -1, 1) == NULL);

    ami_mbuf_freem(m);

    /* An M_PKTHDR source copied from offset 0 gets a header. From an offset
       it does not. */
    m = ami_mbuf_gethdr();
    ramp((UBYTE *)m->m_data, 20, 0);
    m->m_len         = 20;
    m->m_pkthdr.len  = 20;
    m->m_flags       = (WORD)(m->m_flags | M_BCAST);

    c = ami_mbuf_copym(m, 0, 12);
    CHECK(c != NULL);
    CHECK((c->m_flags & M_PKTHDR) != 0);
    CHECK((c->m_flags & M_BCAST) != 0);
    CHECK(c->m_pkthdr.len == 12);
    ami_mbuf_freem(c);

    c = ami_mbuf_copym(m, 4, 12);
    CHECK(c != NULL);
    CHECK((c->m_flags & M_PKTHDR) == 0);
    ami_mbuf_freem(c);

    ami_mbuf_freem(m);
    expect_empty("test_copym");
}

static void test_clusters(void)
{
    struct mbuf *m;
    struct mbuf *c;
    UBYTE        out[64];

    printf("mbuf: clusters, M_EXT, and copym sharing by reference count\n");

    m = ami_mbuf_getcl();
    CHECK(m != NULL);
    if (m == NULL)
        return;

    CHECK((m->m_flags & M_EXT) != 0);
    CHECK(m->m_ext.ext_size == MCLBYTES);
    CHECK(m->m_data == m->m_ext.ext_buf);
    CHECK(ami_mbuf_clusters_outstanding() == 1);

    /* A cluster cannot be attached twice. */
    CHECK(ami_mbuf_clget(m) == -1);

    ramp((UBYTE *)m->m_data, 40, 0);
    m->m_len = 40;

    /* copym shares the cluster rather than copying it: still one cluster. */
    c = ami_mbuf_copym(m, 8, 16);
    CHECK(c != NULL);
    CHECK((c->m_flags & M_EXT) != 0);
    CHECK(c->m_ext.ext_buf == m->m_ext.ext_buf);
    CHECK(ami_mbuf_clusters_outstanding() == 1);
    CHECK(ami_mbuf_copydata(c, 0, 16, out) == 0);
    CHECK(ramp_ok(out, 16, 8));

    /* Freeing the copy drops a reference but not the cluster. */
    ami_mbuf_freem(c);
    CHECK(ami_mbuf_clusters_outstanding() == 1);

    ami_mbuf_freem(m);
    CHECK(ami_mbuf_clusters_outstanding() == 0);

    /* A big build spills into a cluster automatically. */
    {
        UBYTE big[1500];

        ramp(big, sizeof(big), 3);
        m = ami_mbuf_build(big, (ULONG)sizeof(big), TRUE);
        CHECK(m != NULL);
        CHECK(ami_mbuf_length(m) == sizeof(big));
        CHECK(m->m_pkthdr.len == (LONG)sizeof(big));
        CHECK((m->m_flags & M_EXT) != 0);      /* one cluster, not 14 mbufs  */
        CHECK(m->m_next == NULL);
        {
            UBYTE back[1500];

            CHECK(ami_mbuf_copydata(m, 0, (LONG)sizeof(back), back) == 0);
            CHECK(ramp_ok(back, sizeof(back), 3));
        }
        ami_mbuf_freem(m);
    }

    expect_empty("test_clusters");
}

static void test_foreign_ext(void)
{
    static UBYTE  foreign[256];
    struct mbuf  *m;
    struct mbuf  *c;

    printf("mbuf: foreign M_EXT is deep-copied by copym, and is not freed\n");

    ramp(foreign, sizeof(foreign), 7);

    m = ami_mbuf_get();
    CHECK(m != NULL);
    m->m_ext.ext_buf  = foreign;
    m->m_ext.ext_free = NULL;
    m->m_ext.ext_size = (ULONG)sizeof(foreign);
    m->m_data         = foreign;
    m->m_len          = 200;
    m->m_flags        = (WORD)(m->m_flags | M_EXT);

    /* Not one of ours: the copy must own its own storage. */
    c = ami_mbuf_copym(m, 0, 40);
    CHECK(c != NULL);
    CHECK((c->m_flags & M_EXT) == 0);
    CHECK(c->m_data != (APTR)foreign);
    {
        UBYTE out[40];

        CHECK(ami_mbuf_copydata(c, 0, 40, out) == 0);
        CHECK(ramp_ok(out, 40, 7));
    }
    ami_mbuf_freem(c);

    /* Freeing an M_EXT mbuf with no ext_free must not free the buffer. */
    ami_mbuf_freem(m);
    CHECK(foreign[0] == 7);

    expect_empty("test_foreign_ext");
}

static void test_prepend(void)
{
    struct mbuf *m;
    UBYTE        out[64];

    printf("mbuf: prepend takes the in-place path when there is leading room\n");

    /* gethdr puts m_data at the start of m_pktdat, so there is no leading
       room and a new head mbuf is needed. */
    m = ami_mbuf_gethdr();
    ramp((UBYTE *)m->m_data, 20, 20);
    m->m_len        = 20;
    m->m_pkthdr.len = 20;

    m = ami_mbuf_prepend(m, 6);
    CHECK(m != NULL);
    CHECK(m->m_len == 6);
    CHECK((m->m_flags & M_PKTHDR) != 0);
    CHECK(m->m_pkthdr.len == 26);
    CHECK(m->m_next != NULL);
    CHECK((m->m_next->m_flags & M_PKTHDR) == 0);   /* moved to the new head  */
    ramp((UBYTE *)m->m_data, 6, 14);
    CHECK(ami_mbuf_copydata(m, 0, 26, out) == 0);
    CHECK(ramp_ok(out, 26, 14));

    /* Now there is leading room in the new head (MH_ALIGN put the data at the
       far end), so a second prepend must not allocate. */
    {
        ULONG before = ami_mbuf_outstanding();

        m = ami_mbuf_prepend(m, 4);
        CHECK(m != NULL);
        CHECK(ami_mbuf_outstanding() == before);
        CHECK(m->m_len == 10);
        CHECK(m->m_pkthdr.len == 30);
    }

    ami_mbuf_freem(m);
    expect_empty("prepend basics");

    /* Longer than one mbuf can hold: NULL, and the chain is freed. */
    m = make_chain(2, 10, 0);
    CHECK(ami_mbuf_prepend(m, AMI_MLEN + 1) == NULL);
    expect_empty("prepend oversize frees the chain");

    m = make_chain(1, 10, 0);
    CHECK(ami_mbuf_prepend(m, 0) == m);
    ami_mbuf_freem(m);

    CHECK(ami_mbuf_prepend(NULL, 4) == NULL);

    expect_empty("test_prepend");
}

static void test_pullup(void)
{
    struct mbuf *m;

    printf("mbuf: pullup gathers, and frees the chain when it cannot\n");

    /* Already contiguous: unchanged, same head. */
    m = make_chain(2, 10, 0);
    CHECK(ami_mbuf_pullup(m, 10) == m);
    CHECK(m->m_len == 10);
    ami_mbuf_freem(m);

    /* Gather across three mbufs into the head. */
    m = make_chain(3, 10, 0);
    m = ami_mbuf_pullup(m, 25);
    CHECK(m != NULL);
    if (m != NULL)
    {
        CHECK(m->m_len >= 25);
        CHECK(ramp_ok((const UBYTE *)m->m_data, 25, 0));
        CHECK(ami_mbuf_length(m) == 30);
        ami_mbuf_freem(m);
    }
    expect_empty("pullup gather");

    /* More than one mbuf can hold: NULL, and the chain is freed. */
    m = make_chain(4, 50, 0);
    CHECK(ami_mbuf_pullup(m, AMI_MLEN + 1) == NULL);
    expect_empty("pullup oversize frees the chain");

    /* Chain shorter than the request: same contract. */
    m = make_chain(2, 5, 0);
    CHECK(ami_mbuf_pullup(m, 40) == NULL);
    expect_empty("pullup short chain frees the chain");

    /* An M_PKTHDR head that has to be replaced keeps its header. */
    m = ami_mbuf_gethdr();
    m->m_data       = (APTR)((UBYTE *)m + MSIZE - 2);   /* no trailing room  */
    m->m_len        = 2;
    m->m_pkthdr.len = 12;
    ramp((UBYTE *)m->m_data, 2, 0);
    m->m_next = make_chain(1, 10, 2);
    m = ami_mbuf_pullup(m, 12);
    CHECK(m != NULL);
    if (m != NULL)
    {
        CHECK((m->m_flags & M_PKTHDR) != 0);
        CHECK(m->m_pkthdr.len == 12);
        CHECK(m->m_len >= 12);
        CHECK(ramp_ok((const UBYTE *)m->m_data, 12, 0));
        ami_mbuf_freem(m);
    }

    CHECK(ami_mbuf_pullup(NULL, 4) == NULL);

    expect_empty("test_pullup");
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-v") == 0)
        stub_verbose = 1;

    printf("mbuf replica: MSIZE %lu, MLEN %lu, MHLEN %lu\n",
           (unsigned long)MSIZE, (unsigned long)MLEN, (unsigned long)MHLEN);

    test_layout();
    test_alignment_bulk();
    test_exhaustion();
    test_length_and_free();
    test_adj();
    test_cat();
    test_copydata();
    test_copyback();
    test_copym();
    test_clusters();
    test_foreign_ext();
    test_prepend();
    test_pullup();

    ami_mbuf_cleanup();
    CHECK(ami_alloc_count() == 0);

    printf("\n%d checks, %d failure(s)\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
