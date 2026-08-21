/*
 * bsdsocket.library, runtime pieces a shared library has to supply itself.
 *
 * An AmigaOS shared library is loaded by Exec, not by a C startup. There is no
 * crt0 to open dos.library, no _exit, and no newlib reentrancy structure:
 *
 *   DOSBase   the crt normally defines and opens it. src/config talks to
 *             dos.library, so the library opens it in its own init.
 *
 *   rand()    NetX Duo used to take NX_RAND from <stdlib.h> (nx_api.h's
 *             default), and newlib's rand() reaches through _impure_ptr,
 *             which nothing has initialised. It also drags in lib_a-open.o,
 *             which wants _exit and takes the whole link down. NX_RAND now
 *             points at src/common/ami_random.c instead
 *             (port/netxduo-amiga/inc/nx_port.h), so nothing in the stack
 *             calls rand(). These definitions remain so that third-party
 *             code that calls rand() inside the library gets the pool rather
 *             than an uninitialised newlib.
 *
 *   weak      every definition is weak so a build which does have a crt (the
 *             test executables) keeps the crt's DOSBase and libc's rand.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_internal.h"

#include "aminetxduo/random.h"

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>

__attribute__((weak)) struct DosLibrary *DOSBase;

/* --------------------------------------------------------------- rand(), */

__attribute__((weak)) void srand(unsigned int seed)
{
    ami_random_srand(seed);
}

__attribute__((weak)) int rand(void)
{
    return ami_random_rand();
}

/* ------------------------------------------------------------- lifecycle, */

/*
 * Called from bsd_lib_init(), that is from InitResident() on the first
 * OpenLibrary(): a normal task context where OpenLibrary() is legal.
 */
BOOL bsd_runtime_open(VOID)
{
    if (DOSBase == NULL)
        DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);

    /* Seeded here rather than lazily on the first NX_RAND call: collection
       costs a measured 21-22 ms to sample E-Clock jitter, and the first
       NX_RAND call is on the outgoing-packet path. At InitResident() time
       this is a normal task context, so the delay costs nothing. */
    ami_random_init();

    return (DOSBase != NULL) ? TRUE : FALSE;
}

/* ------------------------------------------------------- usergroup.library */

/*
 * usergroup.library, opened and held for as long as this library is loaded.
 *
 * Nothing in here calls it. It is held so that the library NODE is in Exec's
 * library list, because that is the only way an ixemul.library program can
 * reach it.
 *
 * ixnet.library -- the half of ixemul that every GeekGadgets network client
 * goes through -- opens the stack like this, and the path is a literal in the
 * binary (checked with `strings` on ixnet-020.library 48.3, and the same in
 * the 47.2 sources on Aminet, dev/gg/ixemul-src.lha, sysdep/ixemul/ixnet/
 * ixnet_open.c):
 *
 *     if ((p->u_TCPBase = OpenLibrary("bsdsocket.library", 3))) {
 *         ...
 *         p->u_UserGroupBase = OpenLibrary("AmiTCP:libs/usergroup.library", 1);
 *         if (p->u_UserGroupBase) { ... IX_NETWORK_AMITCP; break; }
 *         FreeSignal(...); CloseLibrary(p->u_TCPBase);
 *     }
 *
 * So a machine with no AmiTCP: assign loses the socket layer entirely: ixnet
 * CLOSES the bsdsocket.library it just opened, reports no network, and
 * ixemul's socket() answers ENOSYS for the rest of the program's life. wget
 * prints "socket: Function not implemented" and gives up (GitHub #6).
 *
 * Exec resolves a path-qualified OpenLibrary() against the library list by
 * the file part of the name when the path itself cannot be loaded, so an
 * already-open usergroup.library answers for "AmiTCP:libs/usergroup.library"
 * and no assign is needed. A copy sitting unopened in LIBS: does NOT: that
 * was measured both ways, and only the resident node works.
 *
 * This is what Roadshow does. Its bsdsocket.library holds usergroup.library
 * open with lib_OpenCnt 1 from bring-up onwards, which is why the same wget
 * binary works there on a machine with no AmiTCP: anything, and why it did
 * not work here.
 *
 * Absent is not an error. A user who installed the library on its own has no
 * LIBS:usergroup.library, and everything except the GeekGadgets clients works
 * exactly as before.
 */
static struct Library *bsd_usergroup_base;
static BOOL            bsd_amitcp_tried;

