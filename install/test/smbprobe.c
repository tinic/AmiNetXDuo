/*
 * SMBProbe, ask a mounted device the two questions `List` cannot answer on a
 * machine with nobody at the mouse.
 *
 *     SMBProbe LOCK DEVICE=SMB2: LOG=DH0:probe.txt
 *     SMBProbe WINDOWS LOG=DH0:windows.txt
 *
 * WHY THIS EXISTS.  `List SMB2:` on a device whose handler cannot mount does
 * not return an error: DOS puts up a "Please insert volume" requester and
 * waits for it to be answered.  On a headless emulator that requester is
 * invisible and the command looks like it hung, which is exactly the shape of
 * the report being investigated -- and telling the two apart is the whole
 * question.
 *
 *   LOCK     sets pr_WindowPtr to -1, so DOS answers its own requesters with
 *            Cancel and Lock() RETURNS with an error code instead of
 *            blocking, then Examines the lock and walks it.  Every step is
 *            timestamped and every IoErr() is printed.
 *   WINDOWS  walks Intuition's screen and window lists and prints what is on
 *            them.  Run while something else is blocked, it says whether a
 *            requester is up and which window owns it.
 *
 * Each line is written and flushed as it happens, because the interesting
 * runs are the ones that never reach the end.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>

#include <proto/exec.h>
#include <proto/dos.h>

#define TEMPLATE "LOCK/S,WINDOWS/S,TASKS/S,DEVICE/K,LOG/K"

enum { ARG_LOCK = 0, ARG_WINDOWS, ARG_TASKS, ARG_DEVICE, ARG_LOG,
       ARG_COUNT };

static BPTR gLog;
static struct DateStamp gStart;

static void emit(const char *fmt, LONG a, LONG b, LONG c, LONG d)
{
    LONG args[4];

    args[0] = a;
    args[1] = b;
    args[2] = c;
    args[3] = d;
    if (gLog)
    {
        VFPrintf(gLog, (STRPTR)fmt, args);
        Flush(gLog);
    }
}

/* Milliseconds since the probe started.  DateStamp is the only clock a
 * program can read without opening timer.device, and 1/50 s is finer than
 * anything measured here. */
static LONG elapsed(void)
{
    struct DateStamp now;

    DateStamp(&now);
    return ((now.ds_Days - gStart.ds_Days) * 1440L
            + (now.ds_Minute - gStart.ds_Minute)) * 60000L
           + ((now.ds_Tick - gStart.ds_Tick) * 1000L) / TICKS_PER_SECOND;
}

static int probe_lock(STRPTR dev)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved = me->pr_WindowPtr;
    struct FileInfoBlock *fib;
    struct InfoData *id;
    BPTR lock;
    LONG n = 0;
    int rc = 0;

    emit("device %s\n", (LONG)dev, 0, 0, 0);
    emit("requesters off (pr_WindowPtr = -1)\n", 0, 0, 0, 0);

    me->pr_WindowPtr = (APTR)-1;

    SetIoErr(0);
    lock = Lock(dev, SHARED_LOCK);
    emit("Lock          %ld ms   lock=%ld  IoErr=%ld\n",
         elapsed(), (LONG)lock, IoErr(), 0);

    if (!lock)
    {
        me->pr_WindowPtr = saved;
        emit("RESULT lock=failed\n", 0, 0, 0, 0);
        return 10;
    }

    /* InfoData has no DOS object type; it must be long-word aligned, which
     * AllocMem guarantees and a stack variable does not. */
    id = (struct InfoData *)AllocMem(sizeof(struct InfoData),
                                     MEMF_PUBLIC | MEMF_CLEAR);
    if (id)
    {
        SetIoErr(0);
        if (Info(lock, id))
            emit("Info          %ld ms   blocks=%ld used=%ld blocksize=%ld\n",
                 elapsed(), id->id_NumBlocks, id->id_NumBlocksUsed,
                 id->id_BytesPerBlock);
        else
            emit("Info          %ld ms   failed IoErr=%ld\n",
                 elapsed(), IoErr(), 0, 0);
        FreeMem(id, sizeof(struct InfoData));
    }

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib)
    {
        UnLock(lock);
        me->pr_WindowPtr = saved;
        emit("RESULT lock=ok examine=nomem\n", 0, 0, 0, 0);
        return 10;
    }

    SetIoErr(0);
    if (Examine(lock, fib))
    {
        emit("Examine       %ld ms   name=%s size=%ld\n",
             elapsed(), (LONG)fib->fib_FileName, fib->fib_Size, 0);
        while (ExNext(lock, fib))
        {
            emit("  entry       %ld ms   %s  %ld bytes  type %ld\n",
                 elapsed(), (LONG)fib->fib_FileName, fib->fib_Size,
                 fib->fib_DirEntryType);
            if (++n >= 64)
                break;
        }
        emit("ExNext ended  %ld ms   %ld entries  IoErr=%ld\n",
             elapsed(), n, IoErr(), 0);
    }
    else
    {
        emit("Examine       %ld ms   failed IoErr=%ld\n",
             elapsed(), IoErr(), 0, 0);
        rc = 10;
    }

    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    me->pr_WindowPtr = saved;
    emit("RESULT lock=ok entries=%ld\n", n, 0, 0, 0);
    return rc;
}

/* Collected under Forbid() and printed after it: Write() breaks a Forbid and
 * the whole point of the Forbid is that the lists do not move underneath. */
#define MAXWIN 24

struct seen
{
    char screen[40];
    char window[64];
    LONG flags;
    LONG hasreq;
};

static struct seen gSeen[MAXWIN];

