/*
 * AmiNetXDuo, calls into code another program supplied.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_FOREIGN_CALL_H
#define AMINETXDUO_FOREIGN_CALL_H

#include <exec/types.h>
#include <utility/hooks.h>
#include <stddef.h>

/*
 * A Hook, a SANA-II buffer function, an SBTC_FDCALLBACK: the opener wrote
 * it, in whatever language, and owes back d2-d7/a2-a6.  Not every opener
 * pays.  WirelessManager 1.5 hands in
 *
 *     exg a0,a1; move.l 4.w,a6; jsr CopyMem(a6); moveq #1,d0; rts
 *
 * for S2_CopyToBuff and S2_CopyFromBuff, which returns with SysBase in a6,
 * and GCC keeps live values in a6 across a call it trusts.  So every such
 * call goes through one of these, which treat a6 as clobbered beside
 * d0/d1/a0/a1.  utility.library's CallHookPkt() is no shield: in ROM 40.68
 * it is `move.l 8(a0),-(sp); rts`, the hook returns straight to the caller.
 *
 * Off m68k (host tests) each is a plain C call.
 */

#if defined(__m68k__) && defined(__GNUC__)

/* A utility.library Hook: a0 = hook, a2 = object, a1 = message, result in
   d0.  h_Entry goes through a6, which the call gives up anyway. */
static inline ULONG ami_call_hook(struct Hook *hook, APTR object, APTR message)
{
    register struct Hook *_a0 __asm("a0") = hook;
    register APTR  _a2 __asm("a2") = object;
    register APTR  _a1 __asm("a1") = message;
    register ULONG _d1 __asm("d1");
    register ULONG res __asm("d0");

    __asm __volatile ("movel %1@(%c5),%%a6\n\tjsr %%a6@"
                      : "=r" (res), "+r" (_a0), "+r" (_a1), "+r" (_a2),
                        "=r" (_d1)
                      : "i" (offsetof(struct Hook, h_Entry))
                      : "a6", "cc", "memory");

    return res;
}

/* The same, through an h_Entry the caller read when it chose. */
static inline ULONG ami_call_hook_entry(ULONG (*entry)(VOID), struct Hook *hook,
                                        APTR object, APTR message)
{
    register struct Hook *_a0 __asm("a0") = hook;
    register APTR  _a2 __asm("a2") = object;
    register APTR  _a1 __asm("a1") = message;
    register ULONG _d1 __asm("d1");
    register ULONG res __asm("d0");

    __asm __volatile ("jsr %5@"
                      : "=r" (res), "+r" (_a0), "+r" (_a1), "+r" (_a2),
                        "=r" (_d1)
                      : "a" (entry)
                      : "a6", "cc", "memory");

    return res;
}

/* S2_CopyToBuff / S2_CopyFromBuff (and the 16-bit pair): a0 = to,
   a1 = from, d0 = length, result in d0.  A register-annotated function-
   pointer typedef miscompiles here: GCC loads the pointer into a0 and
   destroys the first argument. */
static inline ULONG ami_call_copy(APTR fn, APTR to, APTR from, ULONG len)
{
    register APTR  _a0 __asm("a0") = to;
    register APTR  _a1 __asm("a1") = from;
    register ULONG _d0 __asm("d0") = len;
    register ULONG _d1 __asm("d1");

    __asm __volatile ("jsr %4@"
                      : "+r" (_d0), "+r" (_a0), "+r" (_a1), "=r" (_d1)
                      : "a" (fn)
                      : "a6", "cc", "memory");

    return _d0;
}

/* SBTC_FDCALLBACK: d0 = fd, d1 = action, result in d0. */
static inline LONG ami_call_fd(APTR fn, LONG fd, LONG action)
{
    register LONG _d0 __asm("d0") = fd;
    register LONG _d1 __asm("d1") = action;
    register APTR _a0 __asm("a0");
    register APTR _a1 __asm("a1");

    __asm __volatile ("jsr %4@"
                      : "+r" (_d0), "+r" (_d1), "=r" (_a0), "=r" (_a1)
                      : "a" (fn)
                      : "a6", "cc", "memory");

    return _d0;
}

#else

/* Through APTR, so the cast is not one function type to another. */
static inline ULONG ami_call_hook_entry(ULONG (*entry)(VOID), struct Hook *hook,
                                        APTR object, APTR message)
{
    return ((ULONG (*)(struct Hook *, APTR, APTR))(APTR)entry)(hook, object,
                                                              message);
}

static inline ULONG ami_call_copy(APTR fn, APTR to, APTR from, ULONG len)
{
    return ((ULONG (*)(APTR, APTR, ULONG))fn)(to, from, len);
}

static inline LONG ami_call_fd(APTR fn, LONG fd, LONG action)
{
    return ((LONG (*)(LONG, LONG))fn)(fd, action);
}

static inline ULONG ami_call_hook(struct Hook *hook, APTR object, APTR message)
{
    return ami_call_hook_entry(hook->h_Entry, hook, object, message);
}

#endif

#endif /* AMINETXDUO_FOREIGN_CALL_H */
