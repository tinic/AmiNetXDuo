/*
 * The transmit data path -- bus_wdata() and bus_wdata_long() -- on the host.
 *
 * WHY THIS FILE EXISTS.  Every read path in netdev_bus.c is observable from
 * its destination buffer, so test_netdev_bus.c drives the real code and reads
 * the answer.  A write path has no destination: the data port is ONE address,
 * each access overwrites the last, and a host test can see only the final
 * value -- not the order, not the count, not whether every byte went out.
 * That is why these two were the last functions in src/netdev reached by
 * nothing at all, while being the path every transmitted frame takes on
 * ne2000.c:214 and :321 (xsurf, xsurf100, ariadne2, the PCMCIA NE2000 clones)
 * and el3.c:631 (the 3c589).
 *
 * So the file includes netdev_bus.c with BUS_PUT8/16/32 and BUS_OUT_L/W
 * defined as recorders.  Each expands to the identical access when they are
 * not defined, and the m68k object is byte-identical either way -- verified,
 * 2068 bytes, .text 0x5c4 -- so no core sees a different instruction stream.
 * Same idiom as NIC_GET/PUT in dp8390.c and PNP_W8 in netdev_isapnp.c.
 *
 * ENDIANNESS.  test_netdev_bus.c's rule applies and this file keeps it: a
 * claim about a word access is stated as a word value, never decomposed into
 * host bytes.  Where the implementation packs bytes explicitly -- an odd
 * source, or a trailing odd byte -- the expectation IS stated as arithmetic on
 * the source bytes, because that is the claim worth making: the chip must see
 * src[k] before src[k+1], and getting that pair backwards is the classic
 * defect this path can have.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ the recorder */

#define WIRE_MAX 8192

static struct { unsigned char width; unsigned long value; } wire[WIRE_MAX];
static unsigned wire_n;
static int      wire_over;      /* the recorder itself ran out of room */

static void wire_put(unsigned char width, unsigned long value)
{
    if (wire_n >= WIRE_MAX)
    {
        wire_over = 1;
        return;
    }
    wire[wire_n].width = width;
    wire[wire_n].value = value;
    wire_n++;
}

/*
 * A block is 32 bytes for both batch forms -- netdev_bus.c passes `i >> 5` and
 * then advances the host pointer by `i >> 2` longs / `i >> 1` words -- so the
 * recorder expands one into the scalar accesses it stands for.  That keeps
 * every expectation below in one vocabulary.
 */
/* (void)(p) keeps the port pointer "used": the recorder does not need the
   address, but the shipped code declares it and -Wextra is on. */
#define BUS_PUT8(p, v)   ((void)(p), wire_put(1, (unsigned long)(UBYTE)(v)))
#define BUS_PUT16(p, v)  ((void)(p), wire_put(2, (unsigned long)(UWORD)(v)))
#define BUS_PUT32(p, v)  ((void)(p), wire_put(4, (unsigned long)(v)))

#define BUS_OUT_L(port, from, blocks)                                        \
    do {                                                                     \
        const ULONG *_p = (const ULONG *)(const void *)(from);               \
        unsigned long _n = (unsigned long)(blocks) * 8ul, _k;                \
        (void)(port);                                                        \
        for (_k = 0; _k < _n; _k++) wire_put(4, (unsigned long)_p[_k]);      \
    } while (0)

#define BUS_OUT_W(port, from, blocks)                                        \
    do {                                                                     \
        const UWORD *_p = (const UWORD *)(const void *)(from);               \
        unsigned long _n = (unsigned long)(blocks) * 16ul, _k;               \
        (void)(port);                                                        \
        for (_k = 0; _k < _n; _k++) wire_put(2, (unsigned long)_p[_k]);      \
    } while (0)

#include "netdev_bus.c"

/* ------------------------------------------------------------------ checks */

static int failures;

static void ok(const char *what, int cond)
{
    if (cond)
    {
        printf("ok   %s\n", what);
        return;
    }
    printf("FAIL %s\n", what);
    failures++;
}

/* The port objects.  Their contents are never read; only their addresses are
   handed to the bus, and the recorder is what observes the traffic. */
