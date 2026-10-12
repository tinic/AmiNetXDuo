/*
 * WirelessManager stand-in for the installer's Wi-Fi startup test
 * (run-workbench.sh -w join).  Amiberry has no Pi Wi-Fi, so this takes
 * WirelessManager's template, appends how it was started to
 * DH0:wm-starts.txt, and then stays running until CTRL-C, as the supplicant
 * does when no network answers.  It exists only on the throw-away test
 * drive; the archive's WirelessManager is what ships.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

enum
{
    ARG_DEVICE = 0,
    ARG_UNIT,
    ARG_CONFIG,
    ARG_VERBOSE,
    ARG_COUNT
};

int main(int argc, char **argv)
{
    static const char template[] = "DEVICE/A,UNIT/K/N,CONFIG/K,VERBOSE/S";
    LONG                         args[ARG_COUNT] = { 0 };
    struct CommandLineInterface *cli = Cli();
    struct RDArgs               *rda;
    BPTR                         log;
    LONG                         unit;
    ULONG                        stack;

    (void)argv;
    if (argc == 0)
        return RETURN_FAIL;

    rda = ReadArgs((CONST_STRPTR)template, args, NULL);
    if (rda == NULL)
        return RETURN_ERROR;

    unit  = (args[ARG_UNIT] != 0) ? *(LONG *)args[ARG_UNIT] : 0;
    stack = (cli != NULL) ? (ULONG)cli->cli_DefaultStack * 4UL : 0UL;

    /* Closed before the wait: WirelessManager holds its log open, and a
       record that only appears once the program ends would be read empty. */
    log = Open((CONST_STRPTR)"DH0:wm-starts.txt", MODE_READWRITE);
    if (log != 0)
    {
        Seek(log, 0, OFFSET_END);
        FPrintf(log, (CONST_STRPTR)"start device=%s unit=%ld stack=%lu\n",
                args[ARG_DEVICE], unit, stack);
        Close(log);
    }
    FreeArgs(rda);

    Wait(SIGBREAKF_CTRL_C);
    return RETURN_OK;
}
