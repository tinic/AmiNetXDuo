/*
 * bsdsocket.library -- weak netstack_* stubs.
 *
 * include/aminetxduo/netstack.h is owned by the stack workstream, not by this
 * one. These definitions are weak, so the moment the real implementation is
 * linked in it wins; until then bsdsocket.library links and can be inspected
 * on its own. Every stub fails in the way the caller already handles: the
 * stack is simply not up.
 *
 * Delete this file (and the AMINETXDUO_BSDSOCKET_WEAK_NETSTACK option in
 * CMakeLists.txt) once src/netstack exists.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_internal.h"

#define BSD_WEAK    __attribute__((weak))

BSD_WEAK LONG netstack_startup(VOID)
{
    AMI_WARN("bsdsocket: netstack_startup() stub -- no stack linked in");
    return AMI_NET_ERR_STATE;
}

BSD_WEAK VOID netstack_shutdown(VOID)
{
}

BSD_WEAK AmiNetStack *netstack_get(VOID)
{
    return NULL;
}

BSD_WEAK NX_IP *netstack_ip(VOID)
{
    return NULL;
}

BSD_WEAK NX_PACKET_POOL *netstack_pool(VOID)
{
    return NULL;
}

BSD_WEAK const AmiConfig *netstack_config(VOID)
{
    return NULL;
}

BSD_WEAK UWORD netstack_interface_count(VOID)
{
    return 0;
}

BSD_WEAK LONG netstack_interface_up(UWORD index)
{
    (VOID)index;
    return AMI_NET_ERR_STATE;
}

BSD_WEAK LONG netstack_interface_down(UWORD index)
{
    (VOID)index;
    return AMI_NET_ERR_STATE;
}

BSD_WEAK BOOL netstack_interface_is_up(UWORD index)
{
    (VOID)index;
    return FALSE;
}

BSD_WEAK LONG netstack_resolve(const char *name, ULONG *addr_out,
                               ULONG timeout_ticks)
{
    (VOID)name;
    (VOID)addr_out;
    (VOID)timeout_ticks;
    return AMI_NET_ERR_STATE;
}

BSD_WEAK LONG netstack_resolve_reverse(ULONG addr, char *name_out,
                                       ULONG name_len, ULONG timeout_ticks)
{
    (VOID)addr;
    (VOID)name_out;
    (VOID)name_len;
    (VOID)timeout_ticks;
    return AMI_NET_ERR_STATE;
}