static union { UWORD w[32]; UBYTE b[64]; } regs;
static union { ULONG l;     UBYTE b[4];  } wide;

/* A source arena with room for an odd start, and a known filling. */
static union { ULONG l[512]; UBYTE b[2048]; } arena;

static void arena_fill(void)
{
    unsigned i;

    /* Every byte distinct within a 251-long cycle, so a burst that lands one
       byte early or late is a different value and not a lucky match. */
    for (i = 0; i < sizeof(arena.b); i++)
        arena.b[i] = (UBYTE)(1u + (i % 251u));
}

/*
 * The one place the expectation is built, and it is derived from the CONTRACT
 * rather than copied from the implementation.  Which form runs is decided the
 * way netdev_bus.h documents bus_long_ok(): a 32-bit window, a longword
 * aligned source and at least four bytes, or else the word form; and a byte
 * port always takes one byte per access.
 *
 *   byte port          one access per byte, value = src[k]
 *   word port, odd     an explicit big-endian pack: src[k] then src[k+1]
 *   word port, even    the host's own word at src+k (see ENDIANNESS above)
 *   long window        the host's own long at src+k, tail as one word
 *
 * A trailing odd byte is `src[k] << 8` in the word form: the chip takes the
 * high half first, so the last byte must sit there and not in the low half.
 */
#define FORM_BYTE  0
#define FORM_WORD  1        /* host word loads, even source */
#define FORM_PACK  2        /* explicit big-endian pack, odd source */
#define FORM_LONG  3

static unsigned exp_n;
static struct { unsigned char width; unsigned long value; } exp[WIRE_MAX];

static void exp_put(unsigned char w, unsigned long v)
{
    if (exp_n < WIRE_MAX)
    {
        exp[exp_n].width = w;
        exp[exp_n].value = v;
    }
    exp_n++;
}

static void expect_build(const UBYTE *src, UWORD len, int form)
{
    unsigned k;

    exp_n = 0;

    if (form == FORM_BYTE)
    {
        for (k = 0; k < len; k++)
            exp_put(1, src[k]);
        return;
    }

    if (form == FORM_LONG)
    {
        for (k = 0; k + 4 <= (unsigned)len; k += 4)
            exp_put(4, (unsigned long)*(const ULONG *)(const void *)(src + k));
        /*
         * ONE word, whatever is left.  With the even length every caller
         * passes, that is exactly the last two bytes.  test_odd_tail_contract()
         * below covers what an odd length would do, and why nobody may pass
         * one.
         */
        if (k < len)
            exp_put(2, (unsigned long)*(const UWORD *)(const void *)(src + k));
        return;
    }

    for (k = 0; k + 2 <= (unsigned)len; k += 2)
    {
        if (form == FORM_PACK)
            exp_put(2, (unsigned long)(((unsigned)src[k] << 8) | src[k + 1]));
        else
            exp_put(2, (unsigned long)*(const UWORD *)(const void *)(src + k));
    }
    if (k < len)
        exp_put(2, (unsigned long)((unsigned)src[k] << 8));
}

static int stream_matches(void)
{
    unsigned i;

    if (wire_over || exp_n != wire_n || exp_n >= WIRE_MAX)
        return 0;
    for (i = 0; i < wire_n; i++)
        if (wire[i].width != exp[i].width || wire[i].value != exp[i].value)
            return 0;
    return 1;
}

static unsigned exp_bytes(void)
{
    unsigned i, n = 0;

    for (i = 0; i < exp_n; i++)
        n += exp[i].width;
    return n;
}

/* Bytes the chip actually received, counted from the widths. */
static unsigned wire_bytes(void)
{
    unsigned i, n = 0;

    for (i = 0; i < wire_n; i++)
        n += wire[i].width;
    return n;
}

/* Which form the contract says this call must take. */
static int form_for(int dmode, int odd, UWORD len, int have_wide)
{
    if (dmode == NETDEV_DMODE_BYTE)
        return FORM_BYTE;
    if (dmode == NETDEV_DMODE_LONG && have_wide && !odd && len >= 4)
        return FORM_LONG;
    return odd ? FORM_PACK : FORM_WORD;
}

