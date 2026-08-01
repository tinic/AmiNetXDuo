/*
 * ClientRun -- run a ported Unix client under tools/fsuae-run.sh.
 *
 * tools/fsuae-run.sh starts one executable with no arguments, so anything that
 * takes a command line needs a driver in the middle.  src/tools/toolssmoke.c
 * is that driver for our own commands; this is a separate one because of the
 * stack.
 *
 * A Kickstart 3.1 Shell gives a command 4,096 bytes of stack.  Our own
 * commands are written to that budget -- src/tools/fetch.c allocates its own
 * 64 KB stack and StackSwap()s onto it before opening tls.library.  curl was
 * written for a machine where the stack is 8 MB and grows on demand, and its
 * crt0 here has no __stack hook to ask for more with (this toolchain's crt0.o
 * exports no such symbol).  Running it on a Shell's 4 KB produces an illegal
 * instruction at a random address some seconds later, on a machine with no
 * memory protection.
 *
 * So every command started here gets NP_StackSize.  System() passes unknown
 * tags through to CreateNewProc(), which is the documented route.  A human
 * typing `curl` at a Shell prompt needs `stack 200000` first, which is what
 * the run report says.
 *
 * Reads DH0:commands.txt, one command per line ('#' and blank ignored), runs
 * each through SystemTagList() with output appended to DH0:client.txt, and
 * writes the return code after each.  The host prints DH0:client.txt back.
 *
 * Two lines are not commands:
 *
 *     &<command>   start it and do not wait.  Its output goes to
 *                  DH0:server.txt rather than the shared report, so a
 *                  long-running process writing while the next command
 *                  writes does not interleave two streams into one file.
 *     wait <n>     Delay(n seconds).
 *     ><command>   run it with a real AmigaDOS console as its INPUT.
 *     R<command>   the same, and RESIZE that console part-way through.
 *     <<command>   run it with DH0:stdin.txt as its input.
 *
 * The first two exist for running a server and a client in the same guest.
 * FS-UAE's SLIRP has no inbound path, so a server on the Amiga cannot be
 * reached from the host, and the only way to make it accept a connection is to
 * make the connection from inside.  That needs one command running while
 * another starts, which a list of synchronous SystemTagList() calls cannot
 * express.
 *
 * SYS_Asynch means the child, not us, closes SYS_Input and SYS_Output, so they
 * are opened fresh for each async command and never handed the same handle
 * twice.
 *
 * WHY A CONSOLE IS ITS OWN KIND OF COMMAND
 *
 * An SSH client decides whether it has a terminal by asking IsInteractive()
 * about its own input, and everything downstream follows: raw mode, the size it
 * puts in pty-req, whether it listens for resize events at all.  Redirected
 * from NIL: it is not interactive and none of that runs, so a run that never
 * opens a console cannot say anything about terminal handling.
 *
 * '>' opens CON: as SYS_Input and leaves SYS_Output pointing at the report, so
 * the terminal is real and every byte the session prints still lands in a file
 * on the host.  The window spec comes from DH0:console.txt, one per '>'
 * command in order, so the harness picks the size without rebuilding this.
 *
 * Before handing the console over it asks the console its own size -- CSI '0 q'
 * is the AmigaDOS Window Bounds Report -- and writes the answer to the report.
 * That is the number the remote end's `stty size` has to agree with, and
 * without it a comparison has only one side.
 *
 * WHY RESIZE IS HERE AND NOT IN A HELPER
 *
 * An SSH client is supposed to notice its window changing and send
 * window-change.  Nobody can drag a window in a headless emulator, so the
 * resize has to be programmatic -- and the process that opened CON: is the one
 * holding the handle whose fh_Arg1 is the Intuition Window, so it is the one
 * that can do it without guessing.  'R' starts the command asynchronously,
 * waits RESIZE_DELAY seconds, calls ChangeWindowBox(), reports the console's
 * new size and waits for the command to finish.  The remote end samples its
 * own size either side of that, and the two pairs have to agree.
 *
 * SPDX-License-Identifier: MIT
 */

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

/*
 * 512 KB, and deliberately far more than any client needs.
 *
 * This is the harness, not the product: a client that runs out of stack here
 * fails in a way that looks like the thing under test, so the number is set to
 * make that impossible rather than to be right.  DOS frees it with the process,
 * so it costs nothing between commands.
 *
 * The real budget is AMIGA_ARGV_STACK in clients/compat/amiga_argv.c, which
 * every client swaps onto and which a user gets on a bare Shell.  That one is
 * measured; see AMIGA_ARGV_STACKCHECK, which main() below turns on.
 */
#define CLIENT_STACK    (512UL * 1024UL)

#define MAX_COMMANDS    24

/* 512, not 320.  A command carrying an absolute host path -- which anything
   running something on the other end of an SSH session does -- goes past 320
   easily, and copy_bounded() then drops the line and the NEXT one starts a
   command in the middle of a quoted string.  That does not fail loudly: it
   runs, returns 10, and looks like the program under test refusing an
   argument. */
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

/* SYS_Input is filled in per command.  System() takes ownership of it, which
   is why a fresh handle is opened for every console rather than one being
   reused.  No SYS_Output: the child's output goes where every other command's
   does, so the input is a terminal and the output is still a file on the host. */
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

