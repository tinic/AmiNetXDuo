/*
 * bsdsocket.library, library skeleton and per-opener child bases.
 *
 * The romtag lives here. Every OpenLibrary() returns a *distinct* base, cloned
 * from the master, carrying that task's descriptor table, errno pointer and tag
 * state (docs/RESEARCH.md S3.1). SocketBase is never shared.
 *
 * Link this file first so the "moveq #-1,d0 / rts" below sits at offset 0 of
 * the first code hunk, which makes the library file harmless if executed.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#ifdef AMINETXDUO_TCPDEVICE
#include "tcp_handler.h"
#endif
#include "interfaces.h"
#include "netmonitor.h"

#include "aminetxduo/config.h"
#include "aminetxduo/version.h"

#include "net68k.h"          /* n68k_cpu_select() */

#include <stddef.h>

#include <exec/execbase.h>   /* ThisTask, TaskReady, TaskWait: bsd_task_alive */
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

/*
 * A shared library has no startup code, so the SysBase the exec inlines want
 * is ours to define and fill in from the romtag init.
 */
struct ExecBase *SysBase;

/* Executing the library file returns a failure code rather than crashing. */
asm("    .text                       \n"
    "    .globl _bsdsocket_entry     \n"
    "_bsdsocket_entry:               \n"
    "    moveq  #-1,%d0              \n"
    "    rts                         \n");

static char bsd_lib_name[]  = BSD_LIB_NAME;
/*
 * What `Version` and `lib_IdString` say.
 *
 * The library had NO $VER: string at all, so `version full file
 * LIBS:bsdsocket.library` had nothing to read, reported by a user trying to
 * tell which release was installed. It carries the project's, like every
 * command (src/tools/tools.h, TOOL_VERSTAG).
 *
 * lib_Version and lib_Revision are a different thing and are not ours to
 * choose: 4 is what AmiTCP-era software opens us by, and the revision is the
 * vector-table revision a caller checks before reaching the RFC 3493 slots.
 * They stay where the ABI needs them, and lib_IdString says both so that
 * neither question needs the other answered first.
 *
 *   Version full file LIBS:bsdsocket.library
 *   bsdsocket.library 0.16.1 (1.8.2026) AmiNetXDuo f75f058
 *
 * The name is in there because Roadshow's library is also bsdsocket.library
 * and says "Roadshow 1.15 DEMO version" in the same place. Two stacks, one
 * filename; the product name is what tells them apart.
 */
#define BSD_STR2(x)         #x
#define BSD_STR(x)          BSD_STR2(x)
#define BSD_LIB_ABI_TEXT    BSD_STR(BSD_LIB_VERSION) "." BSD_STR(BSD_LIB_REVISION)

/* AMINETXDUO_VERSION_CPU carries its own leading space, and is empty on a host
   build.  The archive ships one library per processor and the file said
   nothing about which; this is the only place that answers it for a library
   already installed in LIBS:. */
static const char bsd_lib_ver[] __attribute__((used)) =
    "$VER: bsdsocket.library " AMINETXDUO_VERSION
    " (" AMINETXDUO_VERSION_DATE ") AmiNetXDuo " AMINETXDUO_VERSION_HASH
    AMINETXDUO_VERSION_CPU;

static char bsd_lib_id[] =
    "bsdsocket.library " AMINETXDUO_VERSION " (AmiNetXDuo, ABI "
    BSD_LIB_ABI_TEXT ")\r\n";

static struct AmiSocketBase *bsd_lib_init(
    register struct AmiSocketBase *base    __asm("d0"),
    register APTR                  seglist __asm("a0"),
    register struct ExecBase      *sysbase __asm("a6"));

static const APTR bsd_init_table[4] =
{
    (APTR)sizeof(struct AmiSocketBase),
    (APTR)BsdVectorTable,
    (APTR)NULL,                 /* no data table; we initialise by hand */
    (APTR)bsd_lib_init
};

const struct Resident bsd_romtag =
{
    RTC_MATCHWORD,
    (struct Resident *)&bsd_romtag,
    (APTR)(&bsd_romtag + 1),
    RTF_AUTOINIT,
    BSD_LIB_VERSION,
    NT_LIBRARY,
    0,                          /* priority */
    bsd_lib_name,
    bsd_lib_id,
    (APTR)bsd_init_table
};

/* -------------------------------------------------------------- utilities, */

ULONG bsd_strlen(const char *s)
{
    const char *p = s;

    if (s == NULL)
        return 0;
    while (*p != '\0')
        p++;

    return (ULONG)(p - s);
}