static void one(const char *label, int dmode, int odd, UWORD len)
{
    NetdevBus    bus;
    const UBYTE *src = arena.b + (odd ? 1 : 0);
    char         what[160];
    int          have_wide = (dmode == NETDEV_DMODE_LONG);
    int          form;

    arena_fill();
    wire_n = 0;
    wire_over = 0;

    netdev_bus_setup(&bus, regs.b, 2, have_wide ? wide.b : NULL);
    bus.dmode = (UBYTE)dmode;

    netdev_bus_wdata(&bus, src, len);

    form = form_for(dmode, odd, len, have_wide);
    expect_build(src, len, form);

    snprintf(what, sizeof(what), "%s len %u: every byte in order", label,
             (unsigned)len);
    if (!stream_matches())
        printf("     %u accesses, expected %u\n", wire_n, exp_n);
    ok(what, stream_matches());

    snprintf(what, sizeof(what), "%s len %u: %u bytes reached the port",
             label, (unsigned)len, exp_bytes());
    if (wire_bytes() != exp_bytes())
        printf("     got %u\n", wire_bytes());
    ok(what, wire_bytes() == exp_bytes());
}

/*
 * THE ODD-LENGTH CONTRACT, and it is not symmetric between the two forms.
 *
 * The word form pads: a trailing byte goes out in the high half of one word,
 * so all len bytes reach the chip and it sees len+1.  The long form does NOT:
 * after the longword loop it writes exactly ONE word, so a 3-byte tail
 * delivers two bytes and DROPS THE THIRD.
 *
 * Nothing hits that today -- every caller passes an even length
 * (ne2000.c:321 and el3.c:631 round up with `(len + 1) & ~1`, and
 * ne2000_writemem's four callers pass 32, 32, 32 and ED_PAGE_SIZE) -- so this
 * is a precondition, not a live defect.  It is pinned here so that removing a
 * caller's round-up fails a test instead of quietly truncating a frame.
 */
static void test_odd_tail_contract(void)
{
    NetdevBus bus;

    arena_fill();

    wire_n = 0;
    wire_over = 0;
    netdev_bus_setup(&bus, regs.b, 2, NULL);
    bus.dmode = NETDEV_DMODE_WORD;
    netdev_bus_wdata(&bus, arena.b, 7);
    ok("word form, len 7: 4 accesses, 8 bytes -- the odd byte is padded",
       wire_n == 4 && wire_bytes() == 8);
    ok("word form, len 7: the last access carries src[6] in the high half",
       wire_n == 4 &&
       wire[3].value == (unsigned long)((unsigned)arena.b[6] << 8));

    wire_n = 0;
    wire_over = 0;
    netdev_bus_setup(&bus, regs.b, 2, wide.b);
    bus.dmode = NETDEV_DMODE_LONG;
    netdev_bus_wdata(&bus, arena.b, 7);
    ok("long form, len 7: 2 accesses, 6 bytes -- src[6] is NOT sent",
       wire_n == 2 && wire_bytes() == 6);
    ok("long form, len 7: one long then one word",
       wire_n == 2 && wire[0].width == 4 && wire[1].width == 2);
}

/*
 * Every tail residue at several magnitudes, the batch boundary from both
 * sides, and the two lengths test_netdev_bus.c already calls out: 46 is the
 * shortest Ethernet payload and 331 a DHCP offer's.
 */
static const UWORD lens[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 31, 32, 33, 34, 35,
                              46, 63, 64, 65, 100, 331, 512, 1460, 1461,
                              1462, 1463, 1500 };

static void test_byte_port(void)
{
    UWORD i;

    /* An 8-bit port takes one byte per access whatever the source alignment,
       so both alignments must produce the identical stream. */
    for (i = 0; i < (UWORD)(sizeof(lens) / sizeof(lens[0])); i++)
        one("byte port", NETDEV_DMODE_BYTE, 0, lens[i]);
    for (i = 0; i < (UWORD)(sizeof(lens) / sizeof(lens[0])); i++)
        one("byte port, odd source", NETDEV_DMODE_BYTE, 1, lens[i]);
}