/*
 * The Intuition Window behind a CON: filehandle.
 *
 * fh_Arg1 is NOT it -- tried, and it yields a pointer whose Width and Height
 * read 35 and 9572, which is somebody else's memory.  On a machine with no
 * protection, calling ChangeWindowBox() through that is a wild write.
 *
 * The documented route is ACTION_DISK_INFO to the console handler, which fills
 * an InfoData whose id_VolumeNode is the Window.  The result is still sanity
 * checked before anything is written through it: a plausible window is at
 * least a character wide and no larger than any screen this machine can open.
 */
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

/*
 * Ask the console how big it is and record the answer.
 *
 * CSI '0 q' is the AmigaDOS Window Bounds Report; the console replies
 * CSI '1;1;<rows>;<cols>' ' r'.  Raw mode first, because the reply is a
 * character stream nobody typed and a cooked console would hold it until a
 * newline that never comes.
 *
 * WaitForChar has a timeout on every read, so a console that answers nothing
 * costs a fifth of a second and reports "no reply" rather than hanging the run.
 */
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

    /* CSI 1 ; 1 ; rows ; cols SPACE r -- five bytes of preamble, exactly as
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

    /* Ask every client for its stack high-water mark.  DOS copies the local
       variable list into each child, and clients/compat/amiga_argv.c paints
       the 256 KB stack and reports the deepest word touched when it sees this.
       Set here rather than in the guest's environment because envsetup builds
       ENV: fresh on every boot and the test HD has no SetEnv to write it. */
    (VOID)SetVar((CONST_STRPTR)"AMIGA_ARGV_STACKCHECK", (CONST_STRPTR)"1",
                 -1, LV_VAR);

    /* Only the 'R' form needs it, and a machine without it simply cannot be
       asked to resize a window -- which is reported rather than fatal. */
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

        /* wait <n> -- seconds, so an async server has time to bind and listen
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

        /* &<command> -- start it and carry on.  Nothing waits for it, so
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
         * Rcommand -- console input, and the window changes size under the
         * command while it runs.
         *
         * The child is started asynchronously so this process is still awake
         * to move the window.  fh_Arg1 of a CON: filehandle is its Intuition
         * Window; that is how the resize happens without guessing which window
         * on the screen belongs to us.
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

            /*
             * OUTPUT HAS TO BE THE CONSOLE TOO, and that is the whole reason
             * this arm exists in this shape.
             *
             * An SSH client turns console resize reporting ON by WRITING an
             * escape sequence -- BebboSSH writes "\x1b[2;11;12{" to its
             * stdout.  Point stdout at a file and that sequence lands in the
             * file, the console is never told to report anything, and the
             * window can then be resized all day with nothing to notice it.
             * The first attempt here did exactly that and produced a confident
             * "resize is not propagated" which was this harness's fault.
             *
             * A second handle on the SAME window comes from Open("*") with
             * pr_ConsoleTask pointing at that console -- two handles the child
             * can close independently, one window.  Opening the CON: spec
             * twice would give two windows.
             *
             * The session's own output is therefore not captured for this arm.
             * It does not need to be: the probe on the other end writes its
             * answers into files on the build host.
             */
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
               because the child holds the handle.  ChangeWindowBox() is
               asynchronous and Intuition clamps it to the window's limits, so
               the size that matters is the one the console reports afterwards
               and not the one asked for here. */
            report("--- window before: %ld", (LONG)win->Width);
            report("x%ld pixels\n", (LONG)win->Height);

            ChangeWindowBox(win, win->LeftEdge, win->TopEdge,
                            RESIZE_W, RESIZE_H);

            /* ChangeWindowBox() is a request, not a change: Intuition performs
               it when it next runs and clamps it to the window's own limits.
               So the window is measured again afterwards, and THAT is what
               says whether anything moved.  Without it, "the remote size did
               not change" cannot be told apart from "the window did not
               change". */
            Delay(2 * TICKS_PER_SECOND);
            report("--- window after:  %ld", (LONG)win->Width);
            report("x%ld pixels\n", (LONG)win->Height);

            Delay((ULONG)RESIZE_SETTLE * TICKS_PER_SECOND);
            elapsed = now_ticks() - t0;

            report("--- waited %ld", elapsed / TICKS_PER_SECOND);
            report(".%02ld s\n", (elapsed % TICKS_PER_SECOND) * 2);
            continue;
        }

        /* >command -- a real console as its input, so IsInteractive() is
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
               goes where every other command's does -- the report file on the
               host -- and the terminal under test is still a terminal. */
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
         * Elapsed time is measured here rather than on the host, whose clock
         * includes emulator start-up, the boot and every other command in the
         * list.  A command that takes thirty seconds when it should take one
         * disappears into a wall-clock total.
         */
        report("--- rc %ld", rc);
        report(", %ld", elapsed / TICKS_PER_SECOND);
        report(".%02ld s", (elapsed % TICKS_PER_SECOND) * 2);

        /*
         * AmigaOS reclaims nothing when a process exits, so anything a client
         * allocated and did not free is gone until the next reboot -- and a
         * ported client leaves through exit(), which is a longjmp() away from
         * the frame that owns its 256 KB stack.  Printing the free total after
         * every command turns this list into the measurement: a leak shows as
         * the same step down, run after run, and nothing else does.
         */
        report(", free %ld\n", (LONG)AvailMem(MEMF_ANY));
    }

    if (IntuitionBase != NULL)
        CloseLibrary((struct Library *)IntuitionBase);

    return RETURN_OK;
}
