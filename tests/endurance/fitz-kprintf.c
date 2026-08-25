/*
 * kprintf() and mysnprintf() for Fitz, in C -- the originals are vasm source
 * this tree cannot assemble.  Same semantics, including mysnprintf()'s non-C99
 * return value.  Compiled only into the harness's Fitz build.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <proto/exec.h>
#include <inline/macros.h>

#include <stdarg.h>
#include <stddef.h>

/* RawPutChar is exec LVO -516; the NDK declares it only for the assembler. */
#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

/* ------------------------------------------------- 64-bit division ------- */

/* libgcc.a for this target has neither __udivdi3 nor __umoddi3; Fitz's
   ds_to_unix() needs one. */
unsigned long long __udivdi3(unsigned long long n, unsigned long long d);
unsigned long long __umoddi3(unsigned long long n, unsigned long long d);

/* Must use 32-bit limbs: a variable-count 64-bit shift compiles to
   __lshrdi3/__ashldi3, which this libgcc.a also lacks. */
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

/* Counts bytes actually written, not the length needed: Fitz advances write
   cursors by the return value, so a C99 return would walk off the end. */
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
