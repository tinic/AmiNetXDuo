/*
 * amisslprobe, ask AmiSSL what it is, from inside the guest.
 *
 * A file on DH0: is not a working library.  The snapshot build checks the
 * bytes it copied and the launcher checks the bytes it copied, and both of
 * those are the host reading a file it wrote itself.  This runs on the Amiga
 * and performs the sequence a real application performs:
 *
 *   1. OpenLibrary("amisslmaster.library"), the proxy, ~5 KB;
 *   2. OpenAmiSSLTagList(), which is the proxy's OpenLibrary() of
 *      LIBS:AmiSSL/amissl_v362.library -- 3.5 MB of hunk file that AmigaDOS
 *      reads and relocates, and the reason this program is slow;
 *   3. InitAmiSSLA(), the per-process init;
 *   4. OpenSSL_version(OPENSSL_VERSION), which is a call INTO the library
 *      that returns the string OpenSSL was built with.
 *
 * Step 4 is the point.  Every earlier number could be got from a filename;
 * this one is the library speaking, through a jsr on its own base, and it
 * cannot be right unless the library loaded, relocated and initialised.
 * AmiSSLBase->lib_IdString is reported beside it because it names the CPU
 * build that was loaded, which a filename does not.
 *
 * The memory readings are the other half of the answer.  A versioned AmiSSL
 * library is 3.5 MB and an Amiga is not a machine with 3.5 MB to spare, so
 * "is it there" and "can this machine use it" are different questions and
 * this program answers both.  Free memory is read before the master opens,
 * after the versioned library is in, after per-process init, and after
 * everything closes; the last one against the first says whether the library
 * expunged.
 *
 * THE STACK.  A Kickstart 3.1 Shell gives a command 4,096 bytes and this
 * toolchain's crt0 exports no __stack hook to ask for more (RESEARCH 11.5),
 * which is why tests/compare/checkrunner.c launches its children with an
 * explicit one.  This program cannot launch itself, so it reads the stack it
 * was given and refuses on one too small rather than wandering off the end of
 * it: a stack overflow on this machine is a hang with nothing printed, which
 * is exactly the failure this program exists to distinguish from a slow open.
 *
 *     Stack 65536
 *     C:amisslprobe
 *
 * NO <stdio.h>.  Nothing else that runs in the guest links it, for the reason
 * above -- printf's own buffers do not fit in a Shell's stack -- and every
 * line here goes out through dos.library instead.
 *
 * Output is key=value on stdout and in DH0:amisslprobe.txt, one place because
 * the harness reads a file and one because a human runs it from a Shell.
 * Both are flushed line by line, and that is the whole point of them: the
 * step between two of these lines is the library open, which is minutes.
 * Buffered, the report would arrive at the end and neither the person
 * watching nor the host reading the file could tell a slow open from a
 * wedged one.
 *
 * Exit status is 0 only if every step above succeeded.
 *
 * Built against the AmiSSL SDK -- the same headers tests/crypto68k uses --
 * and linked against nothing of AmiSSL's: every call here is an inline macro
 * that jumps through a library base.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>

#include <proto/amisslmaster.h>
#include <proto/amissl.h>

#include <amissl/amissl.h>

#define REPORT      "DH0:amisslprobe.txt"

/* Enough for the library open and AmiSSL's own per-process init to have room
   under them.  A Shell's 4,096 is not, and the failure it produces says
   nothing, so this is checked rather than hoped for. */
#define STACK_MIN   32768L

static const char version_tag[] __attribute__((used)) =
    "$VER: amisslprobe 1.0 (17.8.2026) AmiNetXDuo";

/* The three bases AmiSSL's inline macros dereference. */
struct Library *AmiSSLMasterBase;
struct Library *AmiSSLBase;
struct Library *AmiSSLExtBase;

/* AmiSSL reaches for this when a socket or C-library call inside the library
   fails.  Nothing here uses the network, but the tag wants an address that
   outlives the call. */
