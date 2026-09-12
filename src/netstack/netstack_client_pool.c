/*
 * AmiNetXDuo, on-demand private packet pools for protocol clients.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"
#include "aminetxduo/nxstatus.h"

#include <exec/memory.h>
#include <stddef.h>

UINT ami_ns_client_pool_create(AmiNsClientPoolBlock **owner,
                               CHAR *name, ULONG payload, ULONG memory_bytes)
{
    AmiNsClientPoolBlock *block;
    ULONG                 words;
    ULONG                 allocation_bytes;
    UINT                  status;

    if (owner == NULL || payload == 0UL || memory_bytes == 0UL)
        return NX_PTR_ERROR;
    if (*owner != NULL)
        return NX_SUCCESS;

    words = (memory_bytes + sizeof(ULONG) - 1UL) / sizeof(ULONG);
    allocation_bytes = (ULONG)offsetof(AmiNsClientPoolBlock, memory) +
                       words * (ULONG)sizeof(ULONG);

    block = (AmiNsClientPoolBlock *)ami_alloc_flags(
        allocation_bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (block == NULL)
        return NX_NO_PACKET;

    status = nx_packet_pool_create(&block->pool, name, payload, block->memory,
                                   words * (ULONG)sizeof(ULONG));
    if (status != NX_SUCCESS)
    {
        ami_free(block);
        return status;
    }

    *owner = block;
    return NX_SUCCESS;
}

VOID ami_ns_client_pool_delete(AmiNsClientPoolBlock **owner)
{
    AmiNsClientPoolBlock *block;

    if (owner == NULL || *owner == NULL)
        return;

    block = *owner;
    *owner = NULL;
    AMI_NX_CLEANUP(nx_packet_pool_delete(&block->pool));
    ami_free(block);
}
