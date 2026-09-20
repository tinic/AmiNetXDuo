/*
 * Deterministic InstallNetProbe substitute for Installer scenario tests.
 * It exercises the installer's four device-tree outcomes without pretending
 * Amiberry exposes an Emu68 device tree.  The real helper is still what ships;
 * this binary exists only on the throw-away Workbench test drive.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/dos.h>

#ifndef FIXTURE_GENET
#define FIXTURE_GENET 0
#endif
#ifndef FIXTURE_WIFI
#define FIXTURE_WIFI 0
#endif

enum
{
    ARG_EMU68 = 0,
    ARG_GENET,
    ARG_WIFIPI,
    ARG_CARD,
    ARG_COUNT
};

int main(int argc, char **argv)
{
    static const char template[] = "EMU68/S,GENET/S,WIFIPI/S,CARD/K";
    LONG           args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    LONG           result = RETURN_WARN;

    (void)argv;
    if (argc == 0)
        return RETURN_FAIL;

    /* Match the shipping helper's AmigaDOS parser.  Using libc argv here
       made this fixture toolchain-dependent and allowed the detector test to
       exercise the wrong card while still completing an installation. */
    rda = ReadArgs((CONST_STRPTR)template, args, NULL);
    if (rda == NULL)
        return RETURN_ERROR;
    if ((args[ARG_EMU68] != 0) + (args[ARG_GENET] != 0) +
        (args[ARG_WIFIPI] != 0) + (args[ARG_CARD] != 0) != 1)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (args[ARG_EMU68] != 0)
        result = (FIXTURE_GENET || FIXTURE_WIFI) ? RETURN_OK : RETURN_WARN;
    else if (args[ARG_GENET] != 0)
        result = FIXTURE_GENET ? RETURN_OK : RETURN_WARN;
    else if (args[ARG_WIFIPI] != 0)
        result = FIXTURE_WIFI ? RETURN_OK : RETURN_WARN;

    FreeArgs(rda);
    return result;
}