static void copyz(char *dst, const char *src, int max)
{
    int i = 0;

    if (!src)
        src = "(none)";
    while (src[i] && i < max - 1)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int probe_windows(void)
{
    struct IntuitionBase *ib;
    struct Screen *scr;
    struct Window *win;
    int n = 0, i;

    ib = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    if (!ib)
    {
        emit("intuition.library will not open\n", 0, 0, 0, 0);
        return 10;
    }

    Forbid();
    for (scr = ib->FirstScreen; scr && n < MAXWIN; scr = scr->NextScreen)
    {
        for (win = scr->FirstWindow; win && n < MAXWIN; win = win->NextWindow)
        {
            copyz(gSeen[n].screen, (const char *)scr->Title, 40);
            copyz(gSeen[n].window, (const char *)win->Title, 64);
            gSeen[n].flags = (LONG)win->Flags;
            gSeen[n].hasreq = win->FirstRequest ? 1 : 0;
            n++;
        }
    }
    Permit();

    for (i = 0; i < n; i++)
        emit("window  screen=\"%s\"  title=\"%s\"  flags=0x%08lx  requester=%ld\n",
             (LONG)gSeen[i].screen, (LONG)gSeen[i].window,
             gSeen[i].flags, gSeen[i].hasreq);
    emit("RESULT windows=%ld\n", (LONG)n, 0, 0, 0);

    CloseLibrary((struct Library *)ib);
    return 0;
}


/* Every task in the system, with what it is waiting for.  A handler that
 * never answered a packet is either TS_WAIT on some signal, which names what
 * it is blocked on, or TS_READY, which means it is spinning.  Collected under
 * Forbid for the same reason the window walk is. */
#define MAXTASK 48

struct tseen
{
    char name[40];
    LONG state;
    LONG sigwait;
    LONG sigrecvd;
    LONG isproc;
    LONG msgs;
};

static struct tseen gTask[MAXTASK];

static int gather(struct List *l, int n, LONG state)
{
    struct Node *nd;

    for (nd = l->lh_Head; nd->ln_Succ && n < MAXTASK; nd = nd->ln_Succ)
    {
        struct Task *t = (struct Task *)nd;
        struct Process *p = (struct Process *)t;
        LONG m = 0;

        copyz(gTask[n].name, (const char *)nd->ln_Name, 40);
        gTask[n].state = state;
        gTask[n].sigwait = (LONG)t->tc_SigWait;
        gTask[n].sigrecvd = (LONG)t->tc_SigRecvd;
        gTask[n].isproc = (nd->ln_Type == NT_PROCESS) ? 1 : 0;
        if (gTask[n].isproc)
        {
            struct Node *mn;
            for (mn = p->pr_MsgPort.mp_MsgList.lh_Head;
                 mn->ln_Succ; mn = mn->ln_Succ)
                m++;
        }
        gTask[n].msgs = m;
        n++;
    }
    return n;
}

static int probe_tasks(void)
{
    struct ExecBase *sb = SysBase;
    int n = 0, i;

    Forbid();
    n = gather(&sb->TaskWait, n, 1);
    n = gather(&sb->TaskReady, n, 2);
    if (n < MAXTASK && sb->ThisTask)
    {
        copyz(gTask[n].name, (const char *)sb->ThisTask->tc_Node.ln_Name, 40);
        gTask[n].state = 3;
        gTask[n].sigwait = (LONG)sb->ThisTask->tc_SigWait;
        gTask[n].sigrecvd = (LONG)sb->ThisTask->tc_SigRecvd;
        gTask[n].isproc = 1;
        gTask[n].msgs = 0;
        n++;
    }
    Permit();

    emit("state 1=waiting 2=ready 3=running\n", 0, 0, 0, 0);
    for (i = 0; i < n; i++)
    {
        emit("task  state=%ld  proc=%ld  msgs=%ld  ",
             gTask[i].state, gTask[i].isproc, gTask[i].msgs, 0);
        emit("sigwait=0x%08lx  sigrecvd=0x%08lx  \"%s\"\n",
             gTask[i].sigwait, gTask[i].sigrecvd, (LONG)gTask[i].name, 0);
    }
    emit("RESULT tasks=%ld\n", (LONG)n, 0, 0, 0);
    return 0;
}

int main(void)
{
    LONG args[ARG_COUNT];
    struct RDArgs *rd;
    STRPTR logname, dev;
    int rc = 20;
    int i;

    for (i = 0; i < ARG_COUNT; i++)
        args[i] = 0;

    rd = ReadArgs((STRPTR)TEMPLATE, args, NULL);
    if (!rd)
    {
        PrintFault(IoErr(), (STRPTR)"SMBProbe");
        return 20;
    }

    logname = args[ARG_LOG] ? (STRPTR)args[ARG_LOG] : (STRPTR)"DH0:smbprobe.txt";
    gLog = Open(logname, MODE_NEWFILE);
    DateStamp(&gStart);

    if (args[ARG_TASKS])
    {
        emit("SMBProbe TASKS\n", 0, 0, 0, 0);
        rc = probe_tasks();
    }
    else if (args[ARG_WINDOWS])
    {
        emit("SMBProbe WINDOWS\n", 0, 0, 0, 0);
        rc = probe_windows();
    }
    else if (args[ARG_LOCK])
    {
        dev = args[ARG_DEVICE] ? (STRPTR)args[ARG_DEVICE] : (STRPTR)"SMB2:";
        emit("SMBProbe LOCK\n", 0, 0, 0, 0);
        rc = probe_lock(dev);
    }
    else
    {
        emit("neither LOCK nor WINDOWS given\n", 0, 0, 0, 0);
    }

    if (gLog)
    {
        Close(gLog);
        gLog = 0;
    }
    FreeArgs(rd);
    return rc;
}
