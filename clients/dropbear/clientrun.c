/*
 * ClientRun -- run a ported Unix client under tools/fsuae-run.sh.
 *
 * WHY THIS EXISTS AND WHY IT IS NOT ToolsSmoke
 *
 *   tools/fsuae-run.sh starts ONE executable with no arguments, so anything
 *   that takes a command line needs a driver in the middle.  src/tools/
 *   toolssmoke.c is that driver for our own commands and this is deliberately
 *   a separate one, for a reason that is the whole point of the file:
 *
 *       A Kickstart 3.1 Shell gives a command 4,096 bytes of stack.
 *
 *   Our own commands are written to that budget -- src/tools/fetch.c goes as
 *   far as allocating its own 64 KB stack and StackSwap()ping onto it before
 *   it opens tls.library.  curl was written for a machine where the stack is
 *   8 MB and grows on demand, and its crt0 here has no __stack hook to ask
 *   for more with (checked: this toolchain's crt0.o exports no such symbol).
 *   Running it on a Shell's 4 KB is not a crash, it is an illegal instruction
 *   at a random address some seconds later, on a machine with no memory
 *   protection.
 *
 *   So every command started here gets NP_StackSize.  System() passes unknown
 *   tags through to CreateNewProc(), which is the documented route.
 *
 *   The same applies to a human typing `curl` at a Shell prompt: they need
 *   `stack 200000` first.  That is ordinary Amiga practice for a ported
 *   program and it is what the run report says.
 *
 * WHAT IT DOES
 *
 *   Reads DH0:commands.txt, one command per line ('#' and blank ignored), runs
 *   each through SystemTagList() with output appended to DH0:client.txt, and
 *   writes the return code after each.  The host prints DH0:client.txt back.
 *
 *   Two lines are not commands:
 *
 *       &<command>   start it and do NOT wait.  Its output goes to
 *                    DH0:server.txt rather than the shared report, so a
 *                    long-running process writing while the next command
 *                    writes does not interleave two streams into one file.
 *       wait <n>     Delay(n seconds).
 *
 *   Both exist for one case: running a SERVER and a client in the same guest.
 *   FS-UAE's SLIRP has no inbound path, so a server on the Amiga cannot be
 *   reached from the host at all, and the only way to make it accept a
 *   connection is to make the connection from inside -- dropbear listening on
 *   127.0.0.1 and dbclient connecting to it.  That needs one command running
 *   while another starts, which is the one thing a list of synchronous
 *   SystemTagList() calls cannot express.
 *
 *   SYS_Asynch means the child, not us, closes SYS_Input and SYS_Output, so
 *   they are opened fresh for each async command and never handed the same
 *   handle twice.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: ClientRun 1.0 (25.7.2026)";

#define REPORT      "DH0:client.txt"

/* A separate file for async output.  Sharing REPORT would interleave a
   server's log with the client's, in a file both have open for append. */
#define ASYNC_REPORT "DH0:server.txt"
#define COMMANDS    "DH0:commands.txt"
#define REDIRECT    " <NIL: >>DH0:client.txt"

/*
 * 512 KB.  curl's own stack use is not published and is not small: the URL
 * parser, the multi state machine and the printf family all recurse a little,
 * and it is a Fast RAM allocation on a machine assumed to have four megabytes
 * that lasts only as long as the command.  Over-provision and move on --
 * the alternative to too much stack here is an unreproducible crash.
 *
 * It was 256 KB before the TLS backend.  A program that opens tls.library is
 * bracketed into ThreadX on ITS OWN STACK (port/threadx-amiga/src/
 * tx_amiga_adopt.c hands _tx_thread_create() the caller's stack region), so
 * NetX Duo, nx_secure and the bignum code all run on whatever this process has
 * left -- which is why src/tools/fetch.c allocates 64 KB and StackSwap()s onto
 * it before its own TLSOpen().  curl cannot do that; it has no idea it is on
 * AmigaOS.  So the budget has to be here, and doubling it costs nothing.
 */
#define CLIENT_STACK    (512UL * 1024UL)

#define MAX_COMMANDS    24
#define MAX_LINE        320

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

int main(int argc, char **argv)
{
    BPTR  out;
    ULONG i;
    LONG  rc;

    (VOID)argc;
    (VOID)argv;

    /* Truncate both reports once, so a rerun does not append to the last run.
       The async one is truncated even when nothing async runs -- otherwise a
       run with no server would print the PREVIOUS run's server log. */
    out = Open((CONST_STRPTR)REPORT, MODE_NEWFILE);
    if (out != (BPTR)0)
        Close(out);

    out = Open((CONST_STRPTR)ASYNC_REPORT, MODE_NEWFILE);
    if (out != (BPTR)0)
        Close(out);

    read_commands();

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

        /* &<command> -- start it and carry on.  There is no return code to
           report, because the point is that we did not wait for one. */
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

        report("\n--- %s\n", (LONG)lines[i]);

        used = 0;
        used = append(command, (ULONG)sizeof(command), used, lines[i]);
        used = append(command, (ULONG)sizeof(command), used, REDIRECT);
        (VOID)used;

        t0 = now_ticks();
        rc = SystemTagList((CONST_STRPTR)command, client_tags);
        elapsed = now_ticks() - t0;

        /*
         * The elapsed time is here because the alternative is measuring on
         * the host, and the host's clock includes emulator start-up, the
         * boot and every other command in the list.  A command that takes
         * thirty seconds when it should take one is the kind of thing that
         * hides in a wall-clock total.
         */
        report("--- rc %ld", rc);
        report(", %ld", elapsed / TICKS_PER_SECOND);
        report(".%02ld s\n", (elapsed % TICKS_PER_SECOND) * 2);
    }

    return RETURN_OK;
}
