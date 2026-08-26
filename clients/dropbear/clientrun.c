/* ClientRun, the driver tools/amiberry-run.sh needs: runs DH0:commands.txt line
 * by line under SystemTagList() with a large NP_StackSize, because a Shell's
 * 4 KB kills a ported client.  SPDX-License-Identifier: MIT */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: ClientRun 1.0 (25.7.2026)";

#define REPORT      "DH0:client.txt"

/* One CON: specification per line, consumed in order by the '>' commands.
   Absent, empty or exhausted, CONSOLE_DEFAULT is used. */
#define CONSOLES    "DH0:console.txt"
#define CONSOLE_DEFAULT "CON:0/0/640/200/AmiNetXDuo/CLOSE"

/* Fed to a '<' command as its input. */
#define STDIN_FILE  "DH0:stdin.txt"

/* Seconds before and after a programmatic resize.  Generous: the session has to
   be up and the remote's first sample taken before the window moves, and this
   is a 14 MHz machine whose SSH handshake alone is several seconds. */
#define RESIZE_DELAY    18
#define RESIZE_SETTLE   20

/* The window a 'R' command resizes to, in pixels.  Deliberately unlike the
   sizes in DH0:console.txt so that "did it change" and "did it change to the
   right thing" are different questions. */
#define RESIZE_W        608
#define RESIZE_H        176

/* A separate file for async output.  Sharing REPORT would interleave a
   server's log with the client's, in a file both have open for append. */
#define ASYNC_REPORT "DH0:server.txt"
#define COMMANDS    "DH0:commands.txt"
#define REDIRECT    " <NIL: >>DH0:client.txt"

/* The console and stdin-file forms redirect output only: input comes from the
   handle in SYS_Input, and a "<" in the command string would override it. */
#define OUT_ONLY    " >>DH0:client.txt"

/* 512 KB, deliberately more than any client needs: this is the harness, and a
   client that runs out of stack here fails in a way that looks like the thing
   under test.  The real budget comes from amiga_client_stack_size(). */
#define CLIENT_STACK    (512UL * 1024UL)

#define MAX_COMMANDS    24

/* 512, not 320: a command carrying an absolute host path goes past 320,
   copy_bounded() then drops the line, and the next one starts mid-quoted-string.
   That runs, returns 10, and looks like the program under test refusing an argument. */
#define MAX_LINE        512

/* System() hands unrecognised tags to CreateNewProc(), which is how a child
   of a 4 KB Shell gets a stack a ported program can survive on. */
static struct TagItem client_tags[] =
{
    { NP_StackSize, CLIENT_STACK },
    { TAG_END,      0            }
};

/* SYS_Input and SYS_Output are filled in per command: with SYS_Asynch the
   child closes them when it exits, so the same handle must never be given
   to two commands. */
static struct TagItem async_tags[] =
{
    { SYS_Asynch,   DOSTRUE      },
    { SYS_Input,    0            },
    { SYS_Output,   0            },
    { NP_StackSize, CLIENT_STACK },
    { TAG_END,      0            }
};

/* SYS_Input is filled in per command and System() takes ownership of it, so a
   fresh handle is opened for each console.  No SYS_Output: the child's output
   goes where every other command's does, so input is a terminal, output a file. */
static struct TagItem console_tags[] =
{
    { SYS_Input,    0            },
    { NP_StackSize, CLIENT_STACK },
    { TAG_END,      0            }
};

/* Asynchronous form of console_tags: SYS_Asynch means the child closes
   SYS_Input, so nothing here closes the console after handing it over. */
static struct TagItem console_async_tags[] =
{
    { SYS_Asynch,   DOSTRUE      },
    { SYS_Input,    0            },
    { SYS_Output,   0            },
    { NP_StackSize, CLIENT_STACK },
    { TAG_END,      0            }
};

struct IntuitionBase *IntuitionBase;

static char  lines[MAX_COMMANDS][MAX_LINE];
static char  command[MAX_LINE + 64];
static ULONG line_count;

/* DateStamp() ticks, flattened.  One tick is 1/50 s, which is finer than
   anything measured here needs and is the same clock the client itself sees
   through clients/compat/amiga_posix.c's gettimeofday(). */
static LONG now_ticks(VOID)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return ds.ds_Minute * (60L * TICKS_PER_SECOND) + ds.ds_Tick;
}

static VOID report(const char *fmt, LONG arg)
{
    BPTR out = Open((CONST_STRPTR)REPORT, MODE_READWRITE);

    if (out == (BPTR)0)
        return;

    Seek(out, 0, OFFSET_END);
    VFPrintf(out, (CONST_STRPTR)fmt, &arg);
    Close(out);
}

