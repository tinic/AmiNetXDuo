/*
 * CurlCheck -- the Amiga half of the curl verification suite.
 *
 * WHAT IT IS FOR
 *
 *   The suite points curl at bsdsocket.library and asks whether the STACK
 *   survived, so the driver has to be the thing that keeps running when a
 *   command does not.  Everything it produces is appended and flushed one
 *   line at a time, on purpose: when a transfer takes the machine down, the
 *   file that is already on the host is the entire evidence, and a buffered
 *   report would lose exactly the line that mattered.
 *
 *   For the same reason it does not score anything.  The expectations live on
 *   the host (tests/curl/curlsuite.py) with the body hashes and the -w
 *   metrics, so a run that dies in the middle is scored the same way as one
 *   that finishes -- every case with no result line is a failure, and the
 *   first of them names the command that did it.
 *
 * INPUT: DH0:checks.txt, one per line
 *
 *   #  ...                 comment
 *   wait N                 sleep N seconds (nothing here needs it yet; the
 *                          nettools harness grew one and it costs four lines)
 *   NAME<TAB>COMMAND       run COMMAND, calling the result NAME
 *
 * OUTPUT
 *
 *   DH0:results.txt        NAME rc ticks availmem   -- one line per case
 *   DH0:w/NAME.txt         that command's own stdout and stderr
 *   DH0:curlcheck.txt      the same thing for a human
 *
 * WHY availmem IS IN THE RESULT LINE
 *
 *   "Crashes most of the time" and "degrades over a few hundred transfers"
 *   look identical from a single run, and the second one is invisible unless
 *   somebody writes the number down.  AvailMem(MEMF_ANY) after each command,
 *   with the child gone and its memory returned, turns a leak of a few
 *   kilobytes per socket into a straight line the host can fit.  It is not
 *   free of noise -- allocation patterns fragment -- but a leak is a trend and
 *   fragmentation is not.
 *
 * WHY EVERY COMMAND GETS 512 KB OF STACK
 *
 *   A Kickstart 3.1 Shell gives a command 4,096 bytes and this toolchain's
 *   crt0 exports no __stack hook to ask for more.  See clients/curl/
 *   clientrun.c, which is the same trick for the same reason; this file is a
 *   separate one because a verification driver wants results.txt and a demo
 *   driver does not.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char version_tag[] __attribute__((used)) =
    "$VER: CurlCheck 1.0 (25.7.2026)";

#define CHECKS      "DH0:checks.txt"
#define RESULTS     "DH0:results.txt"
#define REPORT      "DH0:curlcheck.txt"

#define MAX_LINE    640
#define CLIENT_STACK    (512UL * 1024UL)

static struct TagItem client_tags[] =
{
    { NP_StackSize, CLIENT_STACK },
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

/* Append one formatted line and close.  Closing is the point: the host has to
   be able to read this file while the emulator is still running, and after it
   has stopped running in a way that never reached an exit. */
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
 * it when the directory is staged and does nothing when it is not.  Five
 * lines, and without them the strongest ABI evidence available -- somebody
 * else's binary, over somebody else's TLS -- cannot be collected at all.
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

    truncate_file(RESULTS);
    truncate_file(REPORT);
    maybe_assign("AmiSSL", "DH0:AmiSSL");

    in = Open((CONST_STRPTR)CHECKS, MODE_OLDFILE);
    if (in == (BPTR)0)
    {
        LONG args[1];

        args[0] = (LONG)CHECKS;
        emit(REPORT, "CurlCheck: cannot open %s\n", args);
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
        emit(REPORT, "\n=== CurlCheck ran %ld commands\n", args);
        Printf((CONST_STRPTR)"CurlCheck ran %ld commands\n", (LONG)ran);
    }

    return RETURN_OK;
}
