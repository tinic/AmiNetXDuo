/*
 * ToolsSmoke -- run every command-line tool under the emulator and record
 * what it printed.
 *
 * tools/fsuae-run.sh boots a real Kickstart 3.1 A1200, runs ONE executable
 * from s/Startup-Sequence with no arguments, and prints back anything the
 * Amiga left in DH0:*.txt.  The tools take arguments and write to stdout, so
 * this driver stands in the middle: it runs each of them through
 * SystemTagList() with the Shell's own redirection, appends the return code,
 * and leaves the lot in DH0:tools.txt for the host to read.
 *
 *   ./tools/fsuae-run.sh -t 120 build/cm/src/tools/ToolsSmoke \
 *       build/cm/src/tools/AddNetInterface build/cm/src/tools/Online ... \
 *       src/tools/testdata/Devs
 *
 * With no stack linked in (the netstack_weak.c stubs) every tool should
 * report that the network stack is not running and exit with a sensible
 * return code.  That is the state a user meets first, and it is the one thing
 * that can be tested end to end before src/netstack exists.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: ToolsSmoke 1.0 (24.7.2026)";

#define REPORT      "DH0:tools.txt"
/*
 * "<NIL:" so that ReadArgs()'s "?" prompt reads EOF instead of waiting for a
 * keyboard nobody is sitting at. No "*>>": the 3.1 Shell does not understand
 * appending stderr redirection and passes it through as an argument -- and
 * there is no need, because a child of System() has no separate error stream,
 * so tool_error() and PrintFault() both land on stdout anyway.
 */
#define REDIRECT    " <NIL: >>DH0:tools.txt"

static const char *const commands[] =
{
    "SYS:AddNetInterface ?",
    "SYS:AddNetInterface eth0",
    "SYS:AddNetInterface nosuchinterface",
    "SYS:Online ?",
    "SYS:Online eth0",
    "SYS:Online",                       /* required argument missing */
    "SYS:Offline eth0",
    "SYS:ShowNetStatus ?",
    "SYS:ShowNetStatus",
    "SYS:netstat ?",
    "SYS:netstat -r",
    "SYS:netstat -i",
    "SYS:netstat -a",
    "SYS:netstat ROUTES",
    "SYS:ping ?",
    "SYS:ping 127.0.0.1 COUNT 1",
    "SYS:host ?",
    "SYS:host www.example.com",
    "SYS:host 1.2.3.4",
    NULL
};

/* Append a line to the report, opening and closing around every write so the
   Shell's own >> redirection never fights us for the file position. */
static void report(const char *fmt, LONG a, LONG b)
{
    BPTR fh = Open((CONST_STRPTR)REPORT, MODE_READWRITE);

    if (fh == (BPTR)0)
        return;

    Seek(fh, 0, OFFSET_END);
    {
        LONG args[2];

        args[0] = a;
        args[1] = b;
        VFPrintf(fh, (CONST_STRPTR)fmt, (CONST_APTR)args);
    }
    Close(fh);
}

int main(int argc, char **argv)
{
    struct Process *self = (struct Process *)FindTask(NULL);
    APTR            old_window;
    BPTR            fh;
    int             i;
    int             failures = 0;

    (void)argc;
    (void)argv;

    /* No "please insert volume DEVS:" requester is going to be answered on a
       machine with nobody at the keyboard. */
    old_window = self->pr_WindowPtr;
    self->pr_WindowPtr = (APTR)-1;

    fh = Open((CONST_STRPTR)REPORT, MODE_NEWFILE);
    if (fh == (BPTR)0)
    {
        PutStr((CONST_STRPTR)"ToolsSmoke: cannot create " REPORT "\n");
        self->pr_WindowPtr = old_window;
        return RETURN_FAIL;
    }
    FPuts(fh, (CONST_STRPTR)"AmiNetXDuo tools smoke test "
                            "(no network stack linked in)\n");
    Close(fh);

    for (i = 0; commands[i] != NULL; i++)
    {
        char line[160];
        LONG rc;
        int  n = 0;
        int  k;

        report((const char *)"\n===== %s =====\n", (LONG)commands[i], 0);

        for (k = 0; commands[i][k] != '\0' && n < (int)sizeof(line) - 64; k++)
            line[n++] = commands[i][k];
        for (k = 0; REDIRECT[k] != '\0' && n < (int)sizeof(line) - 1; k++)
            line[n++] = REDIRECT[k];
        line[n] = '\0';

        rc = SystemTagList((CONST_STRPTR)line, NULL);

        if (rc == -1)
        {
            report((const char *)"----- could not run (IoErr %ld) -----\n",
                   IoErr(), 0);
            failures++;
        }
        else
        {
            report((const char *)"----- rc %ld -----\n", rc, 0);
        }

        if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C)
        {
            SetSignal(0L, SIGBREAKF_CTRL_C);
            report((const char *)"\n*** Break -- stopping\n", 0, 0);
            break;
        }
    }

    report((const char *)"\n===== done, %ld command(s) would not run =====\n",
           (LONG)failures, 0);

    self->pr_WindowPtr = old_window;

    return (failures == 0) ? RETURN_OK : RETURN_ERROR;
}
