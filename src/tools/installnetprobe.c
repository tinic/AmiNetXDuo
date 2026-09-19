/*
 * InstallNetProbe, read-only Emu68 network-device discovery for the Installer.
 *
 * The Installer language cannot open an Exec resource.  This deliberately
 * tiny helper exposes only the three yes/no answers it needs and lives beside
 * the Installer rather than in C:.  It reads Emu68's device tree through the
 * same code as anxgenet.device; it never opens a network device and therefore
 * cannot reset, claim or otherwise disturb hardware on a running machine.
 *
 * Exit status is RETURN_OK when the requested feature is present and
 * RETURN_WARN when it is not.  Bad arguments are RETURN_ERROR.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/dos.h>

#include "aminetxduo/version.h"
#include "../netdev/netdev_dtree.h"

const char *const tool_name = "InstallNetProbe";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("InstallNetProbe");

#define TEMPLATE "EMU68/S,GENET/S,WIFIPI/S"

enum
{
    ARG_EMU68 = 0,
    ARG_GENET,
    ARG_WIFIPI,
    ARG_COUNT
};

static BOOL supported_wifi_model(VOID)
{
    static const char *const models[] =
    {
        "raspberrypi,model-zero-2-w",
        "raspberrypi,3-model-b",
        "raspberrypi,3-model-a-plus",
        "raspberrypi,3-model-b-plus",
        "raspberrypi,4-model-b",
        "raspberrypi,4-compute-module"
    };
    UWORD i;

    for (i = 0; i < (UWORD)(sizeof(models) / sizeof(models[0])); i++)
        if (netdev_dtree_root_compatible(models[i]))
            return TRUE;
    return FALSE;
}

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT] = { 0, 0, 0 };
    struct RDArgs *rda;
    BOOL           present;
    NetdevDtInfo   info;

    (VOID)argv;

    if (argc == 0)
        return RETURN_FAIL;
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
        return RETURN_ERROR;
    if ((args[ARG_EMU68] != 0) + (args[ARG_GENET] != 0) +
        (args[ARG_WIFIPI] != 0) != 1)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (args[ARG_EMU68] != 0)
        present = netdev_dtree_present();
    else if (args[ARG_GENET] != 0)
        present = netdev_dtree_find("brcm,bcm2711-genet-v5", &info);
    else
        present = (BOOL)(supported_wifi_model() &&
                         netdev_dtree_alias_present("mmc"));

    FreeArgs(rda);
    return present ? RETURN_OK : RETURN_WARN;
}
