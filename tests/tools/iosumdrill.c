/*
 * IoSumDrill: n68k_port_in_w_sum() against an independent reference, in the
 * guest, on the real instructions.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "n68k_iocopy.h"

static ULONG checks;
static ULONG failures;

static VOID ck(const char *what, ULONG len, ULONG port, BOOL ok)
{
    checks++;
    if (!ok)
    {
        failures++;
        Printf((STRPTR)"FAIL %s: len %ld port $%04lx\n",
               (STRPTR)what, (LONG)len, (LONG)port);
    }
}

static UBYTE ref_byte(ULONG i, UWORD port)
{
    return (UBYTE)(((i & 1UL) == 0UL) ? (port >> 8) : port);
}

static ULONG ref_sum(ULONG len, UWORD port)
{
    ULONG sum   = 0UL;
    ULONG whole = len & ~3UL;
    ULONG tail  = len & 3UL;
    ULONG i;

    for (i = 0UL; i < whole; i += 4UL)
    {
        ULONG w = ((ULONG)ref_byte(i, port)     << 24) |
                  ((ULONG)ref_byte(i + 1, port) << 16) |
                  ((ULONG)ref_byte(i + 2, port) <<  8) |
                   (ULONG)ref_byte(i + 3, port);

        sum += w;
        if (sum < w)
            sum++;                      /* end-around carry, per add */
    }

    if (tail != 0UL)
    {
        ULONG w = 0UL;

        /* Zero padded and byte positioned: the same thing a walk does with a
           partial trailing longword. */
        for (i = 0UL; i < tail; i++)
            w |= (ULONG)ref_byte(whole + i, port) << (24 - (8 * (int)i));

        sum += w;
        if (sum < w)
            sum++;
    }

    return sum;
}

#define GUARD   0xA5u
#define MAXLEN  1600UL
#define PRE     4UL             /* guard bytes BEFORE the destination too:  */

#define ARENA   (PRE + MAXLEN + 4UL)

static VOID one_into(UBYTE *buf, ULONG len, UWORD portval, const char *tag)
{
    static volatile UWORD port;
    UBYTE  *dst = buf + PRE;
    ULONG   got;
    ULONG   want;
    BOOL    bytes_ok = TRUE;
    ULONG   i;

    for (i = 0UL; i < ARENA; i++)
        buf[i] = (UBYTE)GUARD;

    port = portval;

    got  = n68k_port_in_w_sum(dst, (const volatile void *)&port, len);
    want = ref_sum(len, portval);

    for (i = 0UL; i < PRE; i++)
    {
        if (buf[i] != (UBYTE)GUARD)
            bytes_ok = FALSE;
    }

    for (i = 0UL; i < len; i++)
    {
        if (dst[i] != ref_byte(i, portval))
            bytes_ok = FALSE;
    }

    /* Nothing beyond the length may have been written.  The routine rounds its
       reads up to a word, which is the hardware's granularity, but the extra
       byte belongs to the FIFO and not to the caller's buffer. */
    for (i = len; i < len + 4UL; i++)
    {
        if (dst[i] != (UBYTE)GUARD)
            bytes_ok = FALSE;
    }

    ck(tag, len, portval, bytes_ok);

    if (got != want)
        Printf((STRPTR)"     sum got $%08lx want $%08lx (tail %ld)\n",
               (LONG)got, (LONG)want, (LONG)(len & 3UL));
    ck("fused sum", len, portval, (BOOL)(got == want));
}

static VOID one(ULONG len, UWORD portval)
{
    UBYTE *buf;

    buf = (UBYTE *)AllocMem(ARENA, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL)
    {
        Printf((STRPTR)"FAIL: out of memory at len %ld\n", (LONG)len);
        failures++;
        return;
    }

    one_into(buf, len, portval, "drained bytes");

    FreeMem(buf, ARENA);
}

static VOID pair(ULONG len1, ULONG len2)
{
    UBYTE *buf;

    buf = (UBYTE *)AllocMem(ARENA, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL)
    {
        Printf((STRPTR)"FAIL: out of memory at pair %ld/%ld\n",
               (LONG)len1, (LONG)len2);
        failures++;
        return;
    }

    one_into(buf, len1, 0xC3D9u, "pair first");
    one_into(buf, len2, 0x5A16u, "pair second");

    FreeMem(buf, ARENA);
}

static VOID live(ULONG len)
{
    volatile UWORD *vhposr = (volatile UWORD *)0xDFF006UL;
    UBYTE  *buf;
    UBYTE  *dst;
    ULONG   got;
    ULONG   want;
    ULONG   whole;
    ULONG   tail;
    BOOL    guards_ok = TRUE;
    ULONG   i;

    buf = (UBYTE *)AllocMem(ARENA, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL)
    {
        Printf((STRPTR)"FAIL: out of memory at live %ld\n", (LONG)len);
        failures++;
        return;
    }

    for (i = 0UL; i < ARENA; i++)
        buf[i] = (UBYTE)GUARD;

    dst = buf + PRE;
    got = n68k_port_in_w_sum(dst, (const volatile void *)vhposr, len);

    want  = 0UL;
    whole = len & ~3UL;
    tail  = len & 3UL;

    for (i = 0UL; i < whole; i += 4UL)
    {
        ULONG w = ((ULONG)dst[i]     << 24) | ((ULONG)dst[i + 1] << 16) |
                  ((ULONG)dst[i + 2] <<  8) |  (ULONG)dst[i + 3];

        want += w;
        if (want < w)
            want++;
    }

    if (tail != 0UL)
    {
        ULONG w = 0UL;

        for (i = 0UL; i < tail; i++)
            w |= (ULONG)dst[whole + i] << (24 - (8 * (int)i));

        want += w;
        if (want < w)
            want++;
    }

    for (i = 0UL; i < PRE; i++)
        if (buf[i] != (UBYTE)GUARD)
            guards_ok = FALSE;
    for (i = len; i < len + 4UL; i++)
        if (dst[i] != (UBYTE)GUARD)
            guards_ok = FALSE;

    ck("live guards", len, 0xD006u, guards_ok);

    if (got != want)
        Printf((STRPTR)"     live sum got $%08lx, the bytes say $%08lx\n",
               (LONG)got, (LONG)want);
    ck("live sum consistent", len, 0xD006u, (BOOL)(got == want));

    FreeMem(buf, ARENA);
}