/*
 * AmiTCP: -> SYS:, if nothing else has claimed the name.
 *
 * The residency trick above is what makes ixnet's lookup succeed, and it is a
 * trick: it works because Exec falls back to matching the file part, not
 * because the path means anything here. This makes the path mean something.
 * AmiTCP kept usergroup.library in AmiTCP:libs/ and nowhere else, which is why
 * ixnet asks for it there; LIBS: is SYS:Libs on a stock Workbench, so pointing
 * AmiTCP: at SYS: makes that same path reach the copy in LIBS: as a file.
 *
 * The installer makes this assign too, and this is not a substitute for it: a
 * machine that ran the installer has it from boot, before anything opens this
 * library. This is for the machine that did not -- a hand-installed library, a
 * library dropped into LIBS: by another package -- so that it works with
 * nothing for the user to do.
 *
 * NOT DONE IF AmiTCP: ALREADY RESOLVES. A real AmiTCP installation owns the
 * name and keeps its db, bin and libs drawers under it; taking it would break
 * every one of those paths. The installer distinguishes its own previous
 * assign from a real AmiTCP by looking for AmiTCP:db and AmiTCP:bin, and does
 * not need to here: existing at all is reason enough to leave it alone.
 *
 * It is not removed on expunge. An ixemul program that is still running holds
 * usergroup.library open through this very path, and it is a global name that
 * anything may have started using in the meantime. A dangling assign would be
 * the harm; an assign to SYS:, which is always there, is not.
 */
static VOID bsd_amitcp_assign(VOID)
{
    struct Process *me;
    APTR            saved;
    BPTR            lock;

    /* Requesters off across the probe, for the same reason as below: without
       an assign this is a lock on a volume that is not there. */
    me = (struct Process *)FindTask(NULL);
    if (me == NULL || me->pr_Task.tc_Node.ln_Type != NT_PROCESS)
        return;

    saved              = me->pr_WindowPtr;
    me->pr_WindowPtr   = (APTR)-1L;
    lock               = Lock((STRPTR)"AmiTCP:", SHARED_LOCK);
    me->pr_WindowPtr   = saved;

    if (lock != 0)
    {
        UnLock(lock);
        return;
    }

    lock = Lock((STRPTR)"SYS:", SHARED_LOCK);
    if (lock == 0)
        return;

    /* AssignLock() takes the lock on success and leaves it on failure. */
    if (AssignLock((STRPTR)"AmiTCP", lock) == DOSFALSE)
        UnLock(lock);
}

VOID bsd_usergroup_open(VOID)
{
    struct Process *me;
    APTR            saved;

    /* Once per load, and before anything else here: it is the reason the
       AmiTCP:libs/ fallback below can work, and bsd_lib_open() reaches this
       function on every OpenLibrary() while two Locks are worth doing once. */
    if (!bsd_amitcp_tried)
    {
        bsd_amitcp_tried = TRUE;
        bsd_amitcp_assign();
    }

    if (bsd_usergroup_base != NULL)
        return;

    /* LIBS: is where our own installer puts it, so this is the usual answer. */
    bsd_usergroup_base = OpenLibrary((STRPTR)"usergroup.library", 0);
    if (bsd_usergroup_base != NULL)
        return;

    /*
     * Not in LIBS:. A machine with AmiTCP installed keeps one in AmiTCP:libs/
     * -- which is the very path ixnet asks for -- so try it before giving up.
     * A hand-installed library on such a machine then works with nothing for
     * the user to do and nothing for them to be told.
     *
     * Requesters off across the attempt. Without an AmiTCP: assign this open
     * is a lock on a volume that is not there, and DOS would put "Please
     * insert volume AmiTCP:" in front of somebody who merely opened a socket.
     * pr_WindowPtr is the caller's, so it is saved and put back; and only a
     * Process has one, which a library opened from a Task does not.
     */
    me = (struct Process *)FindTask(NULL);
    if (me == NULL || me->pr_Task.tc_Node.ln_Type != NT_PROCESS)
        return;

    saved = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1L;
    bsd_usergroup_base =
        OpenLibrary((STRPTR)"AmiTCP:libs/usergroup.library", 0);
    me->pr_WindowPtr = saved;
}

VOID bsd_runtime_close(VOID)
{
    /* ami_millis() opened timer.device against a timerequest that is a
       file-scope static in this segment. Expunge is about to UnLoadSeg() it. */
    ami_timer_close();

    if (bsd_usergroup_base != NULL)
    {
        CloseLibrary(bsd_usergroup_base);
        bsd_usergroup_base = NULL;
    }

    /* Both of these are zero on a fresh load -- AmigaDOS clears HUNK_BSS -- so
       this matters only on the path where expunge runs and UnLoadSeg() does
       not, and the segment is opened again with its statics still set. Resetting
       here rather than trusting the loader keeps the flag paired with the thing
       it guards. */
    bsd_amitcp_tried = FALSE;

    if (DOSBase != NULL)
    {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
}
