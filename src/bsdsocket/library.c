/*
 * bsdsocket.library, library skeleton and per-opener child bases.
 *
 * Link this file first so the "moveq #-1,d0 / rts" below sits at offset 0 of
 * the first code hunk, which makes the library file harmless when it is run.
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
#include "aminetxduo/events.h"

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

/* Running the library file returns a failure code rather than crashing. */
asm("    .text                       \n"
    "    .globl _bsdsocket_entry     \n"
    "_bsdsocket_entry:               \n"
    "    moveq  #-1,%d0              \n"
    "    rts                         \n");

static char bsd_lib_name[]  = BSD_LIB_NAME;
#define BSD_STR2(x)         #x
#define BSD_STR(x)          BSD_STR2(x)
#define BSD_LIB_ABI_TEXT    BSD_STR(BSD_LIB_VERSION) "." BSD_STR(BSD_LIB_REVISION)

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
    (APTR)NULL,                 /* no data table, we initialise by hand */
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

/* Open-coded NewList(). amiga.lib is not available to a shared library. */
static VOID bsd_new_list(struct MinList *list)
{
    list->mlh_Head     = (struct MinNode *)&list->mlh_Tail;
    list->mlh_Tail     = NULL;
    list->mlh_TailPred = (struct MinNode *)&list->mlh_Head;
}

/*
 * SBTC_SIG_ADDRESS_CHANGE_MASK: the openers that asked to hear about an
 * interface address arriving, changing or going away.
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

/* Never inside Disable(): the caller can free memory on the answer, and
   FreeMem() must not be called with interrupts off. */
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

    if (!alive && bsd_dead_task_signals++ == 0UL)
    {
        AMI_WARN("bsdsocket: an address change was not delivered to task %lx: "
                 "it asked for SBTC_SIG_ADDRESS_CHANGE_MASK and then exited "
                 "without closing the library", (unsigned long)task);
    }
}

/*
 * Once a second, forget the tasks that are gone.
 */
static ULONG bsd_latched_tasks;

static VOID bsd_task_sweep(VOID)
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

        if (child->sb_Task == NULL || bsd_task_alive(child->sb_Task))
            continue;

#ifdef AMINETXDUO_GREEN_REALM
        if (!child->sb_NxSweepSeen)
        {
            child->sb_NxSweepSeen = TRUE;
#endif
        bsd_latched_tasks++;
        AMI_WARN("bsdsocket: task %lx exited without closing the library. "
                 "Its base will no longer be signalled (%lu so far)",
                 (unsigned long)child->sb_Task,
                 (unsigned long)bsd_latched_tasks);
#ifdef AMINETXDUO_GREEN_REALM
        }
#endif

        child->sb_NxNest = 0;
        ami_netstack_release(&child->sb_NxCaller);

        if (!bsd_nx_orphan(child))
            continue;

        child->sb_Task = NULL;
    }

    ReleaseSemaphore(&master->sb_Lock);
}

/*
 * The ARexx port's KILL, and anything else in the netstack that decides the
 * network is finished with.
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

    n68k_cpu_select((ULONG)sysbase->AttnFlags);

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
    base->sb_StackRefs          = 0;
    base->sb_TransientStackRefs = 0;
    base->sb_StackHeld          = FALSE;
    bsd_handoff_init(base);

    bsd_master_base = base;

    ami_event_publish();

    ami_set_address_change_hook(bsd_address_changed);
    ami_set_second_hook(bsd_task_sweep);
    ami_set_shutdown_hook(bsd_shutdown_requested);

    return base;
}

#define BSD_DEAD_RING           8

static APTR         bsd_dead_block[BSD_DEAD_RING];
static ULONG        bsd_dead_bytes[BSD_DEAD_RING];
static struct Task *bsd_dead_owner[BSD_DEAD_RING];
static UINT         bsd_dead_next;

static ULONG bsd_dead_calls;

static LONG bsd_dead_base_call(VOID)
{
    if (bsd_dead_calls == 0UL)
    {
        AMI_WARN("bsdsocket: a call arrived through a base that was already "
                 "closed. It is answered with 0. The caller closed the "
                 "library and kept its pointer.");
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

static VOID bsd_retain_dead(UBYTE *block, ULONG neg, ULONG bytes,
                            struct Task *owner)
{
    UINT slot;

    bsd_poison_vectors(block, neg);

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

    AMI_CENSUS_ADD(mem, neg + pos);

    CopyMem((UBYTE *)master - neg, mem, neg + pos);

    CacheClearU();

    child = (struct AmiSocketBase *)(mem + neg);

    child->sb_Lib.lib_OpenCnt = 1;
    child->sb_Lib.lib_Flags   = LIBF_CHANGED;
    child->sb_Master          = master;
    child->sb_Task            = FindTask(NULL);

    bsd_prof_segtag(child, master->sb_SegList);

    bsd_bzero(&child->sb_Lock, sizeof(child->sb_Lock));
    bsd_bzero(&child->sb_Children, sizeof(child->sb_Children));
    bsd_bzero(&child->sb_Handoffs, sizeof(child->sb_Handoffs));
    child->sb_StackRefs          = 0;
    child->sb_TransientStackRefs = 0;
    child->sb_StackHeld          = FALSE;
    child->sb_NextHandoffId      = 0;

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

    /* The autodoc's defaults: LOG_USER and 0xFF, not zero. LOG_USER is 1<<3,
       the NDK's <sys/syslog.h> ships the priorities and not the facility
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

    child->sb_ServCursor  = 0;
    child->sb_ProtoCursor = 0;
    child->sb_NetCursor   = 0;

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

    ami_mem_open_delta(1);

    return child;
}

static VOID bsd_child_destroy(struct AmiSocketBase *child)
{
    struct AmiSocketBase *master = child->sb_Master;
    ULONG                 neg    = child->sb_Lib.lib_NegSize;
    ULONG                 pos    = child->sb_Lib.lib_PosSize;

    bsd_close_all(child);

    bsd_bpf_close_all(child);

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

    /* Last, because everything above can take the bracket: drop the base's
       cached ThreadX registration. This is the one place that runs on the
       base's own task with every socket already closed (netx_call.c). */
    bsd_nx_release(child);

    ObtainSemaphore(&master->sb_Lock);
    Remove((struct Node *)&child->sb_Node);
    ReleaseSemaphore(&master->sb_Lock);

    ami_mem_open_delta(-1);

    bsd_retain_dead((UBYTE *)child - neg, neg, neg + pos, child->sb_Task);
}

