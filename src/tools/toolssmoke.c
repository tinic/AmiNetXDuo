/*
 * ToolsSmoke, run every command-line tool under the emulator and record
 * what it printed.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "aminetxduo/version.h"

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("ToolsSmoke");

#define REPORT      "DH0:tools.txt"
#define REDIRECT_IN  " <NIL:"
#define REDIRECT_OUT " >>DH0:tools.txt"

#define COMMANDS    "DH0:commands.txt"
/*
 * Truncation at this ceiling is silent, so it sits well above what any staged
 * list uses.  A run that stops reading at line 40 looks like a set of commands
 * that were never written.
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

/* Storage for a staged command list.  Static, because a Shell command's stack
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

/*
 * Milliseconds between two DateStamps.  ds_Tick is fiftieths of a second, so
 * the resolution is 20 ms, which is fine for a stage measured in seconds and
 * is the only clock here that needs no device opened.
 */
static LONG elapsed_ms(const struct DateStamp *from, const struct DateStamp *to)
{
    LONG days  = to->ds_Days   - from->ds_Days;
    LONG mins  = to->ds_Minute - from->ds_Minute;
    LONG ticks = to->ds_Tick   - from->ds_Tick;

    return ((days * 1440L + mins) * 60L * 50L + ticks) * 20L;
}

/* Opens and closes around every write so the Shell's own >> redirection never
   competes for the file position.  The argarray must be cast to (APTR), not
   (CONST_APTR): only APTR converts under both NDK 3.2 and NDK 3.9. */
static void report(const char *fmt, LONG a, LONG b, LONG c)
{
    BPTR fh = Open((CONST_STRPTR)REPORT, MODE_READWRITE);

    if (fh == (BPTR)0)
        return;

    Seek(fh, 0, OFFSET_END);
    {
        LONG args[3];

        args[0] = a;
        args[1] = b;
        args[2] = c;
        VFPrintf(fh, (CONST_STRPTR)fmt, (APTR)args);
    }
    Close(fh);
}

/* No libc here beyond memcpy and memset, and newlib's strstr costs 2 KB. */
static int line_holds(const char *hay, const char *needle)
{
    int i;
    int j;

    if (needle[0] == '\0')
        return 1;

    for (i = 0; hay[i] != '\0'; i++)
    {
        for (j = 0; needle[j] != '\0' && hay[i + j] == needle[j]; j++)
            ;
        if (needle[j] == '\0')
            return 1;
    }

    return 0;
}

/* Run one staged command, header, redirection, rc and all.  Returns 1 when
   the Shell could not start it, which is what `failures` counts. */
static int run_command(const char *command)
{
    char             line[MAX_LINE + 40];
    struct DateStamp started;
    LONG             rc;
    int              n = 0;
    int              k;
    int              has_input = 0;
    int              has_output = 0;
    int              async = 0;

    report((const char *)"\n===== %s =====\n", (LONG)command, 0, 0);

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

    DateStamp(&started);

    if (async)
    {
        /*
         * A detached child cannot share this process's streams. System()
         * closes whatever it is given when the child ends, and closing the
         * Shell's own Output() out from under it is fatal. It gets a pair of
         * NIL: handles of its own, and anything to be kept is redirected by
         * the line itself.
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
            /* Nothing was started, so nothing else closes them. */
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
               IoErr(), 0, 0);
        return 1;
    }

    if (async)
    {
        report((const char *)"----- started in the background -----\n",
               0, 0, 0);
        return 0;
    }

    {
        struct DateStamp finished;

        DateStamp(&finished);

        report((const char *)"----- rc %ld, %ld ms, free %ld -----\n", rc,
               elapsed_ms(&started, &finished),
               (LONG)AvailMem(MEMF_ANY));
    }

    return 0;
}

/* Any one of a comma-separated list of substrings, on this line. */
static int line_holds_any(const char *hay, const char *list)
{
    char one[MAX_LINE];
    int  i = 0;
    int  n;

    while (list[i] != '\0')
    {
        for (n = 0; list[i] != '\0' && list[i] != ',' &&
                    n < (int)sizeof(one) - 1; i++)
            one[n++] = list[i];
        one[n] = '\0';

        if (list[i] == ',')
            i++;

        if (n > 0 && line_holds(hay, one))
            return 1;
    }

    return 0;
}

/*
 * Did this attempt's output carry a line with `needle` and with none of the
 * comma-separated substrings in `stop`?  `from` is where the report stood
 * before the command ran, so only what this attempt appended is read.
 */
static int attempt_met(LONG from, const char *needle, const char *stop)
{
    BPTR fh = Open((CONST_STRPTR)REPORT, MODE_OLDFILE);
    char line[MAX_LINE + 40];
    int  met = 0;

    if (fh == (BPTR)0)
        return 0;

    if (Seek(fh, from, OFFSET_BEGINNING) < 0)
    {
        Close(fh);
        return 0;
    }

    while (!met && FGets(fh, (STRPTR)line, (ULONG)sizeof(line)) != NULL)
    {
        if (!line_holds(line, needle))
            continue;
        if (stop[0] != '\0' && line_holds_any(line, stop))
            continue;
        met = 1;
    }

    Close(fh);
    return met;
}

