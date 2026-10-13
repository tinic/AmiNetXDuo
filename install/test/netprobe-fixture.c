/*
 * Deterministic InstallNetProbe substitute for Installer scenario tests.
 * It exercises the installer's device-tree outcomes without pretending
 * Amiberry exposes an Emu68 device tree.  The real helper is still what ships;
 * this binary exists only on the throw-away Workbench test drive.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h>
#include "../../src/tools/netprobe_memory.h"

#ifndef FIXTURE_GENET
#define FIXTURE_GENET 0
#endif
#ifndef FIXTURE_WIFI
#define FIXTURE_WIFI 0
#endif
/* GENET is in the tree, but at an address anxgenet.device cannot reach
   (Emu68 1.0.x): GENET answers yes and GENETOK no. */
#ifndef FIXTURE_GENET_OLD
#define FIXTURE_GENET_OLD 0
#endif
/* Every call fails, the way a probe that cannot work does (run-workbench -P). */
#ifndef FIXTURE_FAIL
#define FIXTURE_FAIL 0
#endif
#ifndef FIXTURE_CARD
#define FIXTURE_CARD ""
#endif
#ifndef FIXTURE_DEVICE
#define FIXTURE_DEVICE ""
#endif

enum
{
    ARG_EMU68 = 0,
    ARG_GENET,
    ARG_GENETOK,
    ARG_WIFIPI,
    ARG_CARD,
    ARG_MEMORY,
    ARG_DEVICE,
    ARG_COUNT
};

int main(int argc, char **argv)
{
    static const char template[] = "EMU68/S,GENET/S,GENETOK/S,WIFIPI/S,CARD/K,MEMORY/N/K,DEVICE/K";
    LONG           args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    LONG           result = RETURN_WARN;

    (void)argv;
    if (argc == 0 || FIXTURE_FAIL)
        return RETURN_FAIL;

    /* Match the shipping helper's AmigaDOS parser.  Using libc argv here
       made this fixture toolchain-dependent and allowed the detector test to
       exercise the wrong card while still completing an installation. */
    rda = ReadArgs((CONST_STRPTR)template, args, NULL);
    if (rda == NULL)
        return RETURN_ERROR;
    if ((args[ARG_EMU68] != 0) + (args[ARG_GENET] != 0) +
        (args[ARG_GENETOK] != 0) + (args[ARG_WIFIPI] != 0) +
        (args[ARG_CARD] != 0) + (args[ARG_MEMORY] != 0) +
        (args[ARG_DEVICE] != 0) != 1)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (args[ARG_DEVICE] != 0)
        result = ((FIXTURE_DEVICE[0] != '\0' &&
                   strcmp((const char *)args[ARG_DEVICE], FIXTURE_DEVICE) == 0) ||
                  (strcmp(FIXTURE_CARD, "uae") == 0 &&
                   strcmp((const char *)args[ARG_DEVICE], "uaenet.device") == 0))
                 ? RETURN_OK : RETURN_WARN;
    else if (args[ARG_MEMORY] != 0)
    {
        LONG wanted = *(const LONG *)args[ARG_MEMORY];
        ULONG total = netprobe_memory_total();
        if (total <= 0xffff0000UL)
            total = (total + 65535UL) & ~65535UL;
        result = wanted <= 0 ? RETURN_ERROR :
                 total >= (ULONG)wanted ? RETURN_OK : RETURN_WARN;
    }
    else if (args[ARG_CARD] != 0)
        result = strcmp((const char *)args[ARG_CARD], FIXTURE_CARD) == 0 ||
                 (strcmp((const char *)args[ARG_CARD], "pcmcia") == 0 &&
                  FIXTURE_CARD[0] != '\0') ? RETURN_OK : RETURN_WARN;
    else if (args[ARG_EMU68] != 0)
        result = (FIXTURE_GENET || FIXTURE_WIFI) ? RETURN_OK : RETURN_WARN;
    else if (args[ARG_GENET] != 0)
        result = FIXTURE_GENET ? RETURN_OK : RETURN_WARN;
    else if (args[ARG_GENETOK] != 0)
        result = (FIXTURE_GENET && !FIXTURE_GENET_OLD) ? RETURN_OK
                                                       : RETURN_WARN;
    else if (args[ARG_WIFIPI] != 0)
        result = FIXTURE_WIFI ? RETURN_OK : RETURN_WARN;

    FreeArgs(rda);
    return result;
}
