/*
 * bsdsocket.library -- runtime pieces a shared library has to supply itself.
 *
 * An AmigaOS shared library is loaded by Exec, not by a C startup, so there is
 * no crt0 to open dos.library, no _exit, and no newlib reentrancy structure:
 *
 *   DOSBase   the crt normally defines and opens it; src/config talks to
 *             dos.library, so the library opens it in its own init.
 *
 *   rand()    NetX Duo used to take NX_RAND from <stdlib.h> (nx_api.h's
 *             default), and newlib's rand() reaches through _impure_ptr --
 *             which nothing has initialised. Pulling it in also drags
 *             lib_a-open.o, which wants _exit and takes the whole link down.
 *             NX_RAND now points at src/common/ami_random.c instead
 *             (port/netxduo-amiga/inc/nx_port.h), so nothing in the stack
 *             calls rand(); these definitions remain so that third-party code
 *             calling rand() inside the library gets the pool rather than an
 *             uninitialised newlib.
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

/* --------------------------------------------------------------- rand() -- */

__attribute__((weak)) void srand(unsigned int seed)
{
    ami_random_srand(seed);
}

__attribute__((weak)) int rand(void)
{
    return ami_random_rand();
}

/* ------------------------------------------------------------- lifecycle -- */

/*
 * Called from bsd_lib_init(), i.e. from InitResident() on the first
 * OpenLibrary(): a normal task context where OpenLibrary() is legal.
 */
BOOL bsd_runtime_open(VOID)
{
    if (DOSBase == NULL)
        DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);

    /* Seed here rather than lazily on the first NX_RAND call: collection costs
       a measured 21-22 ms sampling E-Clock jitter, and the first NX_RAND call
       is on the outgoing-packet path. At InitResident() time this is a normal
       task context, so the delay costs nothing. */
    ami_random_init();

    return (DOSBase != NULL) ? TRUE : FALSE;
}

VOID bsd_runtime_close(VOID)
{
    if (DOSBase != NULL)
    {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
}