/* Copy with a bound, returning FALSE rather than truncating a URL. */
static BOOL copy_bounded(char *dst, ULONG dstlen, const char *src)
{
    ULONG i = 0;

    while (src[i] != '\0')
    {
        if (i + 1 >= dstlen)
            return FALSE;
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';
    return TRUE;
}

static ULONG append(char *dst, ULONG dstlen, ULONG used, const char *src)
{
    ULONG i = 0;

    while (src[i] != '\0' && used + 1 < dstlen)
        dst[used++] = src[i++];

    dst[used] = '\0';
    return used;
}

static VOID read_commands(VOID)
{
    BPTR  in;
    char  buf[MAX_LINE];
    ULONG i;

    in = Open((CONST_STRPTR)COMMANDS, MODE_OLDFILE);
    if (in == (BPTR)0)
        return;

    while (line_count < (ULONG)MAX_COMMANDS &&
           FGets(in, (STRPTR)buf, (ULONG)sizeof(buf)) != NULL)
    {
        /* strip the newline FGets keeps */
        for (i = 0; buf[i] != '\0'; i++)
        {
            if (buf[i] == '\n' || buf[i] == '\r')
            {
                buf[i] = '\0';
                break;
            }
        }

        if (buf[0] == '\0' || buf[0] == '#')
            continue;

        if (copy_bounded(lines[line_count], (ULONG)MAX_LINE, buf))
            line_count++;
    }

    Close(in);
}

/* Console specs, read once, handed out in order. */
static char  consoles[8][160];
static ULONG console_count;
static ULONG console_next;

static VOID read_consoles(VOID)
{
    BPTR  in;
    char  buf[160];
    ULONG i;

    in = Open((CONST_STRPTR)CONSOLES, MODE_OLDFILE);
    if (in == (BPTR)0)
        return;

    while (console_count < 8 &&
           FGets(in, (STRPTR)buf, (ULONG)sizeof(buf)) != NULL)
    {
        for (i = 0; buf[i] != '\0'; i++)
        {
            if (buf[i] == '\n' || buf[i] == '\r')
            {
                buf[i] = '\0';
                break;
            }
        }
        if (buf[0] == '\0' || buf[0] == '#')
            continue;
        if (copy_bounded(consoles[console_count], 160UL, buf))
            console_count++;
    }

    Close(in);
}

/* fh_Arg1 is NOT the Window behind a CON: handle -- it yields somebody else's
   memory, and ChangeWindowBox() through it is a wild write.  ACTION_DISK_INFO's
   id_VolumeNode is, and the result is range checked before anything is written. */
static struct Window *console_window(BPTR con)
{
    struct InfoData   *id;
    struct MsgPort    *port;
    struct Window     *win = NULL;

    port = ((struct FileHandle *)BADDR(con))->fh_Type;
    if (port == NULL)
        return NULL;

    id = (struct InfoData *)AllocMem((ULONG)sizeof(struct InfoData),
                                     MEMF_PUBLIC | MEMF_CLEAR);
    if (id == NULL)
        return NULL;

    if (DoPkt(port, ACTION_DISK_INFO, MKBADDR(id), 0, 0, 0, 0))
        win = (struct Window *)id->id_VolumeNode;

    FreeMem(id, (ULONG)sizeof(struct InfoData));

    if (win == NULL)
        return NULL;

    if (win->Width < 16 || win->Width > 2048 ||
        win->Height < 16 || win->Height > 1024)
        return NULL;

    return win;
}

/* CSI '0 q' is the AmigaDOS Window Bounds Report; the console replies
   CSI '1;1;<rows>;<cols>' ' r'.  Raw mode first, or a cooked console holds the
   reply until a newline that never comes; every read here is bounded. */
static VOID report_console_size(BPTR con)
{
    UBYTE buf[32];
    ULONG used = 0;
    LONG  rows = 0;
    LONG  cols = 0;
    ULONG i;

    SetMode(con, 1);
    Write(con, (APTR)"\x9b" "0 q", 4);

    while (used < (ULONG)sizeof(buf) - 1 &&
           WaitForChar(con, 200000) == DOSTRUE)
    {
        if (Read(con, &buf[used], 1) != 1)
            break;
        if (buf[used] == 'r')       /* the report ends with ' r' */
        {
            used++;
            break;
        }
        used++;
    }
    buf[used] = '\0';

    if (used < 6)
    {
        report("--- console: no window bounds report (%ld bytes)\n", (LONG)used);
        return;
    }

    /* CSI 1 ; 1 ; rows ; cols SPACE r, five bytes of preamble, exactly as
       BebboSSH's own console.cpp skips them. */
    i = 5;
    for (; i < used && buf[i] != ';'; i++)
        rows = rows * 10 + (buf[i] - '0');
    i++;
    for (; i < used && buf[i] != ' '; i++)
        cols = cols * 10 + (buf[i] - '0');

    report("--- console rows %ld", rows);
    report(", cols %ld\n", cols);
}

int main(int argc, char **argv)
{
    BPTR  out;
    ULONG i;
    LONG  rc;

    (VOID)argc;
    (VOID)argv;

    /* Truncate both reports once, so a rerun does not append to the last run.
       The async one is truncated even when nothing async runs, or a run with
       no server would print the previous run's server log. */
    out = Open((CONST_STRPTR)REPORT, MODE_NEWFILE);
    if (out != (BPTR)0)
        Close(out);

    out = Open((CONST_STRPTR)ASYNC_REPORT, MODE_NEWFILE);
    if (out != (BPTR)0)
        Close(out);

    /* Ask every client for its stack high-water mark; clients/compat/amiga_argv.c
       paints the stack and reports the deepest word touched when it sees this.
       Set here, not in ENV:, which envsetup rebuilds on every boot. */
    (VOID)SetVar((CONST_STRPTR)"AMIGA_ARGV_STACKCHECK", (CONST_STRPTR)"1",
                 -1, LV_VAR);

    /* Only the 'R' form needs it, and a machine without it simply cannot be
       asked to resize a window, which is reported rather than fatal. */
    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary((CONST_STRPTR)"intuition.library", 37);

    read_commands();
    read_consoles();

    if (line_count == 0)
    {
        report("ClientRun: no DH0:commands.txt, nothing to run\n", 0);
        return RETURN_FAIL;
    }

    for (i = 0; i < line_count; i++)
    {
        ULONG used;
        LONG  t0;
        LONG  elapsed;

        /* wait <n>, seconds, so an async server has time to bind and listen
           before the client that is going to connect to it starts. */
        if (lines[i][0] == 'w' && lines[i][1] == 'a' && lines[i][2] == 'i'
            && lines[i][3] == 't' && lines[i][4] == ' ')
        {
            LONG secs = 0;
            const char *p = &lines[i][5];

            while (*p >= '0' && *p <= '9')
                secs = secs * 10 + (*p++ - '0');

            report("\n--- wait %ld s\n", secs);
            if (secs > 0)
                Delay((ULONG)secs * TICKS_PER_SECOND);
            continue;
        }

        /* &<command>, start it and carry on.  Nothing waits for it, so
           there is no return code to report. */
        if (lines[i][0] == '&')
        {
            BPTR in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
            BPTR aout = Open((CONST_STRPTR)ASYNC_REPORT, MODE_READWRITE);

            if (aout != (BPTR)0)
                Seek(aout, 0, OFFSET_END);

            report("\n--- (async) %s\n", (LONG)&lines[i][1]);

            async_tags[1].ti_Data = (ULONG)in;
            async_tags[2].ti_Data = (ULONG)aout;

            rc = SystemTagList((CONST_STRPTR)&lines[i][1], async_tags);

            /* SYS_Asynch only takes ownership of the handles once the child
               exists.  If it never did, they are still ours to close. */
            if (rc != 0)
            {
                report("--- could not start it: rc %ld\n", rc);
                if (in   != (BPTR)0) Close(in);
                if (aout != (BPTR)0) Close(aout);
            }
            continue;
        }

        /*
         * Rcommand: console input, and the window changes size under the command
         * while it runs.  Started asynchronously so this process is still awake
         * to move the window.
         */
        if (lines[i][0] == 'R')
        {
            const char *spec;
            BPTR        in;
            BPTR        nil;
            BPTR        out;
            struct Window *win;

            spec = (console_next < console_count)
                 ? consoles[console_next] : CONSOLE_DEFAULT;
            console_next++;

            report("\n--- %s\n", (LONG)&lines[i][1]);
            report("--- input: %s\n", (LONG)spec);

            in = Open((CONST_STRPTR)spec, MODE_OLDFILE);
            if (in == (BPTR)0)
            {
                report("--- could not open it\n", 0);
                continue;
            }

            report_console_size(in);

            win = console_window(in);
            if (IntuitionBase == NULL || win == NULL)
            {
                report("--- no window behind the console; cannot resize\n", 0);
                Close(in);
                continue;
            }

            /* Output must be the console too: a client enables resize reporting
               by WRITING to its stdout, so a stdout on a file never tells the
               console.  Open("*") under pr_ConsoleTask reopens the SAME window. */
            nil = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
            (VOID)nil;
            {
                struct Process *me = (struct Process *)FindTask(NULL);
                APTR oldct = me->pr_ConsoleTask;

                me->pr_ConsoleTask = ((struct FileHandle *)BADDR(in))->fh_Type;
                out = Open((CONST_STRPTR)"*", MODE_OLDFILE);
                me->pr_ConsoleTask = oldct;
            }
            if (out == (BPTR)0)
            {
                report("--- could not open a second handle on the console\n", 0);
                Close(in);
                continue;
            }
            if (out != (BPTR)0)
                Seek(out, 0, OFFSET_END);
            console_async_tags[1].ti_Data = (ULONG)in;
            console_async_tags[2].ti_Data = (ULONG)out;

            t0 = now_ticks();
            rc = SystemTagList((CONST_STRPTR)&lines[i][1], console_async_tags);
            if (rc != 0)
            {
                report("--- could not start it: rc %ld\n", rc);
                Close(in);
                if (out != (BPTR)0) Close(out);
                continue;
            }

            Delay((ULONG)RESIZE_DELAY * TICKS_PER_SECOND);

            /* The window still belongs to the console, which is still open
               because the child holds the handle. */
            report("--- window before: %ld", (LONG)win->Width);
            report("x%ld pixels\n", (LONG)win->Height);

            ChangeWindowBox(win, win->LeftEdge, win->TopEdge,
                            RESIZE_W, RESIZE_H);

            /* ChangeWindowBox() is a request: Intuition performs it when it next
               runs and clamps it to the window's own limits, so the window is
               measured again afterwards and THAT is what says anything moved. */
            Delay(2 * TICKS_PER_SECOND);
            report("--- window after:  %ld", (LONG)win->Width);
            report("x%ld pixels\n", (LONG)win->Height);

            Delay((ULONG)RESIZE_SETTLE * TICKS_PER_SECOND);
            elapsed = now_ticks() - t0;

            report("--- waited %ld", elapsed / TICKS_PER_SECOND);
            report(".%02ld s\n", (elapsed % TICKS_PER_SECOND) * 2);
            continue;
        }

        /* >command, a real console as its input, so IsInteractive() is
           true and the terminal path in the program under test runs. */
        if (lines[i][0] == '>' || lines[i][0] == '<')
        {
            BOOL        console = (lines[i][0] == '>');
            const char *spec;
            BPTR        in;

            if (console)
            {
                spec = (console_next < console_count)
                     ? consoles[console_next] : CONSOLE_DEFAULT;
                console_next++;
            }
            else
            {
                spec = STDIN_FILE;
            }

            report("\n--- %s\n", (LONG)&lines[i][1]);
            report("--- input: %s\n", (LONG)spec);

            in = Open((CONST_STRPTR)spec, MODE_OLDFILE);
            if (in == (BPTR)0)
            {
                report("--- could not open it\n", 0);
                continue;
            }

            if (console)
                report_console_size(in);

            /* SYS_Input only.  SYS_Output stays unset so the child's output
               goes where every other command's does, the report file on the
               host, and the terminal under test is still a terminal. */
            console_tags[0].ti_Data = (ULONG)in;

            used = 0;
            used = append(command, (ULONG)sizeof(command), used, &lines[i][1]);
            used = append(command, (ULONG)sizeof(command), used, OUT_ONLY);
            (VOID)used;

            t0 = now_ticks();
            rc = SystemTagList((CONST_STRPTR)command, console_tags);
            elapsed = now_ticks() - t0;

            report("--- rc %ld", rc);
            report(", %ld", elapsed / TICKS_PER_SECOND);
            report(".%02ld s\n", (elapsed % TICKS_PER_SECOND) * 2);
            continue;
        }

        report("\n--- %s\n", (LONG)lines[i]);

        used = 0;
        used = append(command, (ULONG)sizeof(command), used, lines[i]);
        used = append(command, (ULONG)sizeof(command), used, REDIRECT);
        (VOID)used;

        t0 = now_ticks();
        rc = SystemTagList((CONST_STRPTR)command, client_tags);
        elapsed = now_ticks() - t0;

        /*
         * Measured here, not on the host, whose clock includes emulator
         * start-up, the boot and every other command in the list.
         */
        report("--- rc %ld", rc);
        report(", %ld", elapsed / TICKS_PER_SECOND);
        report(".%02ld s", (elapsed % TICKS_PER_SECOND) * 2);

        /*
         * AmigaOS reclaims nothing when a process exits, so the free total after
         * every command is the leak measurement: a leak shows as the same step
         * down, run after run, and nothing else does.
         */
        report(", free %ld\n", (LONG)AvailMem(MEMF_ANY));
    }

    if (IntuitionBase != NULL)
        CloseLibrary((struct Library *)IntuitionBase);

    return RETURN_OK;
}
