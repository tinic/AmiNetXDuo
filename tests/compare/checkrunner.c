/*
 * CheckRunner -- the guest half of every harness in tests/compare.
 *
 *   A harness points a command at bsdsocket.library and asks whether the stack
 *   survived, so the driver has to keep running when the command does not.
 *   Everything it produces is appended and flushed one line at a time: when a
 *   transfer takes the machine down, the file already on the host is the whole
 *   evidence, and a buffered report would lose the line that mattered.
 *
 *   For the same reason it scores nothing.  The expectations live on the host,
 *   so a run that dies in the middle is scored the same way as one that
 *   finishes -- every case with no result line is a failure, and the first of
 *   them names the command that did it.
 *
 *   Written for a curl suite that no longer exists; nothing about it was ever
 *   curl-specific.  It runs a list of commands and writes down what happened,
 *   which is what run-compare.sh uses it for.
 *
 * Input: DH0:checks.txt, one per line
 *
 *   #  ...                 comment
 *   wait N                 sleep N seconds (nothing here needs it yet; the
 *                          nettools harness grew one and it costs four lines)
 *   NAME<TAB>COMMAND       run COMMAND, calling the result NAME
 *
 *   DH0:results.txt        NAME rc ticks availmem   -- one line per case
 *   DH0:w/NAME.txt         that command's own stdout and stderr
 *   DH0:checkrunner.txt    the same thing for a human
 *
 *   "Crashes most of the time" and "degrades over a few hundred transfers"
 *   look identical from a single run, and the second is invisible unless
 *   somebody writes the number down.  AvailMem(MEMF_ANY) after each command,
 *   with the child gone and its memory returned, turns a leak of a few
 *   kilobytes per socket into a straight line the host can fit.  Allocation
 *   patterns fragment, so it is not noise-free, but a leak is a trend and
 *   fragmentation is not.
 *
 *   Every command gets 512 Kb of stack because a Kickstart 3.1 Shell gives a
 *   command 4,096 bytes and this toolchain's crt0 exports no __stack hook to
 *   ask for more.  clients/curl/clientrun.c is the same trick for the same
 *   reason; this file is separate because a verification driver wants
 *   results.txt and a demo driver does not.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: CheckRunner 1.0 (25.7.2026)";

#define CHECKS      "DH0:checks.txt"
#define RESULTS     "DH0:results.txt"
#define REPORT      "DH0:checkrunner.txt"

#define MAX_LINE    640
#define CLIENT_STACK    (512UL * 1024UL)

/*
 * NP_WindowPtr = -1 turns "Please insert volume Foo:" into a failed call.
 * A client that names a path on a volume or assign which does not exist --
 * AmiSSL is configured OPENSSLDIR=AmiSSL:, so any AmiSSL-linked binary does
 * this if the assign is missing -- otherwise puts up a requester on a machine
 * with nobody at the keyboard, and the run sits there until the harness times
 * out.
 */
static struct TagItem client_tags[] =
{
    { NP_StackSize, CLIENT_STACK },
    { NP_WindowPtr, (ULONG)-1    },
    { TAG_END,      0            }
};

static char line[MAX_LINE];
static char command[MAX_LINE + 128];

static LONG now_ticks(VOID)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return ds.ds_Minute * (60L * TICKS_PER_SECOND) + ds.ds_Tick;
}

/* Append one formatted line and close.  The host has to be able to read this
   file while the emulator is still running, and after it has stopped in a way
   that never reached an exit. */
static VOID emit(const char *file, const char *fmt, LONG *args)
{
    BPTR out = Open((CONST_STRPTR)file, MODE_READWRITE);

    if (out == (BPTR)0)
        return;

    Seek(out, 0, OFFSET_END);
    VFPrintf(out, (CONST_STRPTR)fmt, args);
    Close(out);
}

static ULONG append(char *dst, ULONG dstlen, ULONG used, const char *src)
{
    ULONG i = 0;

    while (src[i] != '\0' && used + 1 < dstlen)
        dst[used++] = src[i++];

    dst[used] = '\0';
    return used;
}

/*
 * A third-party curl built against AmiSSL wants an AmiSSL: assign, because
 * amisslmaster.library resolves the versioned library through it.  A bare
 * directory hard drive has no C:assign to type it with, so the driver makes
 * the assign when the directory is staged and does nothing when it is not.
 * Without it a third-party AmiSSL-linked binary cannot be run at all.
 */