static int      probe_errno;

static BPTR     out;


/* Every report line goes through here, to both places, flushed.  dos.library
   takes its arguments as a LONG array, which is why the callers cast. */
static void emit(const char *fmt, LONG a, LONG b, LONG c)
{

LONG    args[3];


    args[0] = a;
    args[1] = b;
    args[2] = c;

    VPrintf((STRPTR)fmt, args);
    Flush(Output());

    if (out != 0)
    {
        VFPrintf(out, (STRPTR)fmt, args);
        Flush(out);
    }
}


static void say(const char *key, const char *value)
{

    emit("%s=%s\n", (LONG)key, (LONG)value, 0);
}


static void say_num(const char *key, LONG value)
{

    emit("%s=%ld\n", (LONG)key, value, 0);
}


/* Free memory, in kilobytes.  Bytes would be exact and unreadable; the
   question these answer is whether a machine has room for a 3.5 MB library,
   and that is a question about kilobytes. */
static void say_mem(const char *when)
{

    emit("mem_free_k_%s=%ld\n", (LONG)when,
         (LONG)(AvailMem(MEMF_ANY) / 1024), 0);
    emit("mem_largest_k_%s=%ld\n", (LONG)when,
         (LONG)(AvailMem(MEMF_ANY | MEMF_LARGEST) / 1024), 0);
}


/* Ticks since midnight, at 1/50 s.  DateStamp is enough: the interval being
   measured is tens of seconds to minutes, and opening timer.device to resolve
   it further would be measuring the wrong thing. */
static LONG now_ticks(void)
{

struct DateStamp    ds;


    DateStamp(&ds);
    return((ds.ds_Minute * 3000L) + ds.ds_Tick);
}


static void say_elapsed(const char *key, LONG from, LONG to)
{

LONG    ticks;


    ticks = to - from;
    /* Midnight, once a day, and the number would otherwise be negative. */
    if (ticks < 0)
    {
        ticks += 24L * 60L * 3000L;
    }
    emit("%s=%ld.%02ld\n", (LONG)key, ticks / 50L, (ticks % 50L) * 2L);
}


/* A library node's own version, and its own ID string.  Read off the node
   exec built when the library loaded, not off the file it loaded from. */
static void say_library(const char *prefix, struct Library *base)
{

    emit("%s_version=%ld.%ld\n", (LONG)prefix,
         (LONG)base -> lib_Version, (LONG)base -> lib_Revision);
    emit("%s_id=%s\n", (LONG)prefix,
         (LONG)((base -> lib_IdString != NULL)
                ? (const char *)base -> lib_IdString : "none"), 0);
}


