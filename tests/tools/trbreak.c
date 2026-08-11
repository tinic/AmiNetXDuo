/*
 * TrBreak, does Ctrl-C actually stop a command that is waiting on the network?
 *
 *     TrBreak COMMAND/A/F, SECONDS/N/K, CEILING/N/K
 *
 * Runs COMMAND as a child process, presses Ctrl-C on it after SECONDS, and
 * reports how long it then took to exit.  CEILING is the number of seconds
 * after the break beyond which the command counts as having ignored it.
 *
 * WHY A CHILD PROCESS AND NOT System()
 *
 *   The signal has to arrive at the process that is RUNNING the command, and
 *   System() starts one of its own that this program has no handle on.  So the
 *   command is loaded with LoadSeg() and started with CreateNewProc(), which
 *   hands back the Process the break can be sent to.  Same shape as the child
 *   in tests/tools/resolvebreak.c, the other way round: there the child does
 *   the signalling, here it does the waiting.
 *
 * WHY IT IS NOT ENOUGH TO SET THE BREAK BEFOREHAND
 *
 *   A command that reads the break at the top of its outer loop passes that
 *   test whatever it does afterwards.  The defect this exists for needs the
 *   break to land while the command is in the MIDDLE of a wait: SIGBREAKF_
 *   CTRL_C is edge triggered and SetSignal clears it for the first reader, so
 *   an inner loop that consumes it leaves the outer loop's own check answering
 *   FALSE for ever after.  traceroute shipped like that -- Ctrl-C ended the
 *   wait, the hop counter ran on to thirty, and every remaining hop printed as
 *   a bare number.  Only a break sent mid-run finds it.
 *
 * WHAT IT PRINTS
 *
 *   key=value, one per line, and an exit code:
 *
 *     0  the command exited within CEILING of the break
 *     5  it did not
 *    10  it could not be run at all
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

#define TR_DEFAULT_WAIT     6UL     /* seconds before the break        */
#define TR_DEFAULT_CEILING  3UL     /* seconds it may take to notice   */

/* DateStamp(), for the same reason resolvebreak.c uses it: ds_Tick is 1/50 s
   and it reads the same on every model. */
