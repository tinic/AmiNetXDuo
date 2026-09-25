/*
 * AmiNetXDuo, ownership of the public AMITCP message port.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include <exec/ports.h>
#include <proto/exec.h>

#ifdef AMINETXDUO_AREXX

VOID ami_ns_port_create(VOID)
{
    ami_netstack_rexx_start();
}

VOID ami_ns_port_delete(VOID)
{
    ami_netstack_rexx_stop();
}

VOID ami_ns_port_suspend(VOID)
{
    ami_netstack_rexx_suspend();
}

VOID ami_ns_port_resume(VOID)
{
    ami_netstack_rexx_resume();
}

#else /* !AMINETXDUO_AREXX */

static char            ami_ns_port_name[] = "AMITCP";
static struct MsgPort *ami_ns_bare_port;
static ULONG            ami_ns_bare_port_suspensions;

VOID ami_ns_port_create(VOID)
{
    struct MsgPort *port;

    if (ami_ns_bare_port != NULL)
        return;

    port = CreateMsgPort();
    if (port == NULL)
    {
        AMI_WARN("AMITCP: no public port. WaitForPort will not return");
        return;
    }

    port->mp_Node.ln_Name = ami_ns_port_name;
    port->mp_Node.ln_Pri  = 0;

    Forbid();
    if (FindPort((CONST_STRPTR)ami_ns_port_name) != NULL)
    {
        Permit();
        DeleteMsgPort(port);
        AMI_WARN("AMITCP: a port of that name already exists. "
                 "Ours is not added");
        return;
    }
    AddPort(port);
    ami_ns_bare_port = port;
    ami_ns_bare_port_suspensions = 0;
    Permit();
}

/* The compat layer calls these only for X-Surf-family OpenDevice calls.
 * This bare-port implementation covers minimal and micro builds, which do
 * not include the ARexx host. OpenDevice may wait, so only the list operations
 * run under Forbid. The depth keeps concurrent opens from republishing the
 * port before the last one completes. */
VOID ami_ns_port_suspend(VOID)
{
    Forbid();
    if (ami_ns_bare_port != NULL && ami_ns_bare_port_suspensions++ == 0)
        RemPort(ami_ns_bare_port);
    Permit();
}

VOID ami_ns_port_resume(VOID)
{
    Forbid();
    if (ami_ns_bare_port != NULL && ami_ns_bare_port_suspensions != 0 &&
        --ami_ns_bare_port_suspensions == 0)
        AddPort(ami_ns_bare_port);
    Permit();
}

VOID ami_ns_port_delete(VOID)
{
    struct Message *msg;

    if (ami_ns_bare_port == NULL)
        return;

    if (ami_ns_bare_port_suspensions == 0)
        RemPort(ami_ns_bare_port);

    while ((msg = GetMsg(ami_ns_bare_port)) != NULL)
        ReplyMsg(msg);

    DeleteMsgPort(ami_ns_bare_port);
    ami_ns_bare_port = NULL;
    ami_ns_bare_port_suspensions = 0;
}

#endif /* AMINETXDUO_AREXX */
