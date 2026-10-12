/*
 * WaitBreak: waits for Ctrl-C and does nothing else, in about a hundred
 * bytes with no C runtime.  run-netwireless.sh starts it the way it starts
 * NetWireless, so the Shell and stack around both can be told apart from
 * the commodity's own memory.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>

int waitbreak(void) __attribute__((section(".text.0start")));

int waitbreak(void)
{
    struct ExecBase *SysBase = *(struct ExecBase **)4;

    Wait(SIGBREAKF_CTRL_C);
    return 0;
}
