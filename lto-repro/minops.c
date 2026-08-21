/*
 * TU 3: the vector implementations.  Register-argument functions, reached only
 * through the table in TU 2, one of them calling into hand-written assembly.
 */
#include "minlib.h"

struct Library *min_open(register struct Library *base __asm("a6"))
{
    base->lib_OpenCnt++;
    return base;
}

LONG min_close(register struct Library *base __asm("a6"))
{
    base->lib_OpenCnt--;
    return 0;
}

LONG min_expunge(register struct Library *base __asm("a6"))
{
    (void)base;
    return 0;
}

LONG min_reserved(register struct Library *base __asm("a6"))
{
    (void)base;
    return 0;
}

LONG min_add(register LONG a __asm("d0"),
             register LONG b __asm("d1"),
             register struct Library *base __asm("a6"))
{
    (void)base;
    return a + b;
}

/* Deliberately the other way round from min_add: d1 first, d0 second.  A
 * calling convention that has been "helpfully" normalised shows up here. */
LONG min_sub(register LONG a __asm("d1"),
             register LONG b __asm("d0"),
             register struct Library *base __asm("a6"))
{
    (void)base;
    return a - b;
}

LONG min_ptr(register const char *p __asm("a0"),
             register LONG n __asm("d0"),
             register struct Library *base __asm("a6"))
{
    LONG i, sum = 0;

    (void)base;
    if (p == NULL)
        return -1;
    for (i = 0; i < n; i++)
        sum += (LONG)(UBYTE)p[i];

    return sum;
}

LONG min_mix(register const char *p __asm("a0"),
             register const char *q __asm("a1"),
             register LONG n __asm("d0"),
             register LONG m __asm("d1"),
             register struct Library *base __asm("a6"))
{
    (void)base;
    return min_ptr(p, n, base) - min_ptr(q, m, base)
         + min_asm_sum((const UBYTE *)p, n);
}

LONG min_enosys(register struct Library *base __asm("a6"))
{
    (void)base;
    return -1;
}
