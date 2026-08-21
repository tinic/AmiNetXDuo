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

#include <dos/dosextens.h>
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

VOID bsd_usergroup_open(VOID)
{
    struct Process *me;
    APTR            saved;

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

    if (DOSBase != NULL)
    {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
}
