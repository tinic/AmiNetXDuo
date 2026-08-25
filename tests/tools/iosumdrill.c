/*
 * IoSumDrill: n68k_port_in_w_sum() against an independent reference, in the
 * guest, on the real instructions.
 *
 * The fused drain-and-sum is the one routine in the receive path that no test
 * reaches.  The host tests cannot run it -- it is 68k assembler -- and the
 * emulated NE2000 arm never calls it, because the DP8390 claim path drains
 * without summing and reports summed = 0.  Only the 3c589 asks for the fused
 * form, and the 3c589 exists in one machine.  So it shipped unexamined, and a
 * three-byte tail miscomputed the sum: `lsl.l #8` positioned the last byte
 * using a register whose upper half still held the previous word, and that
 * word ORed itself into the answer.  A payload of 331 bytes -- which is what a
 * DHCP offer on an ordinary LAN is -- has a three-byte tail, so every offer
 * was rejected by the receive verifier and no lease could ever be taken.
 *
 * WHAT IT ASSERTS
 *
 * The port is a word of RAM, so every read returns the same value and the
 * drained buffer is predictable.  That is enough: what varies here is the
 * LENGTH, and the tail cases are what the routine gets wrong.  For each length
 * the drill checks two things against a reference written from the
 * specification rather than from the code under test:
 *
 *   the bytes -- the drain must place exactly what the port handed over, and
 *     not one byte more, so a run past the end shows up as a poisoned guard
 *     byte rather than as nothing at all;
 *   the sum -- the 32-bit ones-complement longword sum with per-add
 *     end-around carry and a zero-padded, byte-positioned tail, which is what
 *     n68k_copy_sum_longwords() produces over the same bytes and what the
 *     verifier will check against.
 *
 * Several port values are used because a wrong answer can be right by accident
 * for one of them: 0x0000 sums to zero however it is assembled, and a value
 * whose halves are equal hides a swap.
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

/*
 * The reference, from the specification.  `port` is the word every read
 * returns, so byte i of the drained run is the high half of that word when i
 * is even and the low half when it is odd.
 */
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
                                /* an underrun is as much a placement bug   */
                                /* as an overrun, and buf[0] used to BE the */
                                /* destination, so nothing could see one.   */

/* The whole allocation: PRE guards, the destination, four tail guards. */
#define ARENA   (PRE + MAXLEN + 4UL)

/*
 * One drain into a guarded destination, verified byte for byte, both guard
 * bands, and the sum.  `buf` is the arena; the destination is buf + PRE,
 * which AllocMem keeps even, as the routine's contract requires.
 */
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

/*
 * Two drains back to back, every pair of tail residues, the second call
 * verified as completely as the first.  The three-byte-tail bug was a
 * register carrying stale state WITHIN a call; this is the same class one
 * level up -- anything the routine leaves behind that poisons the next call,
 * which is exactly the position el3_rint puts it in when a burst of frames
 * is drained in one interrupt.  Different port values, so a leak from the
 * first drain cannot be right by accident in the second.
 */
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

/*
 * The constant-port arms above have a blind spot by construction: every read
 * returns the same word, so a drain that read the port the wrong number of
 * times -- a word duplicated here, a word skipped there -- still writes the
 * expected bytes.  A LIVE port closes it: VHPOSR ($DFF006) is the video
 * beam's position and changes between reads, so consecutive words differ and
 * the check becomes SELF-consistency -- the returned sum must equal the
 * reference sum computed over whatever bytes actually landed in the buffer.
 * A sum that saw a word the buffer did not get, or missed one it did, cannot
 * match.  (The values are not predictable and do not need to be; ones'
 * complement addition commutes, so ordering is out of scope here and covered
 * by the constant arms' byte checks.)
 */
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

int main(void)
{
    static const UWORD ports[] = { 0x1234u, 0xFF01u, 0x8000u, 0x0001u,
                                   0xFFFFu, 0xABABu };
    /* Every tail residue at several magnitudes, the two sizes this bug was
       found at -- 331 is a DHCP offer's payload, 46 the shortest Ethernet
       payload there is -- and the MSS edges a TCP receive drains all day:
       1460 and its neighbours, and 1514, a full frame. */
    static const ULONG lens[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 32, 33, 34, 35,
                                  46, 100, 101, 102, 103, 328, 331, 512, 515,
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

    /* Back to back, every ordered pair of tail residues, at a small length
       (the register-reuse neighbourhood) and at the MSS. */
    for (r1 = 0UL; r1 < 4UL; r1++)
    {
        for (r2 = 0UL; r2 < 4UL; r2++)
        {
            pair(32UL + r1, 32UL + r2);
            pair(1460UL + r1, 328UL + r2);
        }
    }

    /* The live port, every residue at three magnitudes. */
    for (l = 0UL; l < 4UL; l++)
    {
        live(64UL + l);
        live(331UL + l);
        live(1460UL + l);
    }

    Printf((STRPTR)"%ld checks, %ld failures\n", (LONG)checks, (LONG)failures);

    return failures != 0UL ? RETURN_ERROR : RETURN_OK;
}
