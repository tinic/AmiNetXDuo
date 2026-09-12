/*
 * How much of a Shell's stack a command actually touches.
 *
 * A Shell gives a command 4096 bytes and AmigaOS has no MMU, so an overrun
 * corrupts and does not trap.  tools/check-stack-frames.sh bounds what each
 * command's own frames want, from -fstack-usage.  It cannot see across the LVO
 * boundary into bsdsocket.library, and a command reaches the library on its
 * OWN stack -- so the static figure is a floor, not the answer.
 *
 * THE STATIC SUM IS PESSIMISTIC AND THE STATIC PART IS INCOMPLETE, which is
 * why this exists rather than arithmetic.  Adding a command's worst case to a
 * library budget assumes both worst paths coincide, and OpenLibrary() is
 * called near the top of main() where the stack is shallow.  The only honest
 * number is what a real command on a real Shell stack touched, and that is
 * what this measures.
 *
 * Off unless AMINETXDUO_STACKPROBE.  tool_startup.S paints before main() and
 * reports after it, so the mark covers everything main() reached, library
 * calls included.
 *
 * The method is tests/stack/stack_test.c's, which measures the same thing for
 * the library API from a stack it owns.
 *
 * SPDX-License-Identifier: MIT
 */

/* tools.h and nothing else, in that order: it pulls tx_api.h in before
   <exec/types.h>, and putting an exec header first instead collides ThreadX's
   VOID with the NDK's. */
#include "tools.h"

#include <exec/tasks.h>

/* Unlikely as a value and obvious in a dump. */
#define TSP_PATTERN     0xA5C3A5C3UL

/*
 * Painting stops this far below the painter's own frame.  Too small and the
 * paint loop writes over its own locals; 256 is what stack_test.c uses and it
 * has held there.
 */
#define TSP_MARGIN      256

VOID ami_tool_stack_paint(VOID)
{
    struct Task *me = FindTask(NULL);
    ULONG        here;
    ULONG       *lo;
    ULONG       *stop;
    ULONG       *p;

    if (me == NULL)
        return;

    lo   = (ULONG *)me->tc_SPLower;
    stop = (ULONG *)(((ULONG)&here) - TSP_MARGIN);

    if (lo == NULL || stop <= lo)
        return;

    for (p = lo; p < stop; p++)
        *p = TSP_PATTERN;
}

VOID ami_tool_stack_report(VOID)
{
    struct Task *me = FindTask(NULL);
    ULONG       *lo;
    ULONG       *hi;
    ULONG       *p;
    ULONG        stack;
    ULONG        mark;

    if (me == NULL)
        return;

    lo = (ULONG *)me->tc_SPLower;
    hi = (ULONG *)me->tc_SPUpper;

    if (lo == NULL || hi == NULL || hi <= lo)
        return;

    stack = (ULONG)((UBYTE *)hi - (UBYTE *)lo);

    /* The first word the command left alone is the deepest it ever went. */
    for (p = lo; p < hi; p++)
        if (*p != TSP_PATTERN)
            break;

    mark = (ULONG)((UBYTE *)hi - (UBYTE *)p);

    /*
     * One line, keyed so a harness can grep it out of a transcript that also
     * holds the command's own output.  spare is what a deeper call still had:
     * that, not the mark, is the number that says whether this is close.
     *
     * VPrintf() and not tool_say(): this file is linked by everything that
     * uses tool_startup.S, and tests/perf's rtgmodes, rtgbars and rtdiv take
     * the stub WITHOUT tool_util.c.  A probe that drags the command library
     * in behind it cannot go in every startup, which is the one place it has
     * to be.  DOSBase is open by here -- the stub opened it before main().
     */
    {
        ULONG args[3];

        args[0] = stack;
        args[1] = mark;
        args[2] = stack - mark;

        VPrintf((CONST_STRPTR)"stackprobe: stack=%lu mark=%lu spare=%lu\n",
                (APTR)args);
    }
}
