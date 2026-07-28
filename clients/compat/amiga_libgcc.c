/*
 * clients/compat -- the libgcc helpers this toolchain's ZERO BYTE libgcc.a
 * does not supply, beyond the 64-bit division already in src/common.
 *
 *   $AMIGA_TOOLCHAIN_ROOT/lib/gcc/m68k-amigaos/15.2.0/libgcc.a is 0 bytes.
 *   src/common/ami_udivdi3.c covers __udivdi3 / __umoddi3 / __divdi3 /
 *   __moddi3 / __udivmoddi4, which is everything the stack itself and
 *   newlib's printf need.  A ported Unix client reaches further:
 *
 *       __ctzdi2         curl's bitset scan over a 64-bit mask
 *       __popcountdi2    ditto
 *       __floatdidf      curl_off_t -> double, the progress meter
 *       __fixdfdi        double -> curl_off_t, --max-filesize and friends
 *       __atomic_exchange_4   the once-only guard in curl_global_init()
 *
 *   This file is those five and nothing else.  It deliberately does NOT
 *   redefine the division helpers: there is one copy of those in the tree
 *   (src/common/ami_udivdi3.c) and clients/amiga-client.sh compiles that same
 *   file rather than keeping a second.
 *
 *   __floatdidf and __fixdfdi are written in C over double arithmetic, which
 *   on this toolchain means libc.a's __adddf3 / __muldf3 / ..., which call
 *   mathieeedoubbas.library.  That is not a new dependency -- any client that
 *   touches a double already has it -- but it is worth knowing that these two
 *   do not escape it.  mathieeedoubbas.library is NOT in Kickstart 3.1 ROM
 *   (checked: the 40.68 A1200 image contains mathieeesingbas and no other),
 *   so it has to be in LIBS: on the machine.  Every Workbench install has it.
 *
 * __atomic_exchange_4 And why it is Disable()
 *
 *   The 68020 has CAS, so a lock-free version is possible.  It is not worth
 *   it: this is called once per curl_global_init() on a machine with one CPU
 *   and no memory ordering to speak of, and Disable() is four instructions.
 *   What it does have to be is safe against an INTERRUPT touching the same
 *   word, which Forbid() would not give.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <proto/exec.h>

typedef unsigned long long u64;
typedef long long          s64;
typedef unsigned long      u32;

int __ctzdi2(u64 x);
int __popcountdi2(u64 x);
double __floatdidf(s64 x);
s64 __fixdfdi(double x);
u32 __atomic_exchange_4(volatile void *ptr, u32 val, int memorder);


/* Count trailing zeros.  GCC's contract: undefined for x == 0, and every
   caller in a client guards it, but returning 64 is cheaper than a comment
   about it. */
int __ctzdi2(u64 x)
{
    u32 lo = (u32)x;
    u32 hi = (u32)(x >> 32);
    u32 w;
    int n;

    if (lo != 0)
    {
        w = lo;
        n = 0;
    }
    else if (hi != 0)
    {
        w = hi;
        n = 32;
    }
    else
    {
        return 64;
    }

    while ((w & 1UL) == 0UL)
    {
        w >>= 1;
        n++;
    }

    return n;
}

int __popcountdi2(u64 x)
{
    int n = 0;

    while (x != 0ULL)
    {
        x &= x - 1ULL;          /* clears the lowest set bit */
        n++;
    }

    return n;
}

/*
 * 64-bit signed integer -> double.
 *
 * Split rather than a single conversion because there is no 64-bit path in
 * the toolchain to call: the halves go through __floatsidf/__floatunsidf,
 * which libc.a does have.  2^32 is exact in a double, and so is each half, so
 * the only rounding is the final add -- which is what the hardware would do
 * as well.
 */
double __floatdidf(s64 x)
{
    s64  hi = x >> 32;
    u32  lo = (u32)x;

    return (double)(long)hi * 4294967296.0 + (double)lo;
}

/*
 * double -> 64-bit signed integer, truncating toward zero.
 *
 * Out of range is undefined behaviour in the C standard and GCC's own libgcc
 * saturates, so this saturates too: a client that asks --max-filesize 1e30
 * gets a clamp and not a random number.
 */
s64 __fixdfdi(double x)
{
    int  negative = 0;
    u32  hi;
    u32  lo;
    double t;

    if (x != x)                 /* NaN */
        return 0;

    if (x < 0.0)
    {
        negative = 1;
        x = -x;
    }

    if (x >= 9223372036854775808.0)     /* 2^63 */
        return negative ? (-9223372036854775807LL - 1LL) : 9223372036854775807LL;

    if (x < 1.0)
        return 0;

    t  = x / 4294967296.0;
    hi = (u32)(unsigned long)t;
    lo = (u32)(unsigned long)(x - (double)hi * 4294967296.0);

    {
        s64 v = (s64)(((u64)hi << 32) | (u64)lo);
        return negative ? -v : v;
    }
}

/*
 * The only atomic a curl or wget build reaches.  memorder is ignored: this
 * machine has one core, no store buffer and no cache coherency protocol, so
 * every ordering is the same ordering.
 */
u32 __atomic_exchange_4(volatile void *ptr, u32 val, int memorder)
{
    volatile u32 *p = (volatile u32 *)ptr;
    u32 old;

    (void)memorder;

    Disable();
    old = *p;
    *p  = val;
    Enable();

    return old;
}
