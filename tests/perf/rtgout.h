/*
 * Output for the RTG test tools, through dos.library and nothing else.
 *
 * NOT stdio, and that is the whole point of this file.  tests/perf/rtgscreen.c
 * printed through newlib and linked the toolchain crt0, and on the console
 * harness's RTG guest it died before its first line reached the file it was
 * redirected into: an illegal instruction at a garbage PC in low memory, at
 * the same address every run, with the boot stopping there and everything the
 * Startup-Sequence starts after it -- the server included -- never reached.
 * From outside that reads as a guest that never booted.  A 32 KB stack made no
 * difference and the toolchain's crt0 reports immune to both known defects.
 *
 * So these tools do what every command under src/tools does: they link
 * src/tools/tool_startup.S under -nostartfiles and print with VPrintf().  That
 * is the combination that demonstrably runs on this guest -- httpd is started
 * three lines further down the same Startup-Sequence -- and it costs nothing
 * to match it.
 *
 * VPrintf() takes RawDoFmt's data stream: one flat array of 32-bit words, not
 * C varargs.  Callers build the array, which is clumsier to read and has no
 * promotion rules to get wrong.  Flushed on every line because a tool that
 * crashes with its last line still in a buffer has told nobody anything, which
 * is the failure this file exists because of.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_RTGOUT_H
#define AMINETXDUO_RTGOUT_H

#include <exec/types.h>
#include <proto/dos.h>

/* Where the lines go.  Zero is Output(), which is what a tool run in the
   foreground with a redirection wants.  A tool that NEVER RETURNS cannot use
   one: `Run >file` gave the file to Run and the detached command's own output
   went nowhere, so the harness read back the four characters Run printed and
   nothing the tool said.  Such a tool opens its own file, writes its report,
   and closes it -- a closed file is the only kind whose contents are certainly
   all there. */
static BPTR rtg_out_fh = (BPTR)0;

static VOID rtg_say(const char *fmt, const ULONG *args)
{
    BPTR fh = (rtg_out_fh != (BPTR)0) ? rtg_out_fh : Output();

    VFPrintf(fh, (CONST_STRPTR)fmt, (APTR)args);
    Flush(fh);
}

#endif /* AMINETXDUO_RTGOUT_H */