static void test_word_port(void)
{
    UWORD i;

    for (i = 0; i < (UWORD)(sizeof(lens) / sizeof(lens[0])); i++)
        one("word port", NETDEV_DMODE_WORD, 0, lens[i]);
    for (i = 0; i < (UWORD)(sizeof(lens) / sizeof(lens[0])); i++)
        one("word port, odd source", NETDEV_DMODE_WORD, 1, lens[i]);
}

static void test_long_window(void)
{
    UWORD i;

    /* Even lengths only: an odd one cannot reach this path and what it would
       do is test_odd_tail_contract()'s subject, not a sweep's. */
    for (i = 0; i < (UWORD)(sizeof(lens) / sizeof(lens[0])); i++)
        if ((lens[i] & 1u) == 0)
            one("long window", NETDEV_DMODE_LONG, 0, lens[i]);

    /*
     * An odd source is refused the long window by bus_long_ok() and falls back
     * to the byte-packing word form -- the alignment check is the whole point
     * of that helper, because a long access to an odd address is an address
     * error on a 68000 and a silent misread elsewhere.
     */
    for (i = 0; i < (UWORD)(sizeof(lens) / sizeof(lens[0])); i++)
        one("long window, odd source falls back", NETDEV_DMODE_LONG, 1,
            lens[i]);
}

/*
 * bus_long_ok() also refuses a length under 4 and a NULL window, and those
 * refusals are what keep a short frame off the long path entirely.
 */
static void test_long_refusals(void)
{
    NetdevBus bus;

    arena_fill();

    wire_n = 0;
    netdev_bus_setup(&bus, regs.b, 2, NULL);
    bus.dmode = NETDEV_DMODE_LONG;
    netdev_bus_wdata(&bus, arena.b, 64);
    ok("LONG with no wide window uses the word form",
       wire_n == 32 && wire[0].width == 2);

    wire_n = 0;
    netdev_bus_setup(&bus, regs.b, 2, wide.b);
    bus.dmode = NETDEV_DMODE_LONG;
    netdev_bus_wdata(&bus, arena.b, 2);
    ok("LONG with len 2 uses the word form",
       wire_n == 1 && wire[0].width == 2);

    wire_n = 0;
    netdev_bus_wdata(&bus, arena.b, 4);
    ok("LONG with len 4 uses the long form",
       wire_n == 1 && wire[0].width == 4);
}

/*
 * THE len >= 4 REFUSAL NEEDS len 1 AND len 3 TO BE VISIBLE AT ALL, and that is
 * worth writing down because it is not obvious: at len 2 the two forms are
 * INDISTINGUISHABLE.  Both emit a single word access, of the same value, to
 * the same address -- so a bus_long_ok() that had lost its length check would
 * pass every len-2 assertion honestly.  Only the lengths that are not a whole
 * number of words separate them:
 *
 *   len 1   word form: ONE access, src[0] << 8   (the byte, padded)
 *           long form: ONE access, the host word at src+0, reading src[1] too
 *   len 3   word form: TWO accesses, a word then src[2] << 8
 *           long form: ONE access, the host word at src+0 -- two bytes short
 *
 * Found by mutation: removing `len >= 4` survived a sweep that used only even
 * lengths.
 */
static void test_long_refuses_short(void)
{
    NetdevBus bus;

    arena_fill();
    netdev_bus_setup(&bus, regs.b, 2, wide.b);
    bus.dmode = NETDEV_DMODE_LONG;

    wire_n = 0;
    wire_over = 0;
    netdev_bus_wdata(&bus, arena.b, 1);
    ok("LONG len 1 takes the word form, padding the single byte",
       wire_n == 1 && wire[0].width == 2 &&
       wire[0].value == (unsigned long)((unsigned)arena.b[0] << 8));

    wire_n = 0;
    wire_over = 0;
    netdev_bus_wdata(&bus, arena.b, 3);
    ok("LONG len 3 takes the word form: two accesses, not one",
       wire_n == 2 && wire[0].width == 2 && wire[1].width == 2 &&
       wire[1].value == (unsigned long)((unsigned)arena.b[2] << 8));
}

int main(void)
{
    test_byte_port();
    test_word_port();
    test_long_window();
    test_long_refusals();
    test_odd_tail_contract();
    test_long_refuses_short();

    printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