static VOID maybe_assign(const char *name, const char *dir)
{
    BPTR lock = Lock((CONST_STRPTR)dir, SHARED_LOCK);

    if (lock == (BPTR)0)
        return;

    if (AssignLock((CONST_STRPTR)name, lock) == DOSFALSE)
        UnLock(lock);
}

static VOID truncate_file(const char *name)
{
    BPTR out = Open((CONST_STRPTR)name, MODE_NEWFILE);

    if (out != (BPTR)0)
        Close(out);
}

int main(int argc, char **argv)
{
    BPTR  in;
    ULONG ran = 0;

    (VOID)argc;
    (VOID)argv;

    /* Our own process too: Lock() below would otherwise be able to ask. */
    {
        struct Process *self = (struct Process *)FindTask(NULL);

        if (self->pr_Task.tc_Node.ln_Type == NT_PROCESS)
            self->pr_WindowPtr = (APTR)-1;
    }

    truncate_file(RESULTS);
    truncate_file(REPORT);
    maybe_assign("AmiSSL", "DH0:AmiSSL");
    /* Same reason, for a stack that reads its configuration through an assign
       of its own (tests/compare/run-compare.sh). */
    maybe_assign("AmiTCP", "DH0:AmiTCP");

    in = Open((CONST_STRPTR)CHECKS, MODE_OLDFILE);
    if (in == (BPTR)0)
    {
        LONG args[1];

        args[0] = (LONG)CHECKS;
        emit(REPORT, "CheckRunner: cannot open %s\n", args);
        return RETURN_FAIL;
    }

    while (FGets(in, (STRPTR)line, (ULONG)sizeof(line)) != NULL)
    {
        char  *name;
        char  *cmd;
        ULONG  i;
        ULONG  used;
        LONG   t0;
        LONG   elapsed;
        LONG   rc;
        LONG   args[6];

        for (i = 0; line[i] != '\0'; i++)
        {
            if (line[i] == '\n' || line[i] == '\r')
            {
                line[i] = '\0';
                break;
            }
        }

        if (line[0] == '\0' || line[0] == '#')
            continue;

        /* wait N -- seconds */
        if (line[0] == 'w' && line[1] == 'a' && line[2] == 'i' &&
            line[3] == 't' && line[4] == ' ')
        {
            LONG secs = 0;

            for (i = 5; line[i] >= '0' && line[i] <= '9'; i++)
                secs = secs * 10 + (line[i] - '0');
            if (secs > 0)
                Delay(secs * TICKS_PER_SECOND);
            continue;
        }

        name = line;
        cmd  = (char *)0;
        for (i = 0; line[i] != '\0'; i++)
        {
            if (line[i] == '\t')
            {
                line[i] = '\0';
                cmd = &line[i + 1];
                break;
            }
        }
        if (cmd == (char *)0 || cmd[0] == '\0')
            continue;

        /*
         * Each command's own output goes to its own file, so a -w line and a
         * curl error message can be attributed to the case that produced
         * them.  <NIL: because nothing here reads standard input and a
         * command that decided to would otherwise hang the whole run on a
         * console that has no keyboard.
         */
        used = 0;
        used = append(command, (ULONG)sizeof(command), used, cmd);
        used = append(command, (ULONG)sizeof(command), used, " <NIL: >>DH0:w/");
        used = append(command, (ULONG)sizeof(command), used, name);
        used = append(command, (ULONG)sizeof(command), used, ".txt");
        (VOID)used;

        args[0] = (LONG)name;
        args[1] = (LONG)cmd;
        emit(REPORT, "\n--- %s\n    %s\n", args);

        t0 = now_ticks();
        rc = SystemTagList((CONST_STRPTR)command, client_tags);
        elapsed = now_ticks() - t0;
        if (elapsed < 0)
            elapsed = 0;                    /* midnight, once a run */

        args[0] = (LONG)name;
        args[1] = rc;
        args[2] = elapsed;
        args[3] = (LONG)AvailMem(MEMF_ANY);
        emit(RESULTS, "%s %ld %ld %ld\n", args);

        args[0] = rc;
        args[1] = elapsed / TICKS_PER_SECOND;
        args[2] = (elapsed % TICKS_PER_SECOND) * 2;
        args[3] = (LONG)AvailMem(MEMF_ANY);
        emit(REPORT, "--- rc %ld, %ld.%02ld s, %ld bytes free\n", args);

        ran++;
    }

    Close(in);

    {
        LONG args[1];

        args[0] = (LONG)ran;
        emit(REPORT, "\n=== CheckRunner ran %ld commands\n", args);
        Printf((CONST_STRPTR)"CheckRunner ran %ld commands\n", (LONG)ran);
    }

    return RETURN_OK;
}
