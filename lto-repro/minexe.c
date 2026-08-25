/*
 * The same ABI constructs in a plain executable, so a failure can be told
 * apart from anything specific to the -nostartfiles library link.
 *
 * It calls the register-argument vectors THROUGH the table, the way exec's
 * generated JMP stubs do, and prints what it got.  Correct output is one line:
 *
 *   add=10 sub=4 ptr=6 mix=6 asm=6 ok
 */
#include <stdio.h>

#include "minlib.h"

typedef LONG (*Fn2)(register LONG a __asm("d0"), register LONG b __asm("d1"),
                    register struct Library *base __asm("a6"));
typedef LONG (*Fn2r)(register LONG a __asm("d1"), register LONG b __asm("d0"),
                     register struct Library *base __asm("a6"));
typedef LONG (*FnP)(register const char *p __asm("a0"), register LONG n __asm("d0"),
                    register struct Library *base __asm("a6"));

/* Not const, and written at run time, so nothing can fold the calls away. */
static const char buf[3] = { 1, 2, 3 };

int main(void)
{
    struct Library *fake = (struct Library *)0;
    LONG add, sub, ptr, mix, asum;
    int ok = 1;

    add  = ((Fn2) MinVectorTable[4])(4, 6, fake);
    sub  = ((Fn2r)MinVectorTable[5])(7, 3, fake);
    ptr  = ((FnP) MinVectorTable[6])(buf, 3, fake);
    mix  = min_mix(buf, buf, 3, 3, fake);
    asum = min_asm_sum((const UBYTE *)buf, 3);

    if (add != 10)  { ok = 0; }
    if (sub != 4)   { ok = 0; }
    if (ptr != 6)   { ok = 0; }
    if (mix != 6)   { ok = 0; }
    if (asum != 6)  { ok = 0; }

    printf("add=%ld sub=%ld ptr=%ld mix=%ld asm=%ld %s\n",
           (long)add, (long)sub, (long)ptr, (long)mix, (long)asum,
           ok ? "ok" : "MISMATCH");

    return ok ? 0 : 1;
}
