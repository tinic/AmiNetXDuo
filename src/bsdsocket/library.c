/*
 * bsdsocket.library -- library skeleton and per-opener child bases.
 *
 * The romtag lives here, and so does the one structural requirement that
 * everything else depends on (docs/RESEARCH.md S3.1): every OpenLibrary()
 * returns a *distinct* base, cloned from the master, carrying that task's
 * descriptor table, errno pointer and tag state. SocketBase is never shared.
 *
 * This file must be linked FIRST so that the "moveq #-1,d0 / rts" below is at
 * offset 0 of the first code hunk -- that is what makes the library file
 * harmless if someone tries to execute it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>

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

    child = (struct AmiSocketBase *)(mem + neg);

    child->sb_Lib.lib_OpenCnt = 1;
    child->sb_Lib.lib_Flags   = LIBF_CHANGED;
    child->sb_Master          = master;
    child->sb_Task            = FindTask(NULL);

    /*
     * The clone carries copies of the master-only fields, and a copied
     * SignalSemaphore holds pointers into the master's wait queue. Children
     * never touch either, but leaving live-looking pointers around in a
     * system with no memory protection is asking for a bad afternoon.
     */
    bsd_bzero(&child->sb_Lock, sizeof(child->sb_Lock));
    bsd_bzero(&child->sb_Children, sizeof(child->sb_Children));
    child->sb_StackRefs = 0;

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

    /*
     * The wakeup signal belongs to the opening task, which is exactly the
     * task this base belongs to. Everything that can make a socket ready --
     * NetX Duo's receive/connect/disconnect callbacks -- ends up signalling
     * it, so WaitSelect() can Wait() on it alongside the caller's own bits.
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

    ObtainSemaphore(&master->sb_Lock);
    Remove((struct Node *)&child->sb_Node);
    ReleaseSemaphore(&master->sb_Lock);

    FreeMem((UBYTE *)child - neg, neg + pos);
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

    if (master->sb_StackRefs == 0)
    {
        if (netstack_startup() != AMI_NET_OK)
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

    return child;
}

APTR bsd_lib_close(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    struct AmiSocketBase *base   = SocketBase;
    struct AmiSocketBase *master = base;

    if (base->sb_Master != NULL)
    {
        master = base->sb_Master;
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

    seglist = base->sb_SegList;
    neg     = base->sb_Lib.lib_NegSize;
    pos     = base->sb_Lib.lib_PosSize;

    Remove((struct Node *)base);
    FreeMem((UBYTE *)base - neg, neg + pos);

    return seglist;
}

APTR bsd_lib_reserved(VOID)
{
    return NULL;
}
