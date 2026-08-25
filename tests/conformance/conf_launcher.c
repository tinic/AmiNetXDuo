/*
 * AmiNetXDuo, bsdsocktest launcher.
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/crashguard.h"

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#define SUITE       "DH0:bsdsocktest"
#define ARGS_FILE   "DH0:conf-args"
#define OUT_FILE    "DH0:conf-out.txt"
#define CHILD_STACK 65536

static char cmdline[512];

static VOID read_args(VOID)
{
    BPTR  fh;
    LONG  n = 0;
    ULONG i;

    fh = Open((STRPTR)ARGS_FILE, MODE_OLDFILE);
    if (fh != 0)
    {
        n = Read(fh, cmdline, (LONG)sizeof(cmdline) - 1);
        Close(fh);
    }

    if (n < 0)
        n = 0;
    cmdline[n] = '\0';

    /* ReadArgs wants one line: stop at the first newline. */
    for (i = 0; cmdline[i] != '\0'; i++)
    {
        if (cmdline[i] == '\n' || cmdline[i] == '\r')
        {
            cmdline[i] = '\0';
            break;
        }
    }
}

int main(VOID)
{
    char  command[640];
    BPTR  out;
    BPTR  in;
    LONG  rc;

    read_args();

    strcpy(command, SUITE);
    if (cmdline[0] != '\0')
    {
        strcat(command, " ");
        strcat(command, cmdline);
    }

    Printf((STRPTR)"conf: %s\n", (LONG)command);

    out = Open((STRPTR)OUT_FILE, MODE_NEWFILE);
    if (out == 0)
    {
        PrintFault(IoErr(), (STRPTR)"conf: cannot open " OUT_FILE);
        return RETURN_FAIL;
    }

    in = Open((STRPTR)"NIL:", MODE_OLDFILE);

    ami_crash_set_reference((APTR)main, "conf_launcher");
    (VOID)ami_crash_install_alert_hook();
    (VOID)ami_crash_install();

    /*
     * SystemTagList() closes the handles it is given, so these must be ours
     * and must not be closed again here.  NP_StackSize reaches CreateNewProc
     * through the same tag list.
     */
    rc = SystemTags((STRPTR)command,
                    SYS_Input,     (ULONG)in,
                    SYS_Output,    (ULONG)out,
                    NP_StackSize,  CHILD_STACK,
                    TAG_DONE);

    ami_crash_remove();
    ami_crash_remove_alert_hook();

    if (rc < 0)
    {
        PrintFault(IoErr(), (STRPTR)"conf: could not run " SUITE);
        return RETURN_FAIL;
    }

    Printf((STRPTR)"conf: bsdsocktest returned %ld\n", rc);

    /*
     * The suite exits nonzero when tests fail, which is the normal outcome
     * here and must not look like harness breakage: report it on stdout and
     * hand the harness a success so the run is scored from the TAP log.
     */
    return RETURN_OK;
}
