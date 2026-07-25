/* Validates the crash guard by crashing on purpose.
   SPDX-License-Identifier: MIT */
#include <proto/exec.h>
#include <proto/dos.h>
#include "aminetxduo/compat.h"
#include "aminetxduo/crashguard.h"

static LONG checks, failures;

static void check(const char *what, BOOL ok)
{
    checks++;
    if (!ok) failures++;
    Printf("  %s %s\n", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
}

int main(void)
{
    const AmiCrashInfo *info;
    void (*boom)(void) = (void (*)(void))0x00000002;  /* jump into low memory */

    ami_crash_set_reference((APTR)main, "main");

    if (ami_crash_install())
    {
        AMI_ERROR("crashtest: about to jump to 0x2 on purpose");
        boom();
        check("unreachable -- crash was not caught", FALSE);
    }
    else
    {
        check("crash guard caught the exception and resumed", TRUE);
    }
    ami_crash_remove();

    info = ami_crash_info();
    check("crash info recorded", info != NULL);
    if (info)
    {
        Printf("  caught: %s (exception %ld) at PC=%08lx\n",
               (LONG)ami_crash_name(info->number), (LONG)info->number,
               (LONG)info->pc);
        check("exception number is a CPU fault", info->number >= 2 && info->number <= 11);
    }

    Printf("\n%ld checks, %ld failure(s)\n", checks, failures);
    return failures == 0 ? RETURN_OK : RETURN_ERROR;
}
