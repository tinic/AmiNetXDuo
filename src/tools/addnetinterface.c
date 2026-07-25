/*
 * AddNetInterface -- bring up the interface described by
 * DEVS:NetInterfaces/<name>.
 *
 * This is the command S:User-Startup invokes, so it is the one that actually
 * starts the stack:
 *
 *     C:AddNetInterface DEVS:NetInterfaces/eth0 QUIET
 *
 * Roadshow accepts either a bare interface name or the full path to the
 * interface file; so do we -- the name is what matters and the directory is
 * fixed.
 *
 * It deliberately does NOT call netstack_shutdown(). netstack_startup() is
 * reference counted, and the reference this command takes is what keeps the
 * stack alive after it exits -- exactly Roadshow's model, where the interface
 * stays online until Offline or a reboot.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

const char *const tool_name = "AddNetInterface";

static const char version_tag[] __attribute__((used)) =
    "$VER: AddNetInterface 1.0 (24.7.2026)";

#define TEMPLATE    "NAME/A,QUIET/S"

enum
{
    ARG_NAME = 0,
    ARG_QUIET,
    ARG_COUNT
};

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    AmiIfConfig     ifc;
    const char     *name;
    BOOL            quiet;
    LONG            index;
    LONG            err;
    LONG            rc = RETURN_OK;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_NAME]   = 0;
    args[ARG_QUIET]  = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    name  = tool_basename((const char *)args[ARG_NAME]);
    quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;

    /*
     * Parse the interface file first: a typo in User-Startup should produce a
     * clear message rather than a stack that comes up with nothing on it.
     */
    err = ami_config_load_interface(name, &ifc);
    if (err != AMI_CFG_OK)
    {
        if (err == AMI_CFG_ERR_IO)
            tool_error("cannot read DEVS:NetInterfaces/%s", name);
        else if (err == AMI_CFG_ERR_SYNTAX)
            tool_error("DEVS:NetInterfaces/%s is not a valid interface file",
                       name);
        else
            tool_error("out of memory reading DEVS:NetInterfaces/%s", name);

        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!quiet)
    {
        tool_printf("%s: %s unit %ld\n", (LONG)name, (LONG)ifc.device,
                    (LONG)ifc.unit);
    }

    /*
     * netstack_startup() is idempotent and reference counted. It reads the
     * whole Roadshow configuration, starts ThreadX and NetX Duo, attaches
     * every configured interface and runs DHCP where asked.
     */
    err = netstack_startup();
    if (err != AMI_NET_OK)
    {
        tool_error("cannot start the network stack: %s", (LONG)tool_net_error(err));
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    index = tool_find_interface(name);
    if (index < 0)
    {
        /*
         * The file parses but the running stack does not know the name --
         * the stack was already up when this interface file was added.
         */
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!netstack_interface_is_up((UWORD)index))
    {
        err = netstack_interface_up((UWORD)index);
        if (err != AMI_NET_OK)
        {
            tool_error("cannot bring %s online: %s", (LONG)name,
                       (LONG)tool_net_error(err));
            FreeArgs(rda);
            return RETURN_FAIL;
        }
    }

    if (!quiet)
    {
        const AmiConfig *cfg = netstack_config();
        char             addr[16];
        char             mask[16];

        if (cfg != NULL && (UWORD)index < cfg->interface_count)
        {
            const AmiIfConfig *live = &cfg->interfaces[index];

            ami_config_format_ip(live->address, addr, sizeof(addr));
            ami_config_format_ip(live->netmask, mask, sizeof(mask));

            tool_printf("%s: %s netmask %s (%s)\n",
                        (LONG)name, (LONG)addr, (LONG)mask,
                        (LONG)(live->iptype == AMI_IPTYPE_DHCP    ? "DHCP" :
                               live->iptype == AMI_IPTYPE_LINKLOCAL ? "link-local"
                                                                    : "static"));
        }
    }

    if (tool_break())
    {
        tool_fault(ERROR_BREAK);
        rc = RETURN_WARN;
    }

    FreeArgs(rda);
    return (int)rc;
}
