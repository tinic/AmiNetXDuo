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
 *
 * Each half is added only when the command has not brought its own: NetSetup
 * reads a file of answers with "<", and `ftp ... >DH0:session.txt` wants its
 * transcript in a file of its own. Adding a second redirection of the same
 * kind makes the Shell take one of them and quietly drop the other.
 */
#define REDIRECT_IN  " <NIL:"
#define REDIRECT_OUT " >>DH0:tools.txt"

/*
 * The command list can be staged as DH0:commands.txt, one command per line,
 * '#' and blank lines ignored. That is what lets one harness run -- which can
 * only start a single executable with no arguments -- exercise a machine with
 * no configuration, a machine with a broken one, and a machine whose config
 * names a device that is not there, simply by staging different directories.
 *
 * Two prefixes, both there for one reason: a listener and the thing that
 * connects to it have to be running AT THE SAME TIME, and SystemTagList()
 * waits. Without them no staged list can test `nc -l` at all.
 *
 *   &<command>   run it and carry straight on -- SYS_Asynch, which is what
 *                the Shell's own `Run` does. Its output must be redirected by
 *                the line itself; a detached process shares no console with
 *                this one and its Output() is NIL:.
 *   wait <secs>  Delay(), so the next line starts after the background one
 *                has had time to reach its accept().
 */
#define COMMANDS    "DH0:commands.txt"
/*
 * The ceiling is a silent truncation, so it is set well above what any staged
 * list uses: a run that quietly stops reading at line 40 looks exactly like a
 * set of commands that were never written.
 */
#define MAX_COMMANDS    96
#define MAX_LINE        160

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
    "SYS:fetch ?",
    "SYS:fetch not-a-url:///",
    "SYS:host www.example.com",
    "SYS:host 1.2.3.4",
    NULL
};

/* Storage for a staged command list; static, because a Shell command's stack
   is 4K and this is 6K of it. */
static char  script[MAX_COMMANDS][MAX_LINE];
static ULONG script_count;

/*
 * Read DH0:commands.txt into `script`. Returns 0 when there is no such file,
 * in which case the built-in list above is used.
 */
static ULONG load_script(void)
{
    BPTR  fh = Open((CONST_STRPTR)COMMANDS, MODE_OLDFILE);
    char  line[MAX_LINE];

    if (fh == (BPTR)0)
        return 0;

    while (script_count < (ULONG)MAX_COMMANDS &&
           FGets(fh, (STRPTR)line, (ULONG)MAX_LINE) != NULL)
    {
        int n = 0;
        int i;

        while (line[n] != '\0')
            n++;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         line[n - 1] == ' '))
            line[--n] = '\0';

        if (line[0] == '\0' || line[0] == '#')
            continue;

        for (i = 0; i <= n; i++)
            script[script_count][i] = line[i];
        script_count++;
    }

    Close(fh);

    return script_count;
}

/* Append a line to the report, opening and closing around every write so the
   Shell's own >> redirection never fights us for the file position.

   The argarray is cast to (APTR), not (CONST_APTR): NDK 3.2 and NDK 3.9 spell
   VFPrintf's third parameter differently and only APTR converts to both.  See
   the long note in src/tools/tool_util.c. */
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
        VFPrintf(fh, (CONST_STRPTR)fmt, (APTR)args);
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

    (void)load_script();

    for (i = 0; ; i++)
    {
        char        line[MAX_LINE + 40];
        const char *command;
        LONG        rc;
        int         n = 0;
        int         k;
        int         has_input = 0;
        int         has_output = 0;
        int         async = 0;

        if (script_count > 0)
        {
            if ((ULONG)i >= script_count)
                break;
            command = script[i];
        }
        else
        {
            if (commands[i] == NULL)
                break;
            command = commands[i];
        }

        report((const char *)"\n===== %s =====\n", (LONG)command, 0);

        /* "wait <seconds>" -- for letting a background listener settle. */
        if ((command[0] == 'w' || command[0] == 'W') &&
            (command[1] == 'a' || command[1] == 'A') &&
            (command[2] == 'i' || command[2] == 'I') &&
            (command[3] == 't' || command[3] == 'T') &&
            (command[4] == ' ' || command[4] == '\0'))
        {
            LONG secs = 0;

            for (k = 5; command[k] >= '0' && command[k] <= '9'; k++)
                secs = (secs * 10) + (command[k] - '0');

            if (secs > 0)
                Delay((ULONG)secs * 50UL);

            report((const char *)"----- waited %ld s -----\n", secs, 0);
            continue;
        }

        if (command[0] == '&')
        {
            async = 1;
            command++;
        }

        for (k = 0; command[k] != '\0'; k++)
        {
            if (command[k] == '<')
                has_input = 1;
            if (command[k] == '>')
                has_output = 1;
        }

        for (k = 0; command[k] != '\0' && n < (int)sizeof(line) - 32; k++)
            line[n++] = command[k];

        if (!has_input)
        {
            const char *tail = (const char *)REDIRECT_IN;

            for (k = 0; tail[k] != '\0' && n < (int)sizeof(line) - 1; k++)
                line[n++] = tail[k];
        }

        if (!has_output && !async)
        {
            const char *tail = (const char *)REDIRECT_OUT;

            for (k = 0; tail[k] != '\0' && n < (int)sizeof(line) - 1; k++)
                line[n++] = tail[k];
        }

        line[n] = '\0';

        if (async)
        {
            /*
             * A detached child cannot share this process's streams: System()
             * closes whatever it is given when the child ends, and closing
             * the Shell's own Output() out from under it is fatal. It gets a
             * pair of NIL: handles of its own, and anything worth keeping is
             * redirected by the line itself.
             */
            BPTR in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
            BPTR out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);

            rc = SystemTags((CONST_STRPTR)line,
                            SYS_Input,  (Tag)in,
                            SYS_Output, (Tag)out,
                            SYS_Asynch, (Tag)DOSTRUE,
                            TAG_DONE);

            if (rc == -1)
            {
                /* Nothing was started, so nothing will close them. */
                if (in != (BPTR)0)
                    Close(in);
                if (out != (BPTR)0)
                    Close(out);
            }
        }
        else
        {
            rc = SystemTagList((CONST_STRPTR)line, NULL);
        }

        if (rc == -1)
        {
            report((const char *)"----- could not run (IoErr %ld) -----\n",
                   IoErr(), 0);
            failures++;
        }
        else if (async)
        {
            report((const char *)"----- started in the background -----\n",
                   0, 0);
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