/* Where the report currently ends. */
static LONG report_end(void)
{
    BPTR fh = Open((CONST_STRPTR)REPORT, MODE_OLDFILE);
    LONG pos;

    if (fh == (BPTR)0)
        return 0;

    Seek(fh, 0, OFFSET_END);
    pos = Seek(fh, 0, OFFSET_CURRENT);
    Close(fh);

    return (pos < 0) ? 0 : pos;
}

/*
 * "until <secs> <needle> <not> <command>".  Retries every two seconds and
 * stops the moment the condition holds, so the cost is what the machine
 * needed and the deadline is only reached when the thing never happened.
 * Returns the number of attempts the Shell could not start.
 */
static int run_until(const char *spec)
{
    char        needle[MAX_LINE];
    char        stop[MAX_LINE];
    const char *p = spec + 5;           /* past "until" */
    LONG        secs = 0;
    LONG        waited = 0;
    int         failures = 0;
    int         met = 0;
    int         n;

    while (*p == ' ')
        p++;
    for (; *p >= '0' && *p <= '9'; p++)
        secs = (secs * 10) + (*p - '0');
    while (*p == ' ')
        p++;

    for (n = 0; *p != '\0' && *p != ' ' && n < (int)sizeof(needle) - 1; p++)
        needle[n++] = *p;
    needle[n] = '\0';
    while (*p == ' ')
        p++;

    for (n = 0; *p != '\0' && *p != ' ' && n < (int)sizeof(stop) - 1; p++)
        stop[n++] = *p;
    stop[n] = '\0';
    while (*p == ' ')
        p++;

    /* `-` excludes nothing, so a plain condition needs no placeholder of its
       own. */
    if (stop[0] == '-' && stop[1] == '\0')
        stop[0] = '\0';

    if (needle[0] == '\0' || *p == '\0')
    {
        report((const char *)"----- until: malformed, nothing run -----\n",
               0, 0, 0);
        return 1;
    }

    for (;;)
    {
        LONG from = report_end();

        failures += run_command(p);

        if (attempt_met(from, needle, stop))
        {
            met = 1;
            break;
        }

        if (waited >= secs)
            break;

        Delay(2UL * 50UL);
        waited += 2;

        if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C)
            break;
    }

    report(met ? (const char *)"----- until %s: met after %ld s -----\n"
                : (const char *)"----- until %s: NOT MET after %ld s -----\n",
           (LONG)needle, waited, 0);

    return failures;
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

    /* No "please insert volume DEVS:" requester is answered on a machine with
       nobody at the keyboard. */
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
        const char *command;
        int         k;

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

        /* "wait <seconds>", for letting a background listener settle. */
        if ((command[0] == 'w' || command[0] == 'W') &&
            (command[1] == 'a' || command[1] == 'A') &&
            (command[2] == 'i' || command[2] == 'I') &&
            (command[3] == 't' || command[3] == 'T') &&
            (command[4] == ' ' || command[4] == '\0'))
        {
            LONG secs = 0;

            report((const char *)"\n===== %s =====\n", (LONG)command, 0, 0);

            for (k = 5; command[k] >= '0' && command[k] <= '9'; k++)
                secs = (secs * 10) + (command[k] - '0');

            if (secs > 0)
                Delay((ULONG)secs * 50UL);

            report((const char *)"----- waited %ld s -----\n", secs, 0, 0);
            continue;
        }

        /* "until <secs> <needle> <not> <command>", the same idea without the
           guess.  It stops when the condition holds. */
        if ((command[0] == 'u' || command[0] == 'U') &&
            (command[1] == 'n' || command[1] == 'N') &&
            (command[2] == 't' || command[2] == 'T') &&
            (command[3] == 'i' || command[3] == 'I') &&
            (command[4] == 'l' || command[4] == 'L') &&
            command[5] == ' ')
        {
            failures += run_until(command);

            if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C)
            {
                SetSignal(0L, SIGBREAKF_CTRL_C);
                report((const char *)"\n*** Break, stopping\n", 0, 0, 0);
                break;
            }
            continue;
        }

        failures += run_command(command);

        if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C)
        {
            SetSignal(0L, SIGBREAKF_CTRL_C);
            report((const char *)"\n*** Break, stopping\n", 0, 0, 0);
            break;
        }
    }

    report((const char *)"\n===== done, %ld command(s) would not run =====\n",
           (LONG)failures, 0, 0);

    self->pr_WindowPtr = old_window;

    return (failures == 0) ? RETURN_OK : RETURN_ERROR;
}
