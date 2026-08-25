/*
 * TrBreak, does Ctrl-C actually stop a command that is waiting on the network?
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

/* The child tells us it has gone, rather than us watching for its name: with
   NP_Cli the process is named by dos.library and FindTask() is no answer. */
#define TB_CHILD_GONE       SIGBREAKF_CTRL_F

static struct Task   *tb_parent;
static volatile LONG  tb_child_rc;

static VOID tb_child_exit(register LONG rc   __asm("d0"),
                          register APTR data __asm("d1"))
{
    (VOID)data;

    tb_child_rc = rc;
    Signal(tb_parent, TB_CHILD_GONE);
}

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

    tb_parent = FindTask(NULL);
    (VOID)SetSignal(0UL, TB_CHILD_GONE);

    child = CreateNewProcTags(
                NP_Seglist,     (Tag)seg,
                NP_FreeSeglist, (Tag)FALSE,
                NP_Cli,         (Tag)TRUE,
                NP_Name,        (Tag)line,
                NP_Arguments,   (Tag)arguments,
                NP_StackSize,   (Tag)16384,
                NP_Input,       (Tag)Input(),
                NP_Output,      (Tag)Output(),
                NP_CloseInput,  (Tag)FALSE,
                NP_CloseOutput, (Tag)FALSE,
                NP_ExitCode,    (Tag)tb_child_exit,
                TAG_END);

    if (child == NULL)
    {
        Printf((CONST_STRPTR)"error=cannot start %s\n", (LONG)line);
        Printf((CONST_STRPTR)"result=infra\n");
        UnLoadSeg(seg);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    start = tb_ticks();

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

    if ((SetSignal(0UL, 0UL) & TB_CHILD_GONE) != 0)
    {
        Printf((CONST_STRPTR)"alive_at_break=no\n");
        Printf((CONST_STRPTR)"result=vacuous\n");
        UnLoadSeg(seg);
        FreeArgs(rda);
        return RETURN_WARN;
    }

    Printf((CONST_STRPTR)"alive_at_break=yes\n");

    Signal((struct Task *)child, SIGBREAKF_CTRL_C);

    {
        ULONG limit  = (ceiling + 2UL) * 50UL;
        ULONG waited = 0UL;

        while (waited < limit &&
               (SetSignal(0UL, 0UL) & TB_CHILD_GONE) == 0)
        {
            Delay(5);
            waited += 5UL;
        }

        ended = tb_ticks();
        after = (ended - sent) / 50UL;
    }

    Printf((CONST_STRPTR)"exit_after_break=%ld\n", (LONG)after);
    Printf((CONST_STRPTR)"child_rc=%ld\n", (LONG)tb_child_rc);

    if ((SetSignal(0UL, 0UL) & TB_CHILD_GONE) == 0)
    {
        Printf((CONST_STRPTR)"still_running=yes\n");
        Printf((CONST_STRPTR)"result=ignored-break\n");
        rc = RETURN_WARN;

        Signal((struct Task *)child, SIGBREAKF_CTRL_C);
        {
            ULONG waited = 0UL;

            while (waited < 60UL * 50UL &&
                   (SetSignal(0UL, 0UL) & TB_CHILD_GONE) == 0)
            {
                Delay(10);
                waited += 10UL;
            }
        }
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

    /* The child may still hold the seglist if it ignored the break; the extra
       Delay above is the whole of the grace it gets. */
    UnLoadSeg(seg);
    FreeArgs(rda);

    return rc;
}
