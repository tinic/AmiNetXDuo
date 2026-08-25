/*
 * Output for the RTG test tools, through dos.library and nothing else.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_RTGOUT_H
#define AMINETXDUO_RTGOUT_H

#include <exec/types.h>
#include <proto/dos.h>

static BPTR rtg_out_fh = (BPTR)0;

static VOID rtg_say(const char *fmt, const ULONG *args)
{
    BPTR fh = (rtg_out_fh != (BPTR)0) ? rtg_out_fh : Output();

    VFPrintf(fh, (CONST_STRPTR)fmt, (APTR)args);
    Flush(fh);
}

#endif /* AMINETXDUO_RTGOUT_H */