/* ------------------------------------------- the 32-bit mirrored window --- */

/*
 * n68k_port_in_l_sum() takes a longword count and has no tail: the 1..3 bytes
 * that do not fill a longword come off the word port, which is the caller's
 * business.  So the reference here is over whole longwords only.
 */
static ULONG ref_lsum(ULONG longs, ULONG portval)
{
    ULONG sum = 0UL;

    while (longs-- != 0UL)
    {
        sum += portval;
        if (sum < portval)
            sum++;                      /* end-around carry, per add */
    }

    return sum;
}

static VOID one_long(ULONG longs, ULONG portval)
{
    static volatile ULONG port;
    UBYTE  *buf;
    UBYTE  *dst;
    ULONG   bytes = longs << 2;
    ULONG   got;
    ULONG   want;
    BOOL    bytes_ok = TRUE;
    ULONG   i;

    buf = (UBYTE *)AllocMem(ARENA, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL)
    {
        Printf((STRPTR)"FAIL: out of memory at longs %ld\n", (LONG)longs);
        failures++;
        return;
    }

    for (i = 0UL; i < ARENA; i++)
        buf[i] = (UBYTE)GUARD;

    /* PRE is 4, so the destination is longword aligned, which is what the
       routine requires and what the receive slot's payload pointer is. */
    dst  = buf + PRE;
    port = portval;

    got  = n68k_port_in_l_sum(dst, (const volatile void *)&port, longs);
    want = ref_lsum(longs, portval);

    for (i = 0UL; i < PRE; i++)
    {
        if (buf[i] != (UBYTE)GUARD)
            bytes_ok = FALSE;
    }

    for (i = 0UL; i < bytes; i += 4UL)
    {
        if (*(const ULONG *)(const APTR)(dst + i) != portval)
            bytes_ok = FALSE;
    }

    for (i = bytes; i < bytes + 4UL; i++)
    {
        if (dst[i] != (UBYTE)GUARD)
            bytes_ok = FALSE;
    }

    ck("long drained bytes", bytes, (ULONG)(portval >> 16), bytes_ok);

    if (got != want)
        Printf((STRPTR)"     long sum got $%08lx want $%08lx (longs %ld)\n",
               (LONG)got, (LONG)want, (LONG)longs);
    ck("long fused sum", bytes, (ULONG)(portval >> 16),
       (BOOL)(got == want));

    FreeMem(buf, ARENA);
}

int main(void)
{
    static const ULONG lports[] = { 0x12345678UL, 0xFF01FF01UL, 0x80000000UL,
                                    0x00000001UL, 0xFFFFFFFFUL, 0xABABABABUL };
    static const UWORD ports[] = { 0x1234u, 0xFF01u, 0x8000u, 0x0001u,
                                   0xFFFFu, 0xABABu };
    /* Every tail residue at several magnitudes, and the two sizes this bug was
       found at: 331 is a DHCP offer's payload, 46 the shortest Ethernet
       payload there is. */
    static const ULONG lens[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8,
                                  15, 16, 17, 18, 19,
                                  31, 32, 33, 34, 35,
                                  46, 100, 101, 102, 103,
                                  328, 331, 511, 512, 515,
                                  1458, 1459, 1460, 1461, 1462, 1463, 1514 };
    ULONG p;
    ULONG l;
    ULONG r1;
    ULONG r2;

    for (p = 0UL; p < (ULONG)(sizeof(ports) / sizeof(ports[0])); p++)
    {
        for (l = 0UL; l < (ULONG)(sizeof(lens) / sizeof(lens[0])); l++)
            one(lens[l], ports[p]);
    }

    for (r1 = 0UL; r1 < 4UL; r1++)
    {
        for (r2 = 0UL; r2 < 4UL; r2++)
        {
            pair(32UL + r1, 32UL + r2);
            pair(1460UL + r1, 328UL + r2);
        }
    }

    for (l = 0UL; l < 4UL; l++)
    {
        live(64UL + l);
        live(331UL + l);
        live(1460UL + l);
    }

    /* The mirrored-window form.  The lengths are the whole-longword parts of
       the same frame sizes: 0, one, the four-longword block boundary either
       side, and a full Ethernet payload. */
    for (p = 0UL; p < (ULONG)(sizeof(lports) / sizeof(lports[0])); p++)
    {
        static const ULONG longs[] = { 0, 1, 2, 3, 4, 5, 7, 8, 9,
                                       11, 25, 82, 128, 364, 365, 375 };

        for (l = 0UL; l < (ULONG)(sizeof(longs) / sizeof(longs[0])); l++)
            one_long(longs[l], lports[p]);
    }

    Printf((STRPTR)"%ld checks, %ld failures\n", (LONG)checks, (LONG)failures);

    return failures != 0UL ? RETURN_ERROR : RETURN_OK;
}
