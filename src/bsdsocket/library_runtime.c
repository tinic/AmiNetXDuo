/*
 * bsdsocket.library -- the bits a shared library has to bring itself.
 *
 * An AmigaOS shared library is loaded by Exec, not by a C startup, so there is
 * no crt0 to open dos.library, no _exit, and no newlib reentrancy structure.
 * Three consequences, all handled here:
 *
 *   DOSBase   the crt normally defines and opens it; src/config talks to
 *             dos.library, so the library opens it in its own init.
 *
 *   rand()    NetX Duo takes NX_RAND from <stdlib.h> by default (nx_api.h),
 *             and newlib's rand() reaches through _impure_ptr -- which nothing
 *             has initialised. Pulling it in also drags lib_a-open.o, which
 *             wants _exit and takes the whole link down. A self-contained
 *             generator is both smaller and correct here. It is only used for
 *             TCP initial sequence numbers, IP IDs and the DHCP transaction
 *             id, so an xorshift seeded from the E-Clock is ample; nothing in
 *             AmiNetXDuo uses rand() for anything that needs to be secure.
 *
 *   weak      every definition is weak so that a build which *does* have a crt
 *             (the test executables) keeps the crt's DOSBase and libc's rand.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_internal.h"

#include <dos/dosextens.h>
#include <proto/exec.h>

__attribute__((weak)) struct DosLibrary *DOSBase;

/* --------------------------------------------------------------- rand() -- */

static ULONG bsd_rand_state = 0x2545F491UL;

__attribute__((weak)) void srand(unsigned int seed)
{
    bsd_rand_state = (ULONG)seed | 1UL;
}

__attribute__((weak)) int rand(void)
{
    ULONG x = bsd_rand_state;

    /* Marsaglia xorshift32. */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    bsd_rand_state = x;

    /* rand() is specified to return 0..RAND_MAX, and RAND_MAX is 0x7FFFFFFF. */
    return (int)(x & 0x7FFFFFFFUL);
}

/* ------------------------------------------------------------- lifecycle -- */

/*
 * Called from bsd_lib_init() -- i.e. from InitResident() on the first
 * OpenLibrary(), which is a normal task context where OpenLibrary() is legal.
 */
BOOL bsd_runtime_open(VOID)
{
    if (DOSBase == NULL)
        DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);

    srand((unsigned int)(ami_millis() ^ (ULONG)(APTR)FindTask(NULL)));

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