VOID bsd_strncpy(char *dst, const char *src, ULONG size)
{
    ULONG i;

    if (dst == NULL || size == 0)
        return;

    for (i = 0; i + 1 < size && src != NULL && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/*
 * bsd_poll_sets() clears a whole BsdFdSets through this, so a select-driven
 * reader pays it once per recv().  A1200 wire transfer, RATE=2000, one build
 * either side of this function:
 *
 *     byte loop    62 samples of 2545    2.4%
 *     longwords    16 samples of 2469    0.6%
 *
 * Throughput did not separate the two at two samples an arm.
 */
VOID bsd_bzero(APTR p, ULONG size)
{
    UBYTE *q = (UBYTE *)p;
    ULONG *l;

    /* Up to the first aligned longword by bytes.  A word or longword access to
       an odd address is an address error on a 68000, so this is required, not
       an optimisation. */
    while (size > 0u && (((ULONG)q) & 3u) != 0u)
    {
        *q++ = 0;
        size--;
    }

    /* Four at a time: the callers that matter clear a whole BsdFdSets, which
       is far larger than the loop overhead. */
    l = (ULONG *)((APTR)q);
    while (size >= 16u)
    {
        l[0] = 0UL;
        l[1] = 0UL;
        l[2] = 0UL;
        l[3] = 0UL;
        l    += 4;
        size -= 16u;
    }

    while (size >= 4u)
    {
        *l++  = 0UL;
        size -= 4u;
    }

    q = (UBYTE *)((APTR)l);
    while (size-- > 0u)
        *q++ = 0;
}

VOID bsd_bcopy(const APTR src, APTR dst, ULONG size)
{
    if (size > 0)
        CopyMem((APTR)src, dst, size);
}

/*
 * Fill in the segment tag for a base, see struct BsdProfSegTag. The sum is
 * what stops the magic appearing by accident in somebody else's data from
 * being taken for one of these, so it is computed rather than written out.
 */
static VOID bsd_prof_segtag(struct AmiSocketBase *base, APTR seglist)
{
    struct BsdProfSegTag *t = &base->sb_ProfSegTag;

    t->bst_Magic   = BSD_PROF_SEGTAG_MAGIC;
    t->bst_Size    = sizeof(*t);
    t->bst_LibBase = (ULONG)base;
    t->bst_SegList = (ULONG)seglist;
    t->bst_Sum     = 0UL - (t->bst_Magic + t->bst_Size +
                            t->bst_LibBase + t->bst_SegList);
}

/* Open-coded NewList(); amiga.lib is not available to a shared library. */
static VOID bsd_new_list(struct MinList *list)
{
    list->mlh_Head     = (struct MinNode *)&list->mlh_Tail;
    list->mlh_Tail     = NULL;
    list->mlh_TailPred = (struct MinNode *)&list->mlh_Head;
}

/* ------------------------------------------------------------------- init, */

/*
 * SBTC_SIG_ADDRESS_CHANGE_MASK: the openers that asked to hear about an
 * interface address arriving, changing or going away.
 *
 * Registered with src/common so the netstack can reach it, the dependency
 * runs the other way and there is no direct call.  It is deregistered in
 * bsd_lib_expunge(), because this function lives in the segment expunge hands
 * to UnLoadSeg() and a stale hook would be called into freed memory.
 *
 * Runs on the IP thread.  Signal() is Exec, does not block and is legal from
 * any Task.  Nothing here calls into NetX Duo: this is a notification NetX Duo
 * is already inside.
 *
 * ATTEMPT, NEVER OBTAIN.  This claimed sb_Lock was only ever held for the two
 * short list operations bsd_child_create() and bsd_child_free() take it for,
 * and that was wrong: bsd_lib_open() holds it across bsd_netstack_bringup(),
 * which blocks until the stack is up and DHCP has answered.  The first lease
 * then arrives on the IP thread, lands here, waits for a semaphore the opener
 * cannot release until this returns, and the two sit there.  What a user saw
 * was AddNetInterface never returning from its OpenLibrary(), so
 * S:User-Startup never finished and the machine stopped part way through its
 * Startup-Sequence (0.17.0 and 0.17.1; found by
 * install/test/run-workbench.sh).
 *
 * A signal that cannot be delivered is dropped rather than waited for.  That
 * costs nothing real: the only caller who can hold the lock long enough to
 * lose one is an opener still inside OpenLibrary(), which by definition has
 * not yet had the chance to ask for the mask, and every later change signals
 * normally.
 */
static struct AmiSocketBase *bsd_master_base;

/* Is this pointer still one of Exec's tasks?  The running one, plus the two
   scheduler lists, is the whole set.  Disable() and not Forbid(): an interrupt
   moves a task between TaskWait and TaskReady, so Forbid() does not make the
   walk safe. */
static BOOL bsd_task_on_list(struct List *list, struct Task *task)
{
    struct Node *node;

    for (node = list->lh_Head; node->ln_Succ != NULL; node = node->ln_Succ)
    {
        if ((struct Task *)node == task)
            return TRUE;
    }

    return FALSE;
}

/* Never inside Disable(): the caller may want to free memory on the answer,
   and FreeMem() may not be called with interrupts off. */
static BOOL bsd_task_alive(struct Task *task)
{
    BOOL alive;

    if (task == NULL)
        return FALSE;

    Disable();
    alive = (SysBase->ThisTask == task ||
             bsd_task_on_list(&SysBase->TaskReady, task) ||
             bsd_task_on_list(&SysBase->TaskWait, task));
    Enable();

    return alive;
}

static ULONG bsd_dead_task_signals;

/*
 * Signal() a task only if it is still there.
 *
 * sb_Task is the Task that opened the base, and a base can outlive it: a
 * program that exits without CloseLibrary() leaves its child base on the list
 * for good, with a pointer to a struct Task Exec has since freed and probably
 * handed to somebody else.  Signal() on that is not a read, it is a
 * read-modify-write of tc_SigRecvd plus, if the bits it sets are waited for, an
 * AddHead() onto the ready list, so it corrupts whatever is there now.
 *
 * The test and the Signal() are inside one Disable() so the task cannot leave
 * between them.  Nothing here dereferences the candidate pointer: the answer
 * comes from the lists, not from the block.
 *
 * What this cannot catch is a struct Task freed and reallocated as another
 * struct Task, which is on the list and is not the one meant.  That needs a
 * generation stamp Exec does not have.  A wrong task getting a spurious signal
 * is a different order of wrong from writing into a freed block.
 */
static VOID bsd_signal_if_alive(struct Task *task, ULONG mask)
{
    BOOL alive;

    Disable();

    alive = (SysBase->ThisTask == task ||
             bsd_task_on_list(&SysBase->TaskReady, task) ||
             bsd_task_on_list(&SysBase->TaskWait, task));

    if (alive)
        Signal(task, mask);

    Enable();

    /* Said once: a program that does this does it for the rest of the session,
       and the log is the only place the fact can appear at all. */
    if (!alive && bsd_dead_task_signals++ == 0UL)
    {
        AMI_WARN("bsdsocket: an address change was not delivered to task %lx: "
                 "it asked for SBTC_SIG_ADDRESS_CHANGE_MASK and then exited "
                 "without closing the library", (unsigned long)task);
    }
}

/*
 * Once a second, forget the tasks that are gone.
 *
 * bsd_event_post() (select.c) ends in Signal(base->sb_Task, signals) and runs
 * from NetX Duo's receive, transmit and out-of-band callbacks, which is to say
 * on every packet.  It has the same exposure bsd_address_changed() had, and
 * more of it: sb_EventSigMask is allocated before the base joins this list and
 * is never cleared, so its `signals != 0` test is always true and EVERY event
 * on ANY socket of an orphaned base performs that write.  The client does not
 * have to have asked for a single signal mask.
 *
 * The liveness scan bsd_signal_if_alive() makes cannot go there.  An address
 * change happens when a lease arrives; a packet happens hundreds of times a
 * second, and a walk of Exec's task lists with interrupts off is not something
 * to do on the receive path.  So the answer is computed here instead, on the
 * netstack's one-second heartbeat, and the two call sites keep the cheap NULL
 * test they already have.  One latch covers both.
 *
 * THIS MAY LATCH THE POINTER AND MUST DO NOTHING ELSE.  In particular it must
 * not destroy the base: bsd_child_destroy() is only correct on the owning
 * task.  bsd_nx_release() copes with a foreign caller, ami_signal_free() does
 * not -- it would hand the bit back out of the CALLING task's tc_SigAlloc,
 * which on this sweeper is the ThreadX tick task.  So the base stays exactly
 * where it is, leaked, holding its stack reference and its open count.  What
 * changes is that nothing writes through the dead pointer any more.
 *
 * A FALSE NEGATIVE HERE IS WORSE THAN THE DEFECT.  Clearing sb_Task on a task
 * that is alive kills that client's wakeups for the rest of the session, and
 * it would look like the network hanging.  bsd_task_alive() answers from
 * ThisTask plus the two scheduler lists, and that is the whole set for a live
 * task on this system: every blocking point in the ThreadX port is an Exec
 * Wait(), including a caller suspended inside NetX Duo
 * (port/threadx-amiga/src/tx_thread_system_return.c) and including the "park
 * forever" cases, and Wait() leaves the task on TaskWait.  The port never
 * touches TaskReady or TaskWait itself.  The two states that really are off
 * all three lists are a task between allocation and AddTask(), which nobody
 * can yet hold a pointer to, and a task after RemTask(), which is the thing
 * being looked for.
 *
 * What it does NOT close is the second between an exit and the next
 * heartbeat.  A packet arriving in that window still signals the freed block.
 * The window was the whole session; it is now bounded by the period above.
 *
 * Runs on the ThreadX tick task inside a Forbid(), so AttemptSemaphore and
 * never ObtainSemaphore, and nothing that can block.  AMI_WARN is RawPutChar.
 */
static ULONG bsd_latched_tasks;

static VOID bsd_task_sweep(VOID)
{
    struct AmiSocketBase *master = bsd_master_base;
    struct MinNode       *node;

    if (master == NULL)
        return;

    /*
     * ATTEMPT, NEVER OBTAIN, for the reason given at the top of this file and
     * one more: this runs at interrupt level and ObtainSemaphore() would
     * block the tick task.  A second missed costs nothing -- the next
     * heartbeat sweeps the same list.
     */
    if (!AttemptSemaphore(&master->sb_Lock))
        return;

    for (node = master->sb_Children.mlh_Head;
         node->mln_Succ != NULL;
         node = node->mln_Succ)
    {
        struct AmiSocketBase *child =
            (struct AmiSocketBase *)((UBYTE *)node -
                                     offsetof(struct AmiSocketBase, sb_Node));

        if (child->sb_Task == NULL || bsd_task_alive(child->sb_Task))
            continue;

        /* Once per base, not once per second: the latch below is what stops
           it repeating, and a program that does this does it for the rest of
           the session. */
        bsd_latched_tasks++;
        AMI_WARN("bsdsocket: task %lx exited without closing the library; "
                 "its base will no longer be signalled (%lu so far)",
                 (unsigned long)child->sb_Task,
                 (unsigned long)bsd_latched_tasks);

        child->sb_Task = NULL;
    }

    ReleaseSemaphore(&master->sb_Lock);
}

/*
 * The ARexx port's KILL, and anything else in the netstack that decides the
 * network is finished with.
 *
 * The same two steps NetShutdown takes, minus the grace period: signal every
 * opener, then give the stack hold back so the last CloseLibrary() takes the
 * stack with it. Without this, KILL took the interfaces down and left every
 * program holding a library whose network had gone -- the state NetShutdown
 * was in until 2026-08-11, reachable through the other door.
 *
 * bsd_master_base and not a caller's base: there is no caller here, so
 * nothing is skipped and every opener is told.
 */
static VOID bsd_shutdown_requested(VOID)
{
    struct AmiSocketBase *master = bsd_master_base;
    ULONG                 signalled = 0;

    if (master == NULL)
        return;

    (VOID)bsd_stack_notify(master, &signalled);

    if (signalled != 0)
        AMI_INFO("bsdsocket: shutdown, %lu program(s) told to stop",
                 (unsigned long)signalled);

    /* Declined when this is the last reference, which is the one case where
       dropping it would leave the stack running with nobody to close it. */
    (VOID)bsd_stack_unhold(master);
}

static VOID bsd_address_changed(VOID)
{
    struct AmiSocketBase *master = bsd_master_base;
    struct MinNode       *node;

    if (master == NULL)
        return;

    if (!AttemptSemaphore(&master->sb_Lock))
        return;

    for (node = master->sb_Children.mlh_Head;
         node->mln_Succ != NULL;
         node = node->mln_Succ)
    {
        struct AmiSocketBase *child =
            (struct AmiSocketBase *)((UBYTE *)node -
                                     offsetof(struct AmiSocketBase, sb_Node));

        if (child->sb_SigAddressChangeMask != 0UL && child->sb_Task != NULL)
            bsd_signal_if_alive(child->sb_Task, child->sb_SigAddressChangeMask);
    }
    ReleaseSemaphore(&master->sb_Lock);
}

static struct AmiSocketBase *bsd_lib_init(
    register struct AmiSocketBase *base    __asm("d0"),
    register APTR                  seglist __asm("a0"),
    register struct ExecBase      *sysbase __asm("a6"))
{
    SysBase = sysbase;

    /* Before anything can send or receive a packet: which form of the checksum
       and the copy this machine wants.  In an AMINETXDUO_CPU=any build these
       are four assemblies of each routine in this one binary and this is the
       call that picks; anywhere else it does nothing.  src/net68k/n68k_cpu.c. */
    n68k_cpu_select((ULONG)sysbase->AttnFlags);

    /* NOT ami_rt_cpu_select() here, and the reason is worth writing down: this
       library resolves __mulsi3 and its four siblings from newlib's libc.a,
       not from src/common/ami_udivdi3.c, so there is nothing of ours to
       switch.  It would also buy nothing -- no object on the data path
       references them (checked with nm across the whole link) -- which is why
       one binary costs this library 0.8% and costs tls.library and ssh much
       more.  Those two do link ours and do call it. */

    /* A shared library gets no C startup: dos.library and the random number
       generator are ours to set up (library_runtime.c). */
    if (!bsd_runtime_open())
        return NULL;

    base->sb_SegList = seglist;
    base->sb_SysBase = sysbase;
    base->sb_Master  = NULL;
    bsd_prof_segtag(base, seglist);

    base->sb_Lib.lib_Node.ln_Type = NT_LIBRARY;
    base->sb_Lib.lib_Node.ln_Name = bsd_lib_name;
    base->sb_Lib.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    base->sb_Lib.lib_Version      = BSD_LIB_VERSION;
    base->sb_Lib.lib_Revision     = BSD_LIB_REVISION;
    base->sb_Lib.lib_IdString     = bsd_lib_id;

    InitSemaphore(&base->sb_Lock);
    bsd_new_list(&base->sb_Children);
    base->sb_StackRefs = 0;
    base->sb_StackHeld = FALSE;
    bsd_handoff_init(base);

    bsd_master_base = base;
    ami_set_address_change_hook(bsd_address_changed);
    ami_set_second_hook(bsd_task_sweep);
    ami_set_shutdown_hook(bsd_shutdown_requested);

    return base;
}

/* -------------------------------------------------------- child base life, */

/* ------------------------------------------------- closed base poisoning, */

/*
 * A call through a base that was already closed lands here and does nothing.
 * Freeing the block instead means such a call jumps into whatever allocated it
 * next, which is a different guru every time and is what issue #2 looks like.
 * Answering it harmlessly is what the stacks that survive this already do in
 * effect, by not handing the memory back so promptly.
 */

/* How many closed bases keep their poisoned vectors, for as long as the task
   that could call through one is still running.  Each costs
   lib_NegSize + lib_PosSize, on the order of a kilobyte.  Past this the oldest
   is freed for real and a stale call through it is a guru again. */
#define BSD_DEAD_RING           8

static APTR         bsd_dead_block[BSD_DEAD_RING];
static ULONG        bsd_dead_bytes[BSD_DEAD_RING];
static struct Task *bsd_dead_owner[BSD_DEAD_RING];
static UINT         bsd_dead_next;

static ULONG bsd_dead_calls;

/*
 * Returns 0 and nothing else.  Zero rather than -1 because the vectors have
 * assorted shapes and this one handler answers all of them: a caller that
 * expects a pointer gets NULL, which it has to check anyway, where -1 would be
 * an address it might dereference.  A caller expecting a count or a status
 * gets 0, which is the harmless answer for a call that did nothing.
 *
 * The counter is not diagnostics for its own sake: it is the only evidence
 * left that a program called through a base it had closed, now that doing so
 * no longer announces itself.
 */
static LONG bsd_dead_base_call(VOID)
{
    /*
     * Once, not every time: a program that does this usually does it in a
     * loop, and the point is to say that it happened, not to fill the log.
     */
    if (bsd_dead_calls == 0UL)
    {
        AMI_WARN("bsdsocket: a call arrived through a base that was already "
                 "closed; answering it with 0. The caller closed the library "
                 "and kept its pointer.");
    }

    bsd_dead_calls++;
    return 0;
}

ULONG bsd_dead_base_calls(VOID)
{
    return bsd_dead_calls;
}

/*
 * Overwrite the whole negative half with `JMP bsd_dead_base_call`.  Entry i
 * sits at -(6 * (i + 1)), so the half is exactly lib_NegSize / 6 six-byte
 * slots and filling it covers every vector without knowing how many are real.
 *
 * CacheClearU() for the same reason bsd_child_create() needs it: these are
 * instructions written through the data cache, and on a 68030 and up the
 * instruction cache may still hold the JMPs that used to be here.
 */
static VOID bsd_poison_vectors(UBYTE *block, ULONG neg)
{
    ULONG  target = (ULONG)bsd_dead_base_call;
    ULONG  slots  = neg / 6UL;
    ULONG  i;

    for (i = 0; i < slots; i++)
    {
        UBYTE *v = block + (i * 6UL);

        v[0] = 0x4EU;                    /* JMP xxx.L */
        v[1] = 0xF9U;
        v[2] = (UBYTE)(target >> 24);
        v[3] = (UBYTE)(target >> 16);
        v[4] = (UBYTE)(target >> 8);
        v[5] = (UBYTE)(target);
    }

    CacheClearU();
}

/*
 * Give back the entries whose owner has gone.
 *
 * A base belongs to one task, so the only program that can call through this
 * one after closing it is that task.  Once the task is off Exec's lists there
 * is nobody left to protect and the block is only a hole in the machine's
 * memory: the ring's whole reason to hold it has expired.
 *
 * This is what makes a command that opens the library, closes it and exits cost
 * nothing at all.  Without it the ring keeps the first BSD_DEAD_RING of them,
 * which reads as a leak of one base per run for eight runs and then stops, and
 * for a command run a handful of times at boot the "and then stops" never
 * arrives.
 *
 * FreeMem() outside Disable(), which is why bsd_task_alive() ends its own.
 */
static VOID bsd_dead_ring_reap(VOID)
{
    UINT i;

    for (i = 0; i < (UINT)BSD_DEAD_RING; i++)
    {
        if (bsd_dead_block[i] == NULL)
            continue;

        if (bsd_task_alive(bsd_dead_owner[i]))
            continue;

        AMI_CENSUS_DROP(bsd_dead_block[i]);
        FreeMem(bsd_dead_block[i], bsd_dead_bytes[i]);
        bsd_dead_block[i] = NULL;
        bsd_dead_bytes[i] = 0;
        bsd_dead_owner[i] = NULL;
    }
}

/*
 * One retained base per owner, the most recently closed.
 *
 * A base belongs to one task, so this task is the only one that can call
 * through either of them, and what it is about to do is close a second base
 * having already closed a first.  The block held for the first is either still
 * its own, in which case the newer close is the one worth protecting, or the
 * pointer has been handed to a different task since, in which case the first
 * owner is gone and there was nobody left to protect at all.  Both readings
 * give the same answer.
 *
 * This is what a command run repeatedly costs.  Exec hands a freed process
 * block straight back to the next process of the same size, so a command run
 * ten times at boot is usually ten bases with the same sb_Task, and the ring
 * would otherwise hold the first BSD_DEAD_RING of them.  Being alive is not
 * the test that catches it: the pointer is live again, it is just somebody
 * else's.
 *
 * The narrowing is real and worth stating: a program that closes two bases
 * keeps the poisoned vectors of the newer only.  The ring it replaces was a
 * machine-wide window of eight closes, so an unrelated burst of opens could
 * already evict a program's own entry; per owner, that cannot happen.
 */
static VOID bsd_dead_ring_drop_owner(struct Task *owner)
{
    UINT i;

    if (owner == NULL)
        return;

    for (i = 0; i < (UINT)BSD_DEAD_RING; i++)
    {
        if (bsd_dead_block[i] == NULL || bsd_dead_owner[i] != owner)
            continue;

        AMI_CENSUS_DROP(bsd_dead_block[i]);
        FreeMem(bsd_dead_block[i], bsd_dead_bytes[i]);
        bsd_dead_block[i] = NULL;
        bsd_dead_bytes[i] = 0;
        bsd_dead_owner[i] = NULL;
    }
}

/* Poison this block and retain it, freeing whatever it displaces. */
static VOID bsd_retain_dead(UBYTE *block, ULONG neg, ULONG bytes,
                            struct Task *owner)
{
    UINT slot;

    bsd_poison_vectors(block, neg);

    /* Before choosing a slot, not after: both of these usually empty one, and a
       ring with a free slot does not have to displace a base whose owner is
       still running and could still call through it. */
    bsd_dead_ring_drop_owner(owner);
    bsd_dead_ring_reap();

    slot = bsd_dead_next;

    {
        UINT i;

        for (i = 0; i < (UINT)BSD_DEAD_RING; i++)
        {
            UINT s = (UINT)((bsd_dead_next + i) % (UINT)BSD_DEAD_RING);

            if (bsd_dead_block[s] == NULL)
            {
                slot = s;
                break;
            }
        }
    }

    if (bsd_dead_block[slot] != NULL)
    {
        AMI_CENSUS_DROP(bsd_dead_block[slot]);
        FreeMem(bsd_dead_block[slot], bsd_dead_bytes[slot]);
    }

    bsd_dead_block[slot] = (APTR)block;
    bsd_dead_bytes[slot] = bytes;
    bsd_dead_owner[slot] = owner;
    bsd_dead_next        = (UINT)((slot + 1U) % (UINT)BSD_DEAD_RING);
}

/* Called from the expunge: the segment is going away, so nothing may be left
   pointing into it, poisoned or not. */
static VOID bsd_dead_ring_flush(VOID)
{
    UINT i;

    for (i = 0; i < (UINT)BSD_DEAD_RING; i++)
    {
        if (bsd_dead_block[i] != NULL)
        {
            AMI_CENSUS_DROP(bsd_dead_block[i]);
            FreeMem(bsd_dead_block[i], bsd_dead_bytes[i]);
            bsd_dead_block[i] = NULL;
            bsd_dead_bytes[i] = 0;
            bsd_dead_owner[i] = NULL;
        }
    }
}

static struct AmiSocketBase *bsd_child_create(struct AmiSocketBase *master)
{
    struct AmiSocketBase *child;
    ULONG                 neg = master->sb_Lib.lib_NegSize;
    ULONG                 pos = master->sb_Lib.lib_PosSize;
    UBYTE                *mem;
    BYTE                  sig;

    mem = (UBYTE *)AllocMem(neg + pos, MEMF_PUBLIC);
    if (mem == NULL)
        return NULL;

    /* Not an ami_alloc(), so the census does not see it on its own -- and this
       is the block of the three leaks of 2026-08-09 that the census exists for:
       one base per opener, freed by neither the opener nor this function but by
       the dead ring, later, on somebody else's close. */
    AMI_CENSUS_ADD(mem, neg + pos);

    /* Clone the jump table as well as the data: the child *is* a library. */
    CopyMem((UBYTE *)master - neg, mem, neg + pos);

    /*
     * The negative half just copied is code (the LVO jump table). On a 68030
     * and up with the instruction cache on, the CPU may still hold stale lines
     * for this block, it was very likely somebody else's freed code a moment
     * ago, so a call through the child's vectors would execute those instead
     * of the JMPs just written. CacheClearU() flushes both caches. Missing this
     * is invisible on a 68000/68020 and on emulators that do not model the
     * I-cache.
     */
    CacheClearU();

    child = (struct AmiSocketBase *)(mem + neg);

    child->sb_Lib.lib_OpenCnt = 1;
    child->sb_Lib.lib_Flags   = LIBF_CHANGED;
    child->sb_Master          = master;
    child->sb_Task            = FindTask(NULL);

    /* The copy names the master as the base it sits in, which is the check
       that stops a moved record being believed. Same seglist, new base. */
    bsd_prof_segtag(child, master->sb_SegList);

    /*
     * The clone carries copies of the master-only fields, and a copied
     * SignalSemaphore holds pointers into the master's wait queue. Children
     * never touch either, but on a system with no memory protection leaving
     * live-looking pointers around is a hazard, so clear them.
     */
    bsd_bzero(&child->sb_Lock, sizeof(child->sb_Lock));
    bsd_bzero(&child->sb_Children, sizeof(child->sb_Children));
    bsd_bzero(&child->sb_Handoffs, sizeof(child->sb_Handoffs));
    child->sb_StackRefs      = 0;
    child->sb_StackHeld      = FALSE;
    child->sb_NextHandoffId  = 0;

    /* Clear the inherited ThreadX bracket state rather than depend on the
       master never having been inside one. */
    bsd_bzero(&child->sb_NxCaller, sizeof(child->sb_NxCaller));
    child->sb_NxNest = 0;

    child->sb_Table     = NULL;
    child->sb_TableSize = 0;

    child->sb_Errno     = 0;
    child->sb_ErrnoPtr  = NULL;
    child->sb_ErrnoSize = 0;
    child->sb_HErrno    = 0;
    child->sb_HErrnoPtr = NULL;

    child->sb_BreakMask    = SIGBREAKF_CTRL_C;
    child->sb_SigIOMask    = 0;
    child->sb_SigUrgMask   = 0;
    child->sb_SigEventMask = 0;

    child->sb_SigAddressChangeMask = 0;   /* "Default for this mask is 0" */
    child->sb_CanShareBases        = FALSE;

    /* The autodoc's defaults: LOG_USER and 0xFF, not zero. LOG_USER is 1<<3
, the NDK's <sys/syslog.h> ships the priorities and not the facility
       codes, and a mask of 0 would suppress every message rather than pass
       them all. */
    child->sb_LogTag      = NULL;
    child->sb_LogStat     = 0;
    child->sb_LogFacility = BSD_LOG_USER;
    child->sb_LogMask     = 0xFF;
    child->sb_FDCallback  = NULL;
    child->sb_ErrorHook   = NULL;

    child->sb_TimerOpen    = FALSE;
    child->sb_TimerSignal  = -1;
    child->sb_TimerSigMask = 0;

    /* The netdb iterators (netdb.c) start at the top of each table. */
    child->sb_ServCursor  = 0;
    child->sb_ProtoCursor = 0;
    child->sb_NetCursor   = 0;

    /*
     * The wakeup signal belongs to the opening task, which is the task this
     * base belongs to. Everything that can make a socket ready, NetX Duo's
     * receive/connect/disconnect callbacks, signals it, so WaitSelect() can
     * Wait() on it alongside the caller's own bits.
     */
    sig = ami_signal_alloc();
    if (sig < 0)
    {
        AMI_CENSUS_DROP(mem);
        FreeMem(mem, neg + pos);
        return NULL;
    }
    child->sb_EventSignal  = sig;
    child->sb_EventSigMask = 1UL << sig;

    ObtainSemaphore(&master->sb_Lock);
    AddTail((struct List *)&master->sb_Children, (struct Node *)&child->sb_Node);
    ReleaseSemaphore(&master->sb_Lock);

    /* One child base per OpenLibrary(), so this is the number of programs
       holding the network open, the denominator for the socket count in the
       health report. */
    ami_mem_open_delta(1);

    return child;
}

static VOID bsd_child_destroy(struct AmiSocketBase *child)
{
    struct AmiSocketBase *master = child->sb_Master;
    ULONG                 neg    = child->sb_Lib.lib_NegSize;
    ULONG                 pos    = child->sb_Lib.lib_PosSize;

    bsd_close_all(child);

    /* The capture channels this base opened go with it, as the bpf_open()
       autodoc says. Frees and Forbid()/Permit() only, nothing that could
       wait, which is what the close path cannot afford (544398f). */
    bsd_bpf_close_all(child);

    /* The timer signal is the opener's, not the library's: WaitSelect() takes
       it out of the caller's 32 bits and there is no way to recover one, so an
       open/close cycle that forgot it cost the caller a bit for good. */
    if (child->sb_TimerOpen)
    {
        CloseDevice((struct IORequest *)&child->sb_TimerReq);
        child->sb_TimerOpen = FALSE;
        ami_signal_free(child->sb_TimerSignal);
        child->sb_TimerSignal  = -1;
        child->sb_TimerSigMask = 0;
    }

    if (child->sb_Table != NULL)
    {
        ami_free(child->sb_Table);
        child->sb_Table     = NULL;
        child->sb_TableSize = 0;
    }

    ami_signal_free(child->sb_EventSignal);

    /* Last, because everything above may take the bracket: drop the base's
       cached ThreadX registration. This is the one place that runs on the
       base's own task with every socket already closed (netx_call.c). */
    bsd_nx_release(child);

    ObtainSemaphore(&master->sb_Lock);
    Remove((struct Node *)&child->sb_Node);
    ReleaseSemaphore(&master->sb_Lock);

    ami_mem_open_delta(-1);

    bsd_retain_dead((UBYTE *)child - neg, neg, neg + pos, child->sb_Task);
}

/* --------------------------------------------------- stack bring-up proc, */
/*
 * netstack_startup() parses DEVS: and runs all of NetX Duo's init on the
 * calling task's stack, it adopts that task (netstack.c, ami_ns_bring_up()).
 * Opened from a command with the ~4 KB startup-sequence Shell stack, which is
 * how AddNetInterface comes up at boot, that overflows and smashes the return
 * path; with no memory protection it surfaces as an illegal instruction or
 * line-F at a wild address seconds later.
 *
 * So the first open runs the bring-up on a Process of our own with a stack we
 * choose, and waits for its result; later opens just take a reference. The
 * handshake mirrors bsd_tcp_handler_start(): a boot record on this stack, a
 * file-scope pointer to it, and a private signal (not SIGF_SINGLE, see
 * bsd_netstack_bringup()), serialised by the master sb_Lock the caller already
 * holds across the whole bring-up.
 */
#define BSD_STARTUP_STACK   (64UL * 1024UL)

typedef struct
{
    struct Task *nb_Parent;
    ULONG        nb_SigMask;
    LONG         nb_Result;
} BsdNetBoot;

static BsdNetBoot *bsd_net_boot;

static VOID bsd_netstack_boot_main(VOID)
{
    BsdNetBoot *b = bsd_net_boot;

    /* netstack_startup() blocks until NX_IP has initialised, so signal only
       after it returns, the parent Wait()s the whole time and `boot` lives
       on its stack until then. */
    b->nb_Result = netstack_startup();

    /*
     * A failed startup can still leave a stack standing: netstack_startup()
     * sets ns_Refs to 1 when ami_ns_bring_up() fails with the NX_IP already
     * made, on the grounds that a live stack has an owner. Here it has none,
     * bsd_lib_open() is about to return NULL, so no base will ever be closed
     * to give that reference back.
     *
     * Left alone it is permanent. sb_StackRefs was never incremented, so the
     * next successful open takes ns_Refs to 2 and the last close only ever
     * returns it to 1: the stack can never be freed and never be shut down
     * again.
     *
     * The cost is not only the packet pool. A live stack has ThreadX threads
     * running out of this segment, so the library can never be expunged
     * either, measured at about 400 KB kept on a 1 MB machine, which is the
     * segment plus the pool at its AMI_POOL_MIN_PACKETS floor plus the NX_IP
     * and the thread stacks. On the supported 1 MB floor that is most of the
     * machine, held by a command that printed an error and exited.
     *
     * Given back here rather than in bsd_lib_open() because nx_ip_delete()
     * waits for the IP thread, and this Process has 64 KB where the opener may
     * be a Shell command with 4 KB. netstack_shutdown() returns at once when
     * there is no stack, so the case where bring-up failed before allocating
     * anything costs nothing.
     */
    if (b->nb_Result != AMI_NET_OK)
        netstack_shutdown();

    Signal(b->nb_Parent, b->nb_SigMask);
}

static LONG bsd_netstack_bringup(VOID)
{
    BsdNetBoot      boot;
    struct TagItem  tags[5];
    struct Process *proc;
    BYTE            sig;

    /* A private signal, not SIGF_SINGLE: the child runs netstack_startup(),
       which starts the ThreadX kernel, and the port uses SIGF_SINGLE as its
       thread run-signal. Sharing it would let a stray ThreadX dispatch wake
       this Wait() early, we would return and pop this stack frame (with
       `boot` on it), and the child would then write nb_Result into the dead
       frame, smashing our return address (the Shell Process wild jump to a
       near-null PC). */
    sig = (BYTE)AllocSignal(-1);
    if (sig < 0)
        return netstack_startup();      /* no signal: caller-stack fallback */

    boot.nb_Parent  = FindTask(NULL);
    boot.nb_SigMask = 1UL << sig;
    boot.nb_Result  = AMI_NET_ERR_KERNEL;
    bsd_net_boot    = &boot;

    tags[0].ti_Tag  = NP_Entry;     tags[0].ti_Data = (ULONG)bsd_netstack_boot_main;
    tags[1].ti_Tag  = NP_Name;      tags[1].ti_Data = (ULONG)"bsdsocket stack";
    tags[2].ti_Tag  = NP_StackSize; tags[2].ti_Data = BSD_STARTUP_STACK;
    tags[3].ti_Tag  = NP_Cli;       tags[3].ti_Data = (ULONG)FALSE;
    tags[4].ti_Tag  = TAG_DONE;     tags[4].ti_Data = 0;

    proc = CreateNewProc(tags);
    if (proc == NULL)
    {
        /* Cannot spawn: fall back to the caller's stack. An opener that came
           with enough stack still works; one that did not is no worse off. */
        bsd_net_boot = NULL;
        FreeSignal(sig);
        return netstack_startup();
    }

    Wait(boot.nb_SigMask);
    bsd_net_boot = NULL;
    FreeSignal(sig);

    return boot.nb_Result;
}

/* ------------------------------------------------------------ exec vectors, */

struct AmiSocketBase *bsd_lib_open(
    register ULONG                 version    __asm("d0"),
    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    struct AmiSocketBase *master = SocketBase;
    struct AmiSocketBase *child;

    (VOID)version;

    /*
     * Opening a child base again would hand two tasks the same descriptor
     * table. Redirect to the master so the caller still gets a private base.
     */
    if (master->sb_Master != NULL)
        master = master->sb_Master;

    master->sb_Lib.lib_Flags &= ~LIBF_DELEXP;

    /* Hold a reference across the (semaphore-blocking) startup below. */
    master->sb_Lib.lib_OpenCnt++;

    ObtainSemaphore(&master->sb_Lock);

    /*
     * The DEVS:Internet netdb backs get{serv,proto,net}by*() (netdb.c).
     * ami_netdb_load() is idempotent but not re-entrant, so it happens here,
     * inside the master semaphore and on a Process (it reads files), never
     * from a lookup: the netstack's own threads resolve names, and they are
     * not Processes, so the lazy load in netdb_table() would open a file from
     * a context that cannot. This is the ONLY thing that loads it for the
     * library -- ami_config_load() used to as a side effect and no longer
     * does -- and bsd_lib_expunge() below is the matching free.
     */
    (VOID)ami_netdb_load();

    if (master->sb_StackRefs == 0)
    {
        /* Bring the stack up on our own big-stack Process, not the opener's
           stack, see bsd_netstack_bringup() above. */
        if (bsd_netstack_bringup() != AMI_NET_OK)
        {
            ReleaseSemaphore(&master->sb_Lock);
            master->sb_Lib.lib_OpenCnt--;
            AMI_ERROR("bsdsocket: netstack_startup failed");
            return NULL;
        }
    }
    master->sb_StackRefs++;

    ReleaseSemaphore(&master->sb_Lock);

    child = bsd_child_create(master);
    if (child == NULL)
    {
        ObtainSemaphore(&master->sb_Lock);
        if (--master->sb_StackRefs == 0)
            netstack_shutdown();
        ReleaseSemaphore(&master->sb_Lock);

        master->sb_Lib.lib_OpenCnt--;
        return NULL;
    }

    /*
     * TCP: exists from the first OpenLibrary() onwards. That is Roadshow's
     * rule ("when bsdsocket.library is initialized, it attempts to add a file
     * system device by the name of TCP:") and the only thing that works: DOS
     * has to find the device node before it can route an Open("TCP:..."), and
     * `Type` does not open bsdsocket.library.
     *
     * Here rather than in bsd_lib_init() because this runs in the opener's own
     * Process and the handler is created with CreateNewProc(). Guarded inside
     * so it runs once; the handler's own OpenLibrary() lands here and finds it
     * done.
     */
#ifdef AMINETXDUO_TCPDEVICE
    bsd_tcp_handler_start(master);
#else
    (VOID)master;
#endif

    return child;
}

APTR bsd_lib_close(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    struct AmiSocketBase *base   = SocketBase;
    struct AmiSocketBase *master = base;
    BOOL                  stack_went_down = FALSE;

    if (base->sb_Master != NULL)
    {
        master = base->sb_Master;

        /*
         * If this is the last opener, nothing can ObtainSocket() again, so
         * release anything still parked in the hand-off registry rather than
         * leak it. Before the child base goes: bsd_handoff_flush() needs a
         * live base for the ThreadX bracket the teardown runs in.
         */
        ObtainSemaphore(&master->sb_Lock);
        if (master->sb_StackRefs <= 1)
            bsd_handoff_flush(base);
        ReleaseSemaphore(&master->sb_Lock);

        bsd_child_destroy(base);

        /*
         * The teardown runs with the lock held, and has to: the decrement that
         * reaches zero and the shutdown it triggers are one step. Drop the lock
         * between them and an opener arriving in that window sees sb_StackRefs
         * at zero, calls netstack_startup(), and gets a reference to a stack
         * that is being dismantled. Nothing on the teardown path takes this
         * semaphore, so holding it only makes concurrent opens wait.
         */
        ObtainSemaphore(&master->sb_Lock);
        if (master->sb_StackRefs > 0 && --master->sb_StackRefs == 0)
        {
            netstack_shutdown();
            stack_went_down = TRUE;
        }
        ReleaseSemaphore(&master->sb_Lock);

        /*
         * Outside the lock: the report writes to the serial port, which is not
         * something to do while holding a semaphore every opener waits on.
         *
         * The stack is down but the library is still loaded, so this is the
         * count for one run of one program, which is the granularity a leak
         * shows up at.  The netdb tables are deliberately still held here and
         * are freed by the expunge; that is why there are two scopes.
         */
        if (stack_went_down)
            AMI_CENSUS_REPORT("bsd-stack-down");
    }

    if (master->sb_Lib.lib_OpenCnt > 0)
        master->sb_Lib.lib_OpenCnt--;

    if (master->sb_Lib.lib_OpenCnt == 0 &&
        (master->sb_Lib.lib_Flags & LIBF_DELEXP) != 0)
        return bsd_lib_expunge(master);

    return NULL;
}

/*
 * NETCTRL_STACK_HOLD: the library takes its own reference to the stack it is
 * running, so the command that started the network can close its base like any
 * other opener and the network still stands afterwards.
 *
 * That used to be a leaked OpenLibrary(): AddNetInterface opened, never closed,
 * and the reference it did not give back was what kept the interface up.  One
 * per invocation, and each one left a child base on the list for good, holding
 * a struct Task that had exited, an Exec signal bit belonging to that task, and
 * a ThreadX registration naming that task's stack, which is the range
 * tx_thread_create() then refuses to overlap (src/sana2/sana2_rx.c).
 *
 * The reference taken here is exactly the pair the leak used to hold, a
 * netstack reference so the last close does not call netstack_shutdown(), and
 * an open count so an expunge is declined while ThreadX threads are running out
 * of this segment, and nothing else.  It costs no memory and it is taken ONCE:
 * the flag is the whole point, since the second AddNetInterface of a session
 * must not cost what the first did.
 *
 * There is no release.  That is not an oversight: it is the same permanence the
 * leak had, and NetShutdown's header says so in as many words.  Giving it back
 * would need an operation that decides the network is finished with, which is a
 * different feature from this one.
 */
LONG bsd_stack_hold(struct AmiSocketBase *base)
{
    struct AmiSocketBase *master = base;
    LONG                  rc     = 0;

    if (master == NULL)
        return -1;

    if (master->sb_Master != NULL)
        master = master->sb_Master;

    ObtainSemaphore(&master->sb_Lock);

    if (!master->sb_StackHeld)
    {
        /* Nothing to hold. The caller has a base open, so this cannot happen
           from a live opener; it is here so a wrong caller gets an error
           rather than a reference to a stack that is not running. */
        if (master->sb_StackRefs == 0)
        {
            rc = -1;
        }
        else
        {
            /* Forbid() as well as the semaphore: every other lib_OpenCnt
               update happens inside bsd_lib_open()/bsd_lib_close(), which Exec
               calls with the task switcher off, and those take no semaphore.
               This is the one that arrives through a vector. */
            Forbid();
            master->sb_StackRefs++;
            master->sb_Lib.lib_OpenCnt++;
            master->sb_StackHeld = TRUE;
            Permit();
        }
    }

    ReleaseSemaphore(&master->sb_Lock);

    return rc;
}

/*
 * NETCTRL_STACK_RELEASE: give the hold back.
 *
 * The paragraph above says there is no release and that giving it back needs
 * an operation that decides the network is finished with. NetShutdown is that
 * operation, so this is that release.
 *
 * It refuses when the caller is the only reference left, which is the one case
 * that would run netstack_shutdown() from inside this call with the caller's
 * base still live and its next vector call landing in a stack being taken
 * apart. Two references means the hold and the caller, and dropping the hold
 * then leaves the teardown where it belongs, in the caller's CloseLibrary().
 */
LONG bsd_stack_unhold(struct AmiSocketBase *base)
{
    struct AmiSocketBase *master = base;
    LONG                  rc     = 0;

    if (master == NULL)
        return -1;

    if (master->sb_Master != NULL)
        master = master->sb_Master;

    ObtainSemaphore(&master->sb_Lock);

    if (master->sb_StackHeld)
    {
        if (master->sb_StackRefs < 2)
        {
            rc = -1;
        }
        else
        {
            Forbid();                      /* bsd_stack_hold()'s reason */
            master->sb_StackRefs--;
            if (master->sb_Lib.lib_OpenCnt > 0)
                master->sb_Lib.lib_OpenCnt--;
            master->sb_StackHeld = FALSE;
            Permit();
        }
    }

    ReleaseSemaphore(&master->sb_Lock);

    return rc;
}

/*
 * NETCTRL_STACK_NOTIFY: tell every program using the network that it is going
 * away, by sending it the signal it already listens for.
 *
 * SIGBREAKF_CTRL_C always, which is what AmiTCP's api_sendbreaktotasks() sent
 * to every task on its socketBaseList and what AmiTCP_NG still sends. It is
 * the signal an Amiga program already means "stop" by, so a program written
 * for either of those stacks needs no change to work with this one.
 *
 * The base's own sb_BreakMask goes with it when it is something else.
 * SBTC_BREAKMASK is "the signal to send to the process which owns the socket
 * in order to abort a blocking operation", so a program that moved it off
 * Ctrl-C is asking to be woken on that bit instead, and a notification it
 * cannot be woken by is not one. Neither reference implementation reads the
 * mask here; sending both covers the program that set one without missing the
 * program that expects the standard bit.
 *
 * Skips the caller: NetShutdown breaking itself in the middle of its own grace
 * period is not a notification. Skips a base whose task has exited, through
 * bsd_signal_if_alive() and for the reason written there.
 */
LONG bsd_stack_notify(struct AmiSocketBase *base, ULONG *signalled)
{
    struct AmiSocketBase *master = base;
    struct MinNode       *node;
    ULONG                 n = 0;

    if (master == NULL)
        return -1;

    if (master->sb_Master != NULL)
        master = master->sb_Master;

    ObtainSemaphore(&master->sb_Lock);

    for (node = master->sb_Children.mlh_Head;
         node->mln_Succ != NULL;
         node = node->mln_Succ)
    {
        struct AmiSocketBase *child =
            (struct AmiSocketBase *)((UBYTE *)node -
                                     offsetof(struct AmiSocketBase, sb_Node));

        if (child == base || child->sb_Task == NULL)
            continue;

        bsd_signal_if_alive(child->sb_Task,
                            SIGBREAKF_CTRL_C | child->sb_BreakMask);
        n++;
    }

    ReleaseSemaphore(&master->sb_Lock);

    if (signalled != NULL)
        *signalled = n;

    return 0;
}

/* How many sockets a base has open, for the report: the descriptor table with
   the empty slots taken out. */
static UWORD bsd_base_sockets(const struct AmiSocketBase *child)
{
    ULONG i;
    UWORD n = 0;

    if (child->sb_Table == NULL)
        return 0;

    for (i = 0; i < (ULONG)child->sb_TableSize; i++)
    {
        if (child->sb_Table[i] != NULL)
            n++;
    }

    return n;
}

/*
 * What to call an opener in a report.
 *
 * tc_Node.ln_Name is "Background CLI" for anything a script started with Run
 * or System(), which is how a service is normally started and is no use to a
 * reader being told which program will not let go. A Process started from a
 * Shell carries the command's own name in cli_CommandName, which is what the
 * Status command prints, so that comes first and the task name is the
 * fallback for a bare Task or a Process with no CLI.
 *
 * Only for a task that is still there: pr_CLI on a freed Task is a wild read.
 */
static VOID bsd_opener_name(const struct Task *task, char *out, LONG size)
{
    const char *name = task->tc_Node.ln_Name;
    LONG        i;

    if (task->tc_Node.ln_Type == NT_PROCESS)
    {
        const struct Process *pr = (const struct Process *)task;

        if (pr->pr_CLI != 0)
        {
            const struct CommandLineInterface *cli = BADDR(pr->pr_CLI);

            if (cli != NULL && cli->cli_CommandName != 0)
            {
                const UBYTE *bstr = BADDR(cli->cli_CommandName);

                /* A BSTR: length byte, then the characters. Empty means the
                   Shell has not got a command in hand, so keep the task. */
                if (bstr != NULL && bstr[0] != 0)
                {
                    LONG len   = (LONG)bstr[0];
                    LONG start = 0;
                    LONG j;

                    /* The command as typed, so "SYS:httpd" or
                       "Work:net/httpd". The drawer is not what identifies the
                       program to somebody deciding which one to close. */
                    for (j = 0; j < len; j++)
                    {
                        if (bstr[j + 1] == '/' || bstr[j + 1] == ':')
                            start = j + 1;
                    }

                    len -= start;
                    if (len > size - 1)
                        len = size - 1;

                    for (i = 0; i < len; i++)
                        out[i] = (char)bstr[start + i + 1];
                    out[len] = '\0';
                    return;
                }
            }
        }
    }

    for (i = 0; name != NULL && name[i] != '\0' && i < size - 1; i++)
        out[i] = name[i];
    out[i] = '\0';
}

/*
 * NETSTATUS_OPENERS: the same list, as a table a command can print.
 *
 * Copied under the semaphore because a task that exits takes its name with it.
 */
LONG bsd_openers_list(struct AmiSocketBase *base, NetStatusOpener *out,
                      LONG max, LONG *available)
{
    struct AmiSocketBase *master = base;
    struct MinNode       *node;
    LONG                  n     = 0;
    LONG                  total = 0;

    if (master == NULL)
        return -1;

    if (master->sb_Master != NULL)
        master = master->sb_Master;

    ObtainSemaphore(&master->sb_Lock);

    for (node = master->sb_Children.mlh_Head;
         node->mln_Succ != NULL;
         node = node->mln_Succ)
    {
        struct AmiSocketBase *child =
            (struct AmiSocketBase *)((UBYTE *)node -
                                     offsetof(struct AmiSocketBase, sb_Node));

        total++;

        if (n < max)
        {
            NetStatusOpener *o = &out[n++];

            bsd_bzero(o, sizeof(*o));

            o->nso_Task      = (ULONG)child->sb_Task;
            o->nso_BreakMask = child->sb_BreakMask;
            o->nso_Sockets   = bsd_base_sockets(child);

            if (child == base)
                o->nso_Flags |= NETSTATUS_OPENER_SELF;

            /* bsd_task_sweep() clears sb_Task on a base whose task left, and
               a task that has gone since the last sweep is the same fact. */
            if (child->sb_Task == NULL || !bsd_task_alive(child->sb_Task))
                o->nso_Flags |= NETSTATUS_OPENER_GONE;
            else
                bsd_opener_name(child->sb_Task, o->nso_Name,
                                (LONG)sizeof(o->nso_Name));
        }
    }

    ReleaseSemaphore(&master->sb_Lock);

    if (available != NULL)
        *available = total;

    return n;
}

/* The library's own count, for NETSTATUS_SYSTEM. */
ULONG bsd_open_count(struct AmiSocketBase *base)
{
    struct AmiSocketBase *master = base;

    if (master == NULL)
        return 0;

    if (master->sb_Master != NULL)
        master = master->sb_Master;

    return (ULONG)master->sb_Lib.lib_OpenCnt;
}

APTR bsd_lib_expunge(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    struct AmiSocketBase *base = SocketBase;
    APTR                  seglist;
    ULONG                 neg, pos;

    if (base->sb_Master != NULL)
        base = base->sb_Master;

    if (base->sb_Lib.lib_OpenCnt > 0)
    {
        base->sb_Lib.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }

    /*
     * The TCP: handler process runs code out of the segment this is about to
     * hand back for UnLoadSeg(), and it holds no OpenCnt reference, it takes
     * one only while a file handle is open, so OpenCnt at zero means it is
     * idle, not gone. There is no way to prove it is not executing, so decline
     * the expunge. ACTION_DIE takes TCP: down; after that this succeeds.
     */
#ifdef AMINETXDUO_TCPDEVICE
    if (bsd_tcp_handler_alive())
    {
        base->sb_Lib.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }
#endif

    /*
     * Same for an address allocation still running: those workers are
     * Processes of ours executing out of this segment while holding no OpenCnt
     * reference either (see the launch in addralloc.c). Each is a bounded
     * number of seconds from finishing, since the API it serves has a
     * mandatory timeout.
     */
    if (bsd_aam_busy())
    {
        base->sb_Lib.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }

    /*
     * Same while a monitoring hook is installed. This is documented behaviour,
     * not a leak: "It must be called before the library is closed, or the
     * library will stay in memory indefinitely."  Expunging with a hook in the
     * list would unload the segment out from under a caller that still
     * believes its hook is live.
     */
    if (bsd_netmon_busy())
    {
        base->sb_Lib.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }

    /*
     * Deregister before anything is freed.  bsd_address_changed() lives in the
     * segment about to be handed to UnLoadSeg(), and the netstack calls it
     * through a pointer src/common holds; leaving it registered is a call into
     * unloaded memory the moment an address changes afterwards.
     */
    ami_set_address_change_hook(NULL);
    ami_set_second_hook(NULL);
    ami_set_shutdown_hook(NULL);
    bsd_master_base = NULL;

    seglist = base->sb_SegList;
    neg     = base->sb_Lib.lib_NegSize;
    pos     = base->sb_Lib.lib_PosSize;

    /*
     * The DEVS:Internet tables are ami_alloc()ed once by the first open and
     * held in file-scope statics in src/config/netdb.c. Those statics go away
     * with the segment, so an expunge that does not free them first orphans
     * every block: 12,616 bytes on a stock DEVS:Internet, gone for good and
     * again on the next load. Nothing can be in a lookup here, OpenCnt is
     * zero and the checks above have ruled out our own Processes.
     */
    ami_netdb_free();

    bsd_runtime_close();

    /* Every retained child points into the segment about to unload. */
    bsd_dead_ring_flush();

    /*
     * The last moment anything of ours exists.  Everything the library ever
     * allocated has now been given back -- the netdb tables above, the runtime,
     * the retained bases -- so anything the census still holds is memory that
     * survives the segment, which on AmigaOS means until the machine is
     * rebooted.  tools/alloc-census.sh gates on this line.
     */
    AMI_CENSUS_REPORT("bsd-expunge");

    Remove((struct Node *)base);
    FreeMem((UBYTE *)base - neg, neg + pos);

    return seglist;
}

APTR bsd_lib_reserved(VOID)
{
    return NULL;
}