static ULONG tb_ticks(VOID)
{
    struct DateStamp ds;

    DateStamp(&ds);

    return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

/* The command's name and its arguments, split at the first space.  The
   arguments a child gets must end in a newline; ReadArgs in the child reads
   them as a line. */
static ULONG tb_split(char *line, char **args_out)
{
    ULONG i = 0;

    while (line[i] != '\0' && line[i] != ' ')
        i++;

    if (line[i] == '\0')
    {
        *args_out = &line[i];
        return i;
    }

    line[i] = '\0';
    *args_out = &line[i + 1];

    return i;
}

int main(int argc, char **argv)
{
    struct RDArgs  *rda;
    LONG            args[3];
    char            line[256];
    char            arguments[256];
    char           *rest;
    BPTR            seg;
    struct Process *child;
    struct MsgPort *port;
    ULONG           seconds;
    ULONG           ceiling;
    ULONG           start;
    ULONG           sent;
    ULONG           ended;
    ULONG           after;
    ULONG           i;
    LONG            rc = RETURN_OK;

    (VOID)argc;
    (VOID)argv;

    args[0] = 0;
    args[1] = 0;
    args[2] = 0;

    rda = ReadArgs((CONST_STRPTR)"COMMAND/A/F,SECONDS/N/K,CEILING/N/K",
                   args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"TrBreak");
        return RETURN_ERROR;
    }

    seconds = (args[1] != 0) ? (ULONG)(*(LONG *)args[1]) : TR_DEFAULT_WAIT;
    ceiling = (args[2] != 0) ? (ULONG)(*(LONG *)args[2]) : TR_DEFAULT_CEILING;

    for (i = 0; i < (ULONG)sizeof(line) - 1 &&
                ((const char *)args[0])[i] != '\0'; i++)
        line[i] = ((const char *)args[0])[i];
    line[i] = '\0';

    (VOID)tb_split(line, &rest);

    /* CreateNewProc's NP_Arguments wants a newline-terminated string. */
    for (i = 0; i < (ULONG)sizeof(arguments) - 2 && rest[i] != '\0'; i++)
        arguments[i] = rest[i];
    arguments[i]     = '\n';
    arguments[i + 1] = '\0';

    seg = LoadSeg((CONST_STRPTR)line);
    if (seg == (BPTR)0)
    {
        Printf((CONST_STRPTR)"error=cannot load %s\n", (LONG)line);
        Printf((CONST_STRPTR)"result=infra\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    Printf((CONST_STRPTR)"command=%s\n", (LONG)line);
    Printf((CONST_STRPTR)"break_after=%ld\n", (LONG)seconds);

    /*
     * NP_StackSize: a Shell command gets whatever the Shell has and these
     * commands are written for 4 KB, but the default for a CreateNewProc()
     * child is smaller than that on some systems, so it is stated.
     *
     * NP_FreeSeglist is FALSE: the child would free a seglist this process
     * still holds a handle on, and UnLoadSeg() below is the only owner.
     */
    port = CreateMsgPort();

    child = CreateNewProcTags(
                NP_Seglist,     (Tag)seg,
                NP_FreeSeglist, (Tag)FALSE,
                NP_Name,        (Tag)"TrBreak child",
                NP_Arguments,   (Tag)arguments,
                NP_StackSize,   (Tag)16384,
                NP_Input,       (Tag)Input(),
                NP_Output,      (Tag)Output(),
                NP_CloseInput,  (Tag)FALSE,
                NP_CloseOutput, (Tag)FALSE,
                NP_ExitCode,    (Tag)NULL,
                TAG_END);

    if (child == NULL)
    {
        Printf((CONST_STRPTR)"error=cannot start %s\n", (LONG)line);
        Printf((CONST_STRPTR)"result=infra\n");
        if (port != NULL)
            DeleteMsgPort(port);
        UnLoadSeg(seg);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    start = tb_ticks();

    /*
     * Waiting by the clock rather than on the child, because the point is to
     * interrupt it while it is still running.  Delay() in slices so that a
     * Ctrl-C aimed at THIS program is not swallowed by the wait.
     */
    {
        ULONG left = seconds * 50UL;

        while (left > 0UL)
        {
            ULONG slice = (left > 10UL) ? 10UL : left;

            Delay((LONG)slice);
            left -= slice;
        }
    }

    sent = tb_ticks();
    Printf((CONST_STRPTR)"break_sent_at=%ld\n", (LONG)((sent - start) / 50UL));

    Signal((struct Task *)child, SIGBREAKF_CTRL_C);

    /*
     * How long the command takes to go away.  Polled rather than waited on: a
     * child that never exits must be reported, not waited for, and this is the
     * only place that can tell the difference.
     *
     * pr_Task.tc_State is not safe to read once the process has gone, so the
     * end is detected by the child clearing its own pr_CLI... which it does
     * not do either.  The reliable signal is the exit handshake: NP_ExitCode
     * is NULL here, so instead the loop watches a ceiling and reports what it
     * saw, and the transcript's own ordering says whether the command's output
     * stopped.
     */
    {
        ULONG limit = (ceiling + 2UL) * 50UL;
        ULONG waited = 0UL;

        while (waited < limit && FindTask((CONST_STRPTR)"TrBreak child") != NULL)
        {
            Delay(5);
            waited += 5UL;
        }

        ended = tb_ticks();
        after = (ended - sent) / 50UL;
    }

    Printf((CONST_STRPTR)"exit_after_break=%ld\n", (LONG)after);

    if (FindTask((CONST_STRPTR)"TrBreak child") != NULL)
    {
        Printf((CONST_STRPTR)"still_running=yes\n");
        Printf((CONST_STRPTR)"result=ignored-break\n");
        rc = RETURN_WARN;

        /* Leaving a process running would poison every command after this one
           in the same boot, so it is signalled again and given a moment. */
        Signal((struct Task *)child, SIGBREAKF_CTRL_C);
        Delay(100);
    }
    else if (after > ceiling)
    {
        Printf((CONST_STRPTR)"still_running=no\n");
        Printf((CONST_STRPTR)"result=slow-break\n");
        rc = RETURN_WARN;
    }
    else
    {
        Printf((CONST_STRPTR)"still_running=no\n");
        Printf((CONST_STRPTR)"result=broke\n");
    }

    if (port != NULL)
        DeleteMsgPort(port);

    /* The child may still hold the seglist if it ignored the break; the extra
       Delay above is the whole of the grace it gets. */
    UnLoadSeg(seg);
    FreeArgs(rda);

    return rc;
}