/*
 * netstack_startup() parses DEVS: and runs all of NetX Duo's init on the
 * calling task's stack, it adopts that task (netstack.c, ami_ns_bring_up()).
 */
#define BSD_STARTUP_STACK   (64UL * 1024UL)

typedef struct
{
    struct Task *nb_Parent;
    ULONG        nb_SigMask;
    LONG         nb_Result;
} BsdNetBoot;

static BsdNetBoot *bsd_net_boot;

static LONG bsd_netstack_start_owned(VOID)
{
    LONG result = netstack_startup();

    if (result != AMI_NET_OK)
        netstack_shutdown();

    return result;
}

static VOID bsd_netstack_boot_main(VOID)
{
    BsdNetBoot *b = bsd_net_boot;

    b->nb_Result = bsd_netstack_start_owned();

    Signal(b->nb_Parent, b->nb_SigMask);
}

static LONG bsd_netstack_bringup(VOID)
{
    BsdNetBoot      boot;
    struct TagItem  tags[5];
    struct Process *proc;
    BYTE            sig;

    /* A private signal, never SIGF_SINGLE: the ThreadX port uses SIGF_SINGLE as
       its thread run-signal, so sharing it wakes this Wait() early. */
    sig = (BYTE)AllocSignal(-1);
    if (sig < 0)
        return bsd_netstack_start_owned(); /* no signal: caller-stack fallback */

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
        bsd_net_boot = NULL;
        FreeSignal(sig);
        return bsd_netstack_start_owned();
    }

    Wait(boot.nb_SigMask);
    bsd_net_boot = NULL;
    FreeSignal(sig);

    return boot.nb_Result;
}

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

    master->sb_Lib.lib_OpenCnt++;

    ObtainSemaphore(&master->sb_Lock);

    (VOID)ami_netdb_load();

    bsd_usergroup_open();

    if (master->sb_StackRefs == 0)
    {
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
    BOOL                  unload_is_safe = FALSE;

    if (base->sb_Master != NULL)
    {
        master = base->sb_Master;

        ObtainSemaphore(&master->sb_Lock);
        if (master->sb_StackRefs >= master->sb_TransientStackRefs &&
            master->sb_StackRefs - master->sb_TransientStackRefs <= 1)
            bsd_handoff_flush(base);
        ReleaseSemaphore(&master->sb_Lock);

        bsd_child_destroy(base);

        /*
         * The teardown runs with the lock held, and has to: the decrement that
         * reaches zero and the shutdown it triggers are one step. If the lock
         */
        ObtainSemaphore(&master->sb_Lock);
        if (master->sb_StackRefs > 0 && --master->sb_StackRefs == 0)
        {
            netstack_shutdown();
            unload_is_safe = netstack_can_unload();
        }
        ReleaseSemaphore(&master->sb_Lock);

        if (unload_is_safe)
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
 * Keep the netstack alive for an asynchronous worker after the opener that
 * launched it is allowed to close. The worker count, not lib_OpenCnt, keeps
 * this segment loaded; this reference is only about the netstack lifetime.
 */
LONG bsd_stack_transient_hold(struct AmiSocketBase *base)
{
    struct AmiSocketBase *master = base;
    LONG                  rc = -1;

    if (master == NULL)
        return -1;

    if (master->sb_Master != NULL)
        master = master->sb_Master;

    ObtainSemaphore(&master->sb_Lock);

    if (master->sb_StackRefs != 0 &&
        master->sb_StackRefs != (ULONG)-1 &&
        master->sb_TransientStackRefs != (ULONG)-1)
    {
        master->sb_StackRefs++;
        master->sb_TransientStackRefs++;
        rc = 0;
    }

    ReleaseSemaphore(&master->sb_Lock);
    return rc;
}

VOID bsd_stack_transient_release(struct AmiSocketBase *base)
{
    struct AmiSocketBase *master = base;
    BOOL                  unload_is_safe = FALSE;

    if (master == NULL)
        return;

    if (master->sb_Master != NULL)
        master = master->sb_Master;

    ObtainSemaphore(&master->sb_Lock);

    if (master->sb_TransientStackRefs != 0 && master->sb_StackRefs != 0)
    {
        master->sb_TransientStackRefs--;
        if (--master->sb_StackRefs == 0)
        {
            netstack_shutdown();
            unload_is_safe = netstack_can_unload();
        }
    }

    ReleaseSemaphore(&master->sb_Lock);

    if (unload_is_safe)
        AMI_CENSUS_REPORT("bsd-stack-down");
}

/*
 * NETCTRL_STACK_HOLD: the library takes its own reference to the stack it is
 * running, so the command that started the network can close its base like any
 * other opener and the network still stands afterwards.
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

            ami_event(NETEVENT_RELEASE, NETEVENT_NOINDEX,
                      (ULONG)master->sb_Lib.lib_OpenCnt);
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
 * Ctrl-C is asking to be woken on that bit instead, and a notification on a
 * bit it does not wait for reaches nothing. Neither reference implementation
 * reads the mask here. Sending both covers the program that set one without
 * missing the program that expects the standard bit.
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

    ami_event(NETEVENT_NOTIFY, NETEVENT_NOINDEX, n);

    if (signalled != NULL)
        *signalled = n;

    return 0;
}

static UWORD bsd_base_sockets(const struct AmiSocketBase *child)
{
    ULONG i;
    UWORD n = 0;

    if (child->sb_Table == NULL)
        return 0;

    for (i = 0; i < (ULONG)child->sb_TableSize; i++)
    {
        if (child->sb_Table[i] != NULL &&
            child->sb_Table[i] != BSD_FD_RESERVED)
            n++;
    }

    return n;
}

/*
 * What to call an opener in a report.
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
                   Shell has no command, so the task name is used. */
                if (bstr != NULL && bstr[0] != 0)
                {
                    LONG len   = (LONG)bstr[0];
                    LONG start = 0;
                    LONG j;

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
        ami_event(NETEVENT_EXPUNGE_DECLINED, NETEVENT_NOINDEX,
                  NETEVENT_EXP_OPEN);
        base->sb_Lib.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }

    if (!netstack_can_unload())
    {
        ami_event(NETEVENT_EXPUNGE_DECLINED, NETEVENT_NOINDEX,
                  NETEVENT_EXP_KERNEL);
        base->sb_Lib.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }

#ifdef AMINETXDUO_TCPDEVICE
    if (bsd_tcp_handler_alive())
    {
        ami_event(NETEVENT_EXPUNGE_DECLINED, NETEVENT_NOINDEX,
                  NETEVENT_EXP_TCP);
        base->sb_Lib.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }
#endif

    if (bsd_aam_busy())
    {
        ami_event(NETEVENT_EXPUNGE_DECLINED, NETEVENT_NOINDEX,
                  NETEVENT_EXP_ADDRALLOC);
        base->sb_Lib.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }

    if (bsd_netmon_busy())
    {
        ami_event(NETEVENT_EXPUNGE_DECLINED, NETEVENT_NOINDEX,
                  NETEVENT_EXP_NETMON);
        base->sb_Lib.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }

    /*
     * Deregister before anything is freed.  bsd_address_changed() lives in the
     */
    ami_set_address_change_hook(NULL);
    ami_set_second_hook(NULL);
    ami_set_shutdown_hook(NULL);
    bsd_master_base = NULL;

    ami_event_unpublish();

    seglist = base->sb_SegList;
    neg     = base->sb_Lib.lib_NegSize;
    pos     = base->sb_Lib.lib_PosSize;

    ami_netdb_free();

    bsd_runtime_close();

    /* Every retained child points into the segment about to unload. */
    bsd_dead_ring_flush();

    AMI_CENSUS_REPORT("bsd-expunge");

    Remove((struct Node *)base);
    FreeMem((UBYTE *)base - neg, neg + pos);

    return seglist;
}

APTR bsd_lib_reserved(VOID)
{
    return NULL;
}
