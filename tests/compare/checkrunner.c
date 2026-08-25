/*
 * CheckRunner, the guest half of every harness in tests/compare.
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
    /* Commands SystemTagList() could not start at all: a staging fault, and
       not a measurement of the stack under test.  See the return below. */
    ULONG unstarted = 0;

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
        if (rc == -1L)
            unstarted++;
    }

    Close(in);

    {
        LONG args[2];

        args[0] = (LONG)ran;
        args[1] = (LONG)unstarted;
        emit(REPORT, "\n=== CheckRunner ran %ld commands, %ld would not start\n",
             args);
        Printf((CONST_STRPTR)"CheckRunner ran %ld commands, %ld would not "
                             "start\n", (LONG)ran, (LONG)unstarted);
    }

    if (ran == 0)
    {
        emit(REPORT, "CheckRunner: no command in " CHECKS " ran, so nothing "
                     "was measured\n", (LONG *)0);
        return RETURN_ERROR;
    }
    if (unstarted != 0)
        return RETURN_ERROR;

    return RETURN_OK;
}
