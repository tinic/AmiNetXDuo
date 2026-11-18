/*
 * SyncReboot, write every volume's buffers out and restart the machine.
 *
 *     SyncReboot QUIET/S
 *
 * C:Reboot restarts at once, and a write that is still in a handler's
 * buffers is lost.  This is the restart to use after writing to a disk.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "tool_reboot.h"

const char *const tool_name = "SyncReboot";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("SyncReboot");

#define TEMPLATE    "QUIET/S"

enum
{
    ARG_QUIET = 0,
    ARG_COUNT
};

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    args[ARG_QUIET] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[QUIET]",
                   "Write every volume's buffers to disk and restart.");
        return RETURN_ERROR;
    }

    if (args[ARG_QUIET] == 0)
        tool_say("Flushing every volume, then restarting.\n");

    FreeArgs(rda);

    tool_reboot();

    /* Only where ColdReboot() is patched to return. */
    tool_error("the machine did not restart");
    return RETURN_FAIL;
}
