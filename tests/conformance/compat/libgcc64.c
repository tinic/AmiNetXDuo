/*
 * 64-bit division helpers for the conformance build.
 *
 * This toolchain ships a zero-byte libgcc.a (lib/gcc/m68k-amigaos/15.2.0/
 * libgcc.a), and nothing in libc.a, libnix.a or libamiga.a defines the
 * 64-bit divide routines.  Every newlib printf variant references them
 * (vfprintf/vfiprintf use __udivdi3 and __umoddi3 for %lld and for the
 * float paths), so anything that calls printf() fails to link:
 *
 *   lib_a-vfiprintf.o: undefined reference to `__udivdi3'
 *
 * AmiNetXDuo's own code never hits this because it does its own formatting
 * over dos.library.  bsdsocktest is stock C and uses printf throughout, so
 * the helpers have to come from somewhere: here.
 *
 * Textbook restoring shift-subtract division -- correctness matters, speed
 * does not; these only ever run inside printf.
 *
 * SPDX-License-Identifier: MIT
 */

typedef unsigned long long UDItype;
typedef long long          DItype;

UDItype __udivmoddi4(UDItype num, UDItype den, UDItype *rem);
UDItype __udivdi3(UDItype num, UDItype den);
UDItype __umoddi3(UDItype num, UDItype den);
DItype  __divdi3(DItype num, DItype den);
DItype  __moddi3(DItype num, DItype den);

UDItype __udivmoddi4(UDItype num, UDItype den, UDItype *rem)
{
    UDItype quot = 0;
    UDItype qbit = 1;

    if (den == 0)
    {
        /* Undefined; mirror libgcc and let the hardware raise it. */
        if (rem)
            *rem = 0;
        return 0;
    }

    /* Left-justify the divisor, remembering how far by shifting qbit with it. */
    while ((DItype)den >= 0)
    {
        den <<= 1;
        qbit <<= 1;
    }

    while (qbit != 0)
    {
        if (den <= num)
        {
            num -= den;
            quot += qbit;
        }
        den >>= 1;
        qbit >>= 1;
    }

    if (rem)
        *rem = num;

    return quot;
}

UDItype __udivdi3(UDItype num, UDItype den)
{
    return __udivmoddi4(num, den, 0);
}

UDItype __umoddi3(UDItype num, UDItype den)
{
    UDItype rem = 0;

    __udivmoddi4(num, den, &rem);
    return rem;
}

DItype __divdi3(DItype num, DItype den)
{
    int      neg = 0;
    UDItype  q;

    if (num < 0) { num = -num; neg ^= 1; }
    if (den < 0) { den = -den; neg ^= 1; }

    q = __udivmoddi4((UDItype)num, (UDItype)den, 0);

    return neg ? -(DItype)q : (DItype)q;
}

DItype __moddi3(DItype num, DItype den)
{
    int     neg = 0;
    UDItype r   = 0;

    if (num < 0) { num = -num; neg = 1; }
    if (den < 0) { den = -den; }

    __udivmoddi4((UDItype)num, (UDItype)den, &r);

    return neg ? -(DItype)r : (DItype)r;
}
