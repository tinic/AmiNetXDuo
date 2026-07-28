/*
 * bsdsocket.library -- library skeleton and per-opener child bases.
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
#include "tcp_handler.h"
#include "interfaces.h"
#include "netmonitor.h"

#include "aminetxduo/config.h"

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
static char bsd_lib_id[]    = "bsdsocket.library 4.0 (AmiNetXDuo)\r\n";

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

/* -------------------------------------------------------------- utilities -- */

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

    while (size-- > 0)
        *q++ = 0;
}

VOID bsd_bcopy(const APTR src, APTR dst, ULONG size)
{
    if (size > 0)
        CopyMem((APTR)src, dst, size);
}

/* Open-coded NewList(); amiga.lib is not available to a shared library. */
static VOID bsd_new_list(struct MinList *list)
{
    list->mlh_Head     = (struct MinNode *)&list->mlh_Tail;
    list->mlh_Tail     = NULL;
    list->mlh_TailPred = (struct MinNode *)&list->mlh_Head;
}

/* ------------------------------------------------------------------- init -- */

static struct AmiSocketBase *bsd_lib_init(
    register struct AmiSocketBase *base    __asm("d0"),
    register APTR                  seglist __asm("a0"),
    register struct ExecBase      *sysbase __asm("a6"))
{
    SysBase = sysbase;

    /* A shared library gets no C startup: dos.library and the random number
       generator are ours to set up (library_runtime.c). */
    if (!bsd_runtime_open())
        return NULL;

    base->sb_SegList = seglist;
    base->sb_SysBase = sysbase;
    base->sb_Master  = NULL;

    base->sb_Lib.lib_Node.ln_Type = NT_LIBRARY;
    base->sb_Lib.lib_Node.ln_Name = bsd_lib_name;
    base->sb_Lib.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    base->sb_Lib.lib_Version      = BSD_LIB_VERSION;
    base->sb_Lib.lib_Revision     = BSD_LIB_REVISION;
    base->sb_Lib.lib_IdString     = bsd_lib_id;

    InitSemaphore(&base->sb_Lock);
    bsd_new_list(&base->sb_Children);
    base->sb_StackRefs = 0;
    bsd_handoff_init(base);

    return base;
}

/* -------------------------------------------------------- child base life -- */

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

    /* Clone the jump table as well as the data: the child *is* a library. */
    CopyMem((UBYTE *)master - neg, mem, neg + pos);

    /*
     * The negative half just copied is code (the LVO jump table). On a 68030
     * and up with the instruction cache on, the CPU may still hold stale lines
     * for this block -- it was very likely somebody else's freed code a moment
     * ago -- so a call through the child's vectors would execute those instead
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

    child->sb_LogTag      = NULL;
    child->sb_LogStat     = 0;
    child->sb_LogFacility = 0;
    child->sb_LogMask     = 0;
    child->sb_FDCallback  = NULL;

    child->sb_TimerOpen = FALSE;

    /* The netdb iterators (netdb.c) start at the top of each table. */
    child->sb_ServCursor  = 0;
    child->sb_ProtoCursor = 0;
    child->sb_NetCursor   = 0;

    /*
     * The wakeup signal belongs to the opening task, which is the task this
     * base belongs to. Everything that can make a socket ready -- NetX Duo's
     * receive/connect/disconnect callbacks -- signals it, so WaitSelect() can
     * Wait() on it alongside the caller's own bits.
     */
    sig = ami_signal_alloc();
    if (sig < 0)
    {
        FreeMem(mem, neg + pos);
        return NULL;
    }
    child->sb_EventSignal  = sig;
    child->sb_EventSigMask = 1UL << sig;

    ObtainSemaphore(&master->sb_Lock);
    AddTail((struct List *)&master->sb_Children, (struct Node *)&child->sb_Node);
    ReleaseSemaphore(&master->sb_Lock);

    return child;
}

static VOID bsd_child_destroy(struct AmiSocketBase *child)
{
    struct AmiSocketBase *master = child->sb_Master;
    ULONG                 neg    = child->sb_Lib.lib_NegSize;
    ULONG                 pos    = child->sb_Lib.lib_PosSize;

    bsd_close_all(child);

    if (child->sb_TimerOpen)
    {
        CloseDevice((struct IORequest *)&child->sb_TimerReq);
        child->sb_TimerOpen = FALSE;
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

    FreeMem((UBYTE *)child - neg, neg + pos);
}

/* --------------------------------------------------- stack bring-up proc -- */
/*
 * netstack_startup() parses DEVS: and runs all of NetX Duo's init on the
 * calling task's stack -- it adopts that task (netstack.c, ami_ns_bring_up()).
 * Opened from a command with the ~4 KB startup-sequence Shell stack, which is
 * how AddNetInterface comes up at boot, that overflows and smashes the return
 * path; with no memory protection it surfaces as an illegal instruction or
 * line-F at a wild address seconds later.
 *
 * So the first open runs the bring-up on a Process of our own with a stack we
 * choose, and waits for its result; later opens just take a reference. The
 * handshake mirrors bsd_tcp_handler_start(): a boot record on this stack, a
 * file-scope pointer to it, and a private signal (not SIGF_SINGLE -- see
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
       after it returns -- the parent Wait()s the whole time and `boot` lives
       on its stack until then. */
    b->nb_Result = netstack_startup();
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
       this Wait() early -- we would return and pop this stack frame (with
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

/* ------------------------------------------------------------ exec vectors -- */

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
     * from a lookup. netstack_startup() below reaches it too via
     * ami_config_load(); calling it explicitly keeps the netdb up even if that
     * path changes.
     */
    (VOID)ami_netdb_load();

    if (master->sb_StackRefs == 0)
    {
        /* Bring the stack up on our own big-stack Process, not the opener's
           stack -- see bsd_netstack_bringup() above. */
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
    bsd_tcp_handler_start(master);

    return child;
}

APTR bsd_lib_close(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    struct AmiSocketBase *base   = SocketBase;
    struct AmiSocketBase *master = base;

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

        ObtainSemaphore(&master->sb_Lock);
        if (master->sb_StackRefs > 0 && --master->sb_StackRefs == 0)
            netstack_shutdown();
        ReleaseSemaphore(&master->sb_Lock);
    }

    if (master->sb_Lib.lib_OpenCnt > 0)
        master->sb_Lib.lib_OpenCnt--;

    if (master->sb_Lib.lib_OpenCnt == 0 &&
        (master->sb_Lib.lib_Flags & LIBF_DELEXP) != 0)
        return bsd_lib_expunge(master);

    return NULL;
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
     * hand back for UnLoadSeg(), and it holds no OpenCnt reference -- it takes
     * one only while a file handle is open, so OpenCnt at zero means it is
     * idle, not gone. There is no way to prove it is not executing, so decline
     * the expunge. ACTION_DIE takes TCP: down; after that this succeeds.
     */
    if (bsd_tcp_handler_alive())
    {
        base->sb_Lib.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }

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

    seglist = base->sb_SegList;
    neg     = base->sb_Lib.lib_NegSize;
    pos     = base->sb_Lib.lib_PosSize;

    bsd_runtime_close();

    Remove((struct Node *)base);
    FreeMem((UBYTE *)base - neg, neg + pos);

    return seglist;
}

APTR bsd_lib_reserved(VOID)
{
    return NULL;
}