int main(void)
{

struct TagItem                  tags[6];
struct Process                 *self;
struct CommandLineInterface    *cli;
LONG                            stack;
LONG                t0;
LONG                t1;
const char         *version;
int                 rc = RETURN_FAIL;


    out = Open((STRPTR)REPORT, MODE_NEWFILE);

    say("probe", "amissl");

    /* Before anything else, because everything else depends on it.
     *
     * NOT pr_StackSize.  A Shell command does not get its own process: it
     * runs inside the Shell's, on a stack the Shell swaps in for it, and the
     * size of that one is cli_DefaultStack -- in LONGWORDS, which is the
     * field's documented and easily-missed unit.  pr_StackSize is the Shell's
     * own, it is 3,200 bytes in the web terminal, and reading it says a
     * command has too little stack when `Stack 65536` has just given it
     * plenty.  A program launched from Workbench has no CLI and pr_StackSize
     * is then the right and only answer.
     */
    self = (struct Process *)FindTask(NULL);
    cli = (struct CommandLineInterface *)BADDR(self -> pr_CLI);
    stack = (cli != NULL) ? ((LONG)cli -> cli_DefaultStack) * 4L
                          : (LONG)self -> pr_StackSize;
    say_num("stack_bytes", stack);
    if (stack < STACK_MIN)
    {
        say("error", "not enough stack: run `Stack 65536` in this Shell first");
        goto done;
    }

    say_mem("start");

    /* ------------------------------------------------------ the master -- */

    t0 = now_ticks();
    AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library",
                                   AMISSLMASTER_MIN_VERSION);
    t1 = now_ticks();
    if (AmiSSLMasterBase == NULL)
    {
        say("error", "LIBS:amisslmaster.library would not open");
        goto done;
    }
    say_elapsed("master_open_seconds", t0, t1);
    say_library("master", AmiSSLMasterBase);

    /* ---------------------------------------------- the versioned library --
     *
     * AmiSSL_InitAmiSSL FALSE splits the load from the per-process init, so
     * the two costs are reported apart.  They are paid at different times by
     * a real program: the library stays resident once loaded and the init
     * does not.
     */

    tags[0].ti_Tag = AmiSSL_UsesOpenSSLStructs; tags[0].ti_Data = (ULONG)FALSE;
    tags[1].ti_Tag = AmiSSL_InitAmiSSL;         tags[1].ti_Data = (ULONG)FALSE;
    tags[2].ti_Tag = AmiSSL_GetAmiSSLBase;      tags[2].ti_Data = (ULONG)&AmiSSLBase;
    tags[3].ti_Tag = TAG_DONE;                  tags[3].ti_Data = 0UL;

    say("opening", "LIBS:AmiSSL/amissl_v362.library");
    t0 = now_ticks();
    if (OpenAmiSSLTagList(AMISSL_CURRENT_VERSION, tags) != 0)
    {
        t1 = now_ticks();
        say_elapsed("library_open_seconds", t0, t1);
        say("error", "OpenAmiSSLTagList failed: is LIBS:AmiSSL/ populated?");
        goto done;
    }
    t1 = now_ticks();
    say_elapsed("library_open_seconds", t0, t1);
    say_library("library", AmiSSLBase);
    say_mem("library_open");

    /* -------------------------------------------------- per-process init -- */

    tags[0].ti_Tag = AmiSSL_GetAmiSSLBase;    tags[0].ti_Data = (ULONG)&AmiSSLBase;
    tags[1].ti_Tag = AmiSSL_GetAmiSSLExtBase; tags[1].ti_Data = (ULONG)&AmiSSLExtBase;
    tags[2].ti_Tag = AmiSSL_ErrNoPtr;         tags[2].ti_Data = (ULONG)&probe_errno;
    tags[3].ti_Tag = TAG_DONE;                tags[3].ti_Data = 0UL;

    t0 = now_ticks();
    if (InitAmiSSLA(tags) != 0)
    {
        t1 = now_ticks();
        say_elapsed("init_seconds", t0, t1);
        say("error", "InitAmiSSLA failed");
        goto done;
    }
    t1 = now_ticks();
    say_elapsed("init_seconds", t0, t1);
    say_mem("init");

    /* ------------------------------------------------ the library speaks -- */

    version = (const char *)OpenSSL_version(OPENSSL_VERSION);
    say("openssl_version", (version != NULL) ? version : "none");

    rc = RETURN_OK;

done:
    if (AmiSSLBase != NULL)
    {
        CloseAmiSSL();
        AmiSSLBase    = NULL;
        AmiSSLExtBase = NULL;
    }
    if (AmiSSLMasterBase != NULL)
    {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
    }

    /* After the close, so the reading says whether the 3.5 MB came back.  It
       does not have to: exec expunges a library only when it needs the
       memory, so a machine with room keeps it loaded and the next opener
       pays nothing. */
    say_mem("closed");
    say("RESULT", (rc == RETURN_OK) ? "PASS" : "FAIL");

    if (out != 0)
    {
        Close(out);
        out = 0;
    }
    return(rc);
}
