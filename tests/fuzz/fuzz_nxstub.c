/*
 * AmiNetXDuo -- the ThreadX surface fuzz_dns and fuzz_mdns stand on.
 *
 * The drivers run NetX Duo's DNS and mDNS parsers on one host thread, so the
 * mutex is uncontended by construction and granting it always is correct here
 * rather than a shortcut. Nothing suspends either: every receive is
 * NX_NO_WAIT and the datagram is already queued before it is asked for.
 *
 * The same stubs are spelled out in tests/netstack/host/test_bcast_loopback_host.c;
 * they live in their own file here because two drivers need them.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "nx_api.h"



UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    NX_PARAMETER_NOT_USED(mutex_ptr);
    NX_PARAMETER_NOT_USED(wait_option);
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    NX_PARAMETER_NOT_USED(mutex_ptr);
    return TX_SUCCESS;
}

UINT _tx_mutex_create(TX_MUTEX *mutex_ptr, CHAR *name, UINT inherit)
{
    NX_PARAMETER_NOT_USED(mutex_ptr);
    NX_PARAMETER_NOT_USED(name);
    NX_PARAMETER_NOT_USED(inherit);
    return TX_SUCCESS;
}

UINT _tx_mutex_delete(TX_MUTEX *mutex_ptr)
{
    NX_PARAMETER_NOT_USED(mutex_ptr);
    return TX_SUCCESS;
}


/* mDNS sets an event flag to wake its own thread; nothing here waits on one.
   fuzz_dns supplies its own, so that driver defines FUZZ_HAS_EVENT_FLAGS. */
#ifndef FUZZ_HAS_EVENT_FLAGS
UINT _tx_event_flags_set(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG flags_to_set,
                         UINT set_option)
{
    NX_PARAMETER_NOT_USED(group_ptr);
    NX_PARAMETER_NOT_USED(flags_to_set);
    NX_PARAMETER_NOT_USED(set_option);
    return TX_SUCCESS;
}
#endif /* FUZZ_HAS_EVENT_FLAGS */
