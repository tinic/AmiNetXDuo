/*
 * AmiNetXDuo tools -- weak netstack_* stubs.
 *
 * src/netstack/ is being written by another workstream; include/aminetxduo/
 * netstack.h is the contract. These definitions are weak, so the real
 * implementation wins the moment it is linked in. Until then the tools link
 * and run on their own, and every stub fails in the way the tools already
 * handle: the stack is simply not up. That is also the state a user meets
 * first, so it is worth exercising.
 *
 * Delete this file once src/netstack exists and is linked into the tools.
 *
 * The same trick, for the same reason, is used by
 * src/bsdsocket/netstack_weak.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#define TOOL_WEAK   __attribute__((weak))

TOOL_WEAK LONG netstack_startup(VOID)
{
    return AMI_NET_ERR_STATE;
}

TOOL_WEAK VOID netstack_shutdown(VOID)
{
}

TOOL_WEAK AmiNetStack *netstack_get(VOID)
{
    return NULL;
}

TOOL_WEAK NX_IP *netstack_ip(VOID)
{
    return NULL;
}

TOOL_WEAK NX_PACKET_POOL *netstack_pool(VOID)
{
    return NULL;
}

TOOL_WEAK const AmiConfig *netstack_config(VOID)
{
    return NULL;
}

TOOL_WEAK UWORD netstack_interface_count(VOID)
{
    return 0;
}

TOOL_WEAK LONG netstack_interface_up(UWORD index)
{
    (VOID)index;
    return AMI_NET_ERR_STATE;
}

TOOL_WEAK LONG netstack_interface_down(UWORD index)
{
    (VOID)index;
    return AMI_NET_ERR_STATE;
}

TOOL_WEAK BOOL netstack_interface_is_up(UWORD index)
{
    (VOID)index;
    return FALSE;
}

TOOL_WEAK LONG netstack_resolve(const char *name, ULONG *addr_out,
                                ULONG timeout_ticks)
{
    (VOID)name;
    (VOID)addr_out;
    (VOID)timeout_ticks;
    return AMI_NET_ERR_STATE;
}

TOOL_WEAK LONG netstack_resolve_reverse(ULONG addr, char *name_out,
                                        ULONG name_len, ULONG timeout_ticks)
{
    (VOID)addr;
    (VOID)name_out;
    (VOID)name_len;
    (VOID)timeout_ticks;
    return AMI_NET_ERR_STATE;
}

/*
 * ThreadX's tx_kernel_enter() calls this, and the linker pulls
 * tx_initialize_kernel_enter.c in behind tx_amiga_kernel_running(). No tool
 * ever starts the kernel -- the stack does that -- so this exists only to
 * satisfy the reference, weakly, so that the stack's own definition wins.
 */
TOOL_WEAK VOID tx_application_define(void *first_unused_memory)
{
    (VOID)first_unused_memory;
}
