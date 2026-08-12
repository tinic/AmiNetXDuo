/*
 * Does the machine survive unloading a program?  For any program.
 *
 * tools/smoke/kernelstop.c asks this question about one binary by re-execing
 * itself.  This asks it about a binary it is handed, so a test that was not
 * written as a parent/child pair can still be put through it:
 *
 *   unloadprobe DH0:ram_driver_test
 *
 * The failure it is built for is the one commit 475311e found in
 * tools/smoke/lifecycle: tx_amiga_kernel_start() installs a VERTB interrupt
 * server whose struct Interrupt, and whose is_Code, live in the program's hunk.
 * AmigaDOS frees that hunk the instant the program exits, and the next VBlank
 * 20 ms later calls through it.  The program has already reported PASS and
 * already returned its exit status by then, so nothing about its own output
 * can tell a program that stopped the kernel from one that did not.
 *
 * Two separate things happen here, and they answer different questions.
 *
 *   THE ASSERTION is exact and does not depend on luck: walk the VERTB server
 *   chain and ask, of each node, whether its address is inside a free MemChunk.
 *   Exec calls through those nodes fifty times a second.  A node sitting in
 *   free memory is the defect stated as a boolean -- it is true on every run,
 *   including the runs where nothing happens to crash.
 *
 *   THE DEMONSTRATION is the crash: fill free memory with the 68000 ILLEGAL
 *   opcode and hold the machine open.  The child's hunk was freed a moment
 *   ago, so the allocations very probably land on top of it, and the next
 *   VBlank calls an is_Code that is now 0x4AFC4AFC.  Without this, freed memory
 *   often still holds the instructions that were there before it was freed and
 *   a broken program passes.
 *
 * The report is written and closed BEFORE the poison goes down, because the
 * interesting runs are the ones where the machine does not come back.
 *
 * Build:
 *
 *   cmake --build build/cm --parallel --target smoke_unloadprobe
 *
 * Run, with the program under test staged alongside it:
 *
 *   AMINETXDUO_RUN_TAG=unl ./tools/amiberry-run.sh -t 240 \
 *       -a 'DH0:ram_driver_test' \
 *       build/cm/tools/smoke/unloadprobe build/cm/tests/ram_driver/ram_driver_test
 *
 * Exit 0 means the child ran, left nothing of the port's behind, and the
 * machine outlived it.  Anything else is described in the report.  A run that
 * takes the machine down produces no exit status at all: amiberry-run.sh reads
 * the illegal instruction out of the emulator log and returns 4.
 *
 * SPDX-License-Identifier: MIT
 */

/* tx_api.h FIRST: exec/types.h does #define VOID void, which collides with
   tx_port.h's typedef void VOID if the Amiga headers land first. */
#include "tx_api.h"
#include "tx_amiga.h"

#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <hardware/intbits.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/compat.h"
#include "aminetxduo/crashguard.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: unloadprobe 1.0 (12.8.2026)";

/* How long to hold the machine open once the child's hunk is gone.  At 50 Hz
   a stale VERTB server gets 400 chances to call into freed memory. */
#define SURVIVE_TICKS   400L

/* A server chain that does not terminate inside this many nodes is itself
   corrupt, and walking it further is how a probe becomes the crash. */
#define MAX_SERVERS     64

/* Anything under this is teardown still in flight, not a leak. */
#define LEAK_TOLERANCE  8192UL
#define SETTLE_TRIES    40
#define SETTLE_TICKS    5L

#define POISON_CHUNKS   8
#define POISON_BYTES    (64UL * 1024UL)

static LONG checks, failures;
static APTR poison[POISON_CHUNKS];

/* Snapshot of the VERTB chain, taken under Forbid() and read afterwards.
   Nothing is printed while the walk holds the machine, and ln_Name is never
   followed for a node that turned out to be in freed memory. */
static struct
{
    APTR    node;
    APTR    code;
    APTR    name;
    BOOL    freed;
} server[MAX_SERVERS];

static LONG servers;


