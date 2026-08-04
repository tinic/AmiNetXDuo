/*
 * kprintf() and mysnprintf() for Fitz, in C.
 *
 * Fitz supplies both in `src/kprintf.asm`, which is vasm source (`XDEF`,
 * `include "lvo/exec_lib.i"`).  This tree has no vasm, and Fitz is built from
 * source here so that a debug build prints
 *
 *     * EAGAIN
 *     * recv error err=-1 len=... errno=...
 *
 * on the serial port, which tools/fsuae-run.sh already captures.  Those two
 * lines are the direct evidence for the defect this harness hunts; without
 * them a lost connection is only ever an inference.
 *
 * So the two entry points are reimplemented here, on the same Exec primitive
 * the original uses (RawDoFmt), with the same semantics, including
 * mysnprintf()'s non-C99 return value, which Fitz's own header calls out
 * ("does not allow str=NULL, size=0") and which its callers rely on.
 *
 * Compiled only into the harness's Fitz build; nothing in src/ sees it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <proto/exec.h>

#include <stdarg.h>
#include <stddef.h>

/* RawPutChar is exec LVO -516; the NDK declares it only for the assembler. */
#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

/* ------------------------------------------------- 64-bit division ------- */

/*
 * `__udivdi3` and `__umoddi3` are absent from this toolchain's libgcc.a,
 * checked with `nm`, which finds neither in any archive it ships.  Fitz's
 * `ds_to_unix()` (src/amiga-common.c) needs one: it composes a Unix timestamp
 * in `uint64_t` and divides the tick field by 50 there.
 *
 * Supplied here rather than by editing Fitz, so the harness tests the released
 * program unmodified.
 *
 * Restoring-shift division: 64 iterations, no multiply, so it is correct on a
 * 68000 as well.  It runs once per timestamp conversion and is nowhere near
 * any measured path.
 */
unsigned long long __udivdi3(unsigned long long n, unsigned long long d);
unsigned long long __umoddi3(unsigned long long n, unsigned long long d);

/*
 * Written on 32-bit limbs rather than on `unsigned long long` directly,
 * because a variable-count 64-bit shift compiles to `__lshrdi3`/`__ashldi3`
 *, which this libgcc.a does not have either, so the obvious version of this
 * function fails to link for the same reason it exists.
 */
typedef struct
{
    ULONG   hi;
    ULONG   lo;
} Fitz64;

static Fitz64 fitz_split(unsigned long long v)
{
    Fitz64 r;

    r.hi = (ULONG)(v >> 32);
    r.lo = (ULONG)v;

    return r;
}

static unsigned long long fitz_join(Fitz64 v)
{
    return ((unsigned long long)v.hi << 32) | (unsigned long long)v.lo;
}

static Fitz64 fitz_divmod(unsigned long long nn, unsigned long long dd,
                          Fitz64 *rem)
{
    Fitz64 n = fitz_split(nn);
    Fitz64 d = fitz_split(dd);
    Fitz64 q, r;
    int    i;

    q.hi = 0UL; q.lo = 0UL;
    r.hi = 0UL; r.lo = 0UL;

    if (d.hi == 0UL && d.lo == 0UL)
    {
        if (rem != NULL)
            *rem = r;
        return q;                       /* division by zero */
    }

    for (i = 63; i >= 0; i--)
    {
        ULONG bit = (i >= 32) ? ((n.hi >> (i - 32)) & 1UL)
                              : ((n.lo >> i) & 1UL);

        r.hi = (r.hi << 1) | (r.lo >> 31);
        r.lo = (r.lo << 1) | bit;

        if (r.hi > d.hi || (r.hi == d.hi && r.lo >= d.lo))
        {
            ULONG borrow = (r.lo < d.lo) ? 1UL : 0UL;

            r.lo -= d.lo;
            r.hi -= d.hi + borrow;

            if (i >= 32)
                q.hi |= (1UL << (i - 32));
            else
                q.lo |= (1UL << i);
        }
    }

    if (rem != NULL)
        *rem = r;

    return q;
}

unsigned long long __udivdi3(unsigned long long n, unsigned long long d)
{
    return fitz_join(fitz_divmod(n, d, NULL));
}

unsigned long long __umoddi3(unsigned long long n, unsigned long long d)
{
    Fitz64 r;

    (VOID)fitz_divmod(n, d, &r);

    return fitz_join(r);
}

/* --------------------------------------------------------------- kprintf -- */

static VOID kp_put(register UBYTE c __asm("d0"),
                   register APTR unused __asm("a3"))
{
    (VOID)unused;

    if (c != '\0')
        RawPutChar(c);
}

void kprintf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)())kp_put, NULL);
    va_end(args);
}

/* ------------------------------------------------------------ mysnprintf -- */

typedef struct
{
    ULONG   sn_Count;
    ULONG   sn_Len;
    char   *sn_Ptr;
} SnCtx;

/*
 * The original stops incrementing once the buffer is full, so the count is
 * the number of bytes actually written, not the length that would have been
 * needed.  Mirrored exactly: Fitz uses the return value to advance write
 * cursors, and a C99 return would walk them off the end of a truncated
 * buffer.
 */
static VOID sn_put(register UBYTE c __asm("d0"),
                   register SnCtx *ctx __asm("a3"))
{
    if (ctx->sn_Count < ctx->sn_Len)
    {
        ctx->sn_Ptr[ctx->sn_Count] = (char)c;
        ctx->sn_Count++;
    }
}

int mysnprintf(const char *out, size_t osize, ...)
{
    SnCtx       ctx;
    va_list     args;
    const char *fmt;

    if (out == NULL || osize == 0)
        return 0;

    ctx.sn_Count = 0UL;
    ctx.sn_Len   = (ULONG)osize;
    ctx.sn_Ptr   = (char *)out;

    va_start(args, osize);
    fmt = va_arg(args, const char *);
    RawDoFmt((STRPTR)fmt, args, (void (*)())sn_put, &ctx);
    va_end(args);

    /* RawDoFmt writes the terminating NUL, so the count includes it. */
    if (ctx.sn_Count == 0UL)
    {
        ((char *)out)[0] = '\0';
        return 0;
    }

    ((char *)out)[ctx.sn_Count - 1UL] = '\0';

    return (int)(ctx.sn_Count - 1UL);
}
