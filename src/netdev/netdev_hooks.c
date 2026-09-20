/*
 * AmiNetXDuo SANA-II callback ABI trampolines.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"

#include <utility/hooks.h>

/* Buffer-management hooks use a0 = to, a1 = from, d0 = len.  A register-
   annotated function-pointer typedef miscompiles here: GCC loads the pointer
   into a0 and destroys the first argument. */
BOOL netdev_copy_call(APTR fn, APTR to, APTR from, ULONG len)
{
    register APTR  _a2 __asm("a2") = fn;
    register APTR  _a0 __asm("a0") = to;
    register APTR  _a1 __asm("a1") = from;
    register ULONG _d0 __asm("d0") = len;
    register LONG  _d1 __asm("d1");
    register LONG  res __asm("d0");

    if (fn == NULL)
        return FALSE;

    __asm __volatile ("jsr a2@"
                      : "=r" (res), "=r" (_a0), "=r" (_a1), "=r" (_d1)
                      : "r" (_a2), "0" (_d0), "1" (_a0), "2" (_a1)
                      : "cc", "memory");

    return (BOOL)(res != 0);
}

/* A standard utility.library Hook: a0 = hook, a2 = object, a1 = message,
   result in d0.  h_Entry, not h_SubEntry. */
BOOL netdev_hook_call(APTR hook, APTR object, APTR message)
{
    register APTR _a3 __asm("a3");
    register APTR _a0 __asm("a0") = hook;
    register APTR _a2 __asm("a2") = object;
    register APTR _a1 __asm("a1") = message;
    register LONG _d1 __asm("d1");
    register LONG res __asm("d0");

    if (hook == NULL)
        return TRUE;

    _a3 = (APTR)((struct Hook *)hook)->h_Entry;

    __asm __volatile ("jsr a3@"
                      : "=r" (res), "=r" (_a0), "=r" (_a1), "=r" (_a2),
                        "=r" (_d1)
                      : "r" (_a3), "1" (_a0), "2" (_a1), "3" (_a2)
                      : "cc", "memory");

    return (BOOL)(res != 0);
}