static void check(const char *what, BOOL ok)
{
    checks++;
    if (!ok)
        failures++;
    Printf((CONST_STRPTR)"  %s %s\n", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
    AMI_ERROR("  %s %s", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what);
}

static void checkv(const char *what, BOOL ok, LONG value)
{
    checks++;
    if (!ok)
        failures++;
    Printf((CONST_STRPTR)"  %s %s (%ld)\n", (LONG)(ok ? "ok  " : "FAIL"),
           (LONG)what, value);
    AMI_ERROR("  %s %s (%ld)", (LONG)(ok ? "ok  " : "FAIL"), (LONG)what, value);
}


/*
 * Is this address inside a chunk on some memory header's FREE list?
 *
 * Caller holds Forbid().  Exec's free list is the authority on what has been
 * given back; an address that appears on it is memory nobody owns, and a
 * pointer exec itself is still following into it is the whole defect.
 */
static BOOL addr_is_free(APTR addr)
{
    struct MemHeader *mh;

    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ != NULL;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
    {
        struct MemChunk *mc;

        if (addr < mh->mh_Lower || addr >= mh->mh_Upper)
            continue;

        for (mc = mh->mh_First; mc != NULL; mc = mc->mc_Next)
        {
            if ((ULONG)addr >= (ULONG)mc &&
                (ULONG)addr < ((ULONG)mc + mc->mc_Bytes))
                return TRUE;
        }
    }

    return FALSE;
}

/*
 * Snapshot the VERTB server chain.
 *
 * IntVects[INTB_VERTB].iv_Data is the list header, and it belongs to exec, so
 * it survives anything a program does.  The NODES are the callers' own struct
 * Interrupt, which is what makes this worth looking at: ours is in the hunk.
 */
static void snapshot_vertb(void)
{
    struct List *chain;
    struct Node *node;

    servers = 0;

    Forbid();

    chain = (struct List *)SysBase->IntVects[INTB_VERTB].iv_Data;
    if (chain != NULL && chain->lh_Head != NULL)
    {
        node = chain->lh_Head;
        while (servers < MAX_SERVERS && node != NULL && node->ln_Succ != NULL)
        {
            struct Interrupt *is = (struct Interrupt *)node;

            server[servers].node  = (APTR)node;
            server[servers].code  = (APTR)is->is_Code;
            server[servers].name  = (APTR)node->ln_Name;
            server[servers].freed = addr_is_free((APTR)node);
            servers++;

            node = node->ln_Succ;
        }
    }

    Permit();
}

static LONG report_vertb(void)
{
    LONG bad = 0;
    LONG i;

    for (i = 0; i < servers; i++)
    {
        if (server[i].freed)
        {
            bad++;
            /* ln_Name is in the same freed hunk; do not follow it. */
            Printf((CONST_STRPTR)"       VERTB server at %08lx is in FREE memory, "
                                 "is_Code %08lx\n",
                   (LONG)server[i].node, (LONG)server[i].code);
            AMI_ERROR("  VERTB server at %08lx is in FREE memory, is_Code %08lx",
                      (LONG)server[i].node, (LONG)server[i].code);
        }
        else
        {
            Printf((CONST_STRPTR)"       VERTB server at %08lx  %s\n",
                   (LONG)server[i].node,
                   (LONG)(server[i].name != NULL ? server[i].name : (APTR)"(unnamed)"));
        }
    }

    return bad;
}


/*
 * How much of `before` is still out, once the machine has stopped handing it
 * back.  A Process's stack, CLI and Process structure go back after System()
 * has returned to its caller, so a single AvailMem() reads whatever teardown
 * is still in flight.  A real leak never comes back and still fails.
 */
static ULONG settled_bytes_out(ULONG before)
{
    ULONG out = 0;
    int   n;

    for (n = 0; n < SETTLE_TRIES; n++)
    {
        ULONG now = AvailMem(MEMF_PUBLIC);

        out = (before > now) ? (before - now) : 0UL;
        if (out < LEAK_TOLERANCE)
            break;
        Delay(SETTLE_TICKS);
    }

    return out;
}

static void poison_free_memory(void)
{
    ULONG i, w;

    for (i = 0; i < POISON_CHUNKS; i++)
    {
        poison[i] = AllocMem(POISON_BYTES, MEMF_PUBLIC);
        if (poison[i] == NULL)
            continue;
        for (w = 0; w < POISON_BYTES / 2UL; w++)
            ((UWORD *)poison[i])[w] = 0x4AFC;    /* ILLEGAL */
    }
}

static void poison_release(void)
{
    ULONG i;

    for (i = 0; i < POISON_CHUNKS; i++)
    {
        if (poison[i] != NULL)
        {
            FreeMem(poison[i], POISON_BYTES);
            poison[i] = NULL;
        }
    }
}


/*
 * The command to run, from dos.library rather than from argv.
 *
 * A guest program started by the Startup-Sequence arrives with argc == 1
 * whatever its startup code did, so argv cannot carry this.  GetArgStr() is
 * the Shell's own argument string; it keeps the trailing newline, which has to
 * come off before it is pasted into a command line.
 */
static char command[256];

static BOOL take_command(void)
{
    CONST_STRPTR args = (CONST_STRPTR)GetArgStr();
    ULONG        n = 0;

    if (args == NULL)
        return FALSE;

    while (args[n] != '\0' && args[n] != '\n' && n < (sizeof(command) - 64UL))
    {
        command[n] = (char)args[n];
        n++;
    }

    /* Trailing blanks would land between the command and the redirection. */
    while (n > 0 && (command[n - 1] == ' ' || command[n - 1] == '\t'))
        n--;

    command[n] = '\0';
    if (n == 0)
        return FALSE;

    /* The child's own output goes to a file the harness already dumps, so it
       does not interleave with this report on the way to the serial log. */
    {
        static const char redirect[] = " <NIL: >>DH0:unloadchild.txt";
        ULONG             i;

        for (i = 0; redirect[i] != '\0'; i++)
            command[n + i] = redirect[i];
        command[n + i] = '\0';
    }

    return TRUE;
}


int main(int argc, char **argv)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR            old_window;
    BPTR            fh;
    LONG            rc;
    ULONG           mem_before, mem_out;
    LONG            stale;

    (void)argc;
    (void)argv;

    Printf((CONST_STRPTR)"AmiNetXDuo, does the machine survive unloading this "
                         "program?\n");

    if (!take_command())
    {
        PutStr((CONST_STRPTR)"usage: unloadprobe <command to run>\n");
        return RETURN_ERROR;
    }

    Printf((CONST_STRPTR)"  child: %s\n", (LONG)command);
    AMI_ERROR("=== unloadprobe: %s", (LONG)command);

    ami_crash_set_reference((APTR)main, "unloadprobe main");
    check("crash guard installed", ami_crash_install());
    check("Alert (Guru) hook installed", ami_crash_install_alert_hook());

    /* Nobody is at the keyboard to answer a requester. */
    old_window       = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;

    fh = Open((CONST_STRPTR)"DH0:unloadchild.txt", MODE_NEWFILE);
    if (fh != (BPTR)0)
        Close(fh);

    mem_before = AvailMem(MEMF_PUBLIC);

    rc = SystemTags((CONST_STRPTR)command, TAG_DONE);
    me->pr_WindowPtr = old_window;

    checkv("the child ran and exited normally", rc == RETURN_OK, rc);

    /* ---- the child's hunk is freed now.  Who is still pointing into it? --- */

    check("no ThreadX tick Task outlived the child",
          FindTask((STRPTR)"ThreadX tick") == NULL);
    check("no ThreadX scheduler Task outlived the child",
          FindTask((STRPTR)"ThreadX") == NULL);

    /*
     * The assertion this probe exists for.  Exec calls every one of these
     * fifty times a second; one of them being in memory that has been handed
     * back is not a risk, it is a wild jump that has not happened yet.
     */
    snapshot_vertb();
    stale = report_vertb();
    checkv("no VERTB interrupt server is in freed memory", stale == 0, stale);

    mem_out = settled_bytes_out(mem_before);
    checkv("the child gave its memory back (bytes still out)",
           mem_out < LEAK_TOLERANCE, (LONG)mem_out);

    /* ---- write the verdict BEFORE provoking the crash -------------------- */

    Printf((CONST_STRPTR)"\n%ld checks, %ld failure(s), %s\n", checks, failures,
           (LONG)(failures == 0 ? "PASS" : "FAIL"));
    AMI_ERROR("=== unloadprobe: %ld checks, %ld failures, %s", checks, failures,
              (LONG)(failures == 0 ? "PASS" : "FAIL"));

    Flush(Output());

    /* ---- and now outlive the child --------------------------------------- */

    Printf((CONST_STRPTR)"  .. %ld ticks with freed memory poisoned\n",
           (LONG)SURVIVE_TICKS);
    AMI_ERROR("unloadprobe: poisoning freed memory, waiting %ld ticks",
              (LONG)SURVIVE_TICKS);
    Flush(Output());

    poison_free_memory();
    Delay(SURVIVE_TICKS);
    poison_release();

    check("the machine is still alive well after the child unloaded", TRUE);

    Printf((CONST_STRPTR)"\n%ld checks, %ld failure(s), %s\n", checks, failures,
           (LONG)(failures == 0 ? "PASS" : "FAIL"));
    AMI_ERROR("=== unloadprobe done: %ld checks, %ld failures, %s", checks,
              failures, (LONG)(failures == 0 ? "PASS" : "FAIL"));

    ami_crash_remove_alert_hook();
    ami_crash_remove();

    return failures == 0 ? RETURN_OK : RETURN_ERROR;
}
