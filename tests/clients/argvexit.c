/*
 * ArgvExit -- does a ported client give its 256 KB stack back when it exits?
 *
 * clients/compat/amiga_argv.c runs every ported client on a 256 KB stack it
 * AllocMem()s, because the Shell gives a command 4 KB and curl and dropbear
 * want more. The FreeMem() sits after the swapped-stack call returns -- and a
 * client that ends with exit() rather than by returning from main() never
 * comes back to it. Dropbear always exits that way. AmigaOS does not reclaim
 * AllocMem() on process exit, so that was 256 KB per invocation until reboot,
 * on a machine whose supported floor is 1 MB.
 *
 * This is the smallest program that takes that path: it links the same
 * amiga_argv.c with the same three --wrap flags a client build uses, prints
 * what AvailMem() says, and leaves through exit(). Run it several times and
 * the number either holds or falls by 256 KB a go.
 *
 * It is not a client and does not open bsdsocket.library -- the thing under
 * test is the startup and exit shim, and nothing else.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdlib.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    Printf((CONST_STRPTR)"ArgvExit: free %lu\n", (LONG)AvailMem(MEMF_ANY));

    /* The whole point: out through exit(), not a return. */
    exit(0);

    return 0;   /* not reached */
}
