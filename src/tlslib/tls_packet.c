/*
 * tls.library, its own NX_PACKET pool.
 *
 * nx_secure is written against NetX Duo's packet API and reads NX_PACKET's
 * fields directly, so the shape is not negotiable.  Where the packets come
 * from is: tls.library used to borrow bsdsocket.library's pool through a
 * private LVO, which is exactly what tied it to our own stack.  This is a
 * standalone pool of a fixed number of fixed-size blocks, allocated with the
 * connection and freed with it, so the library needs nothing from the stack
 * underneath it but send() and recv().
 *
 * The four entry points are the four nx_secure links against.  They are named
 * with the leading underscore because that is what nx_secure calls after
 * nx_api.h's NX_DISABLE_ERROR_CHECKING mapping, and defining them here keeps
 * common/src/nx_packet_*.c -- and through them _tx_thread_current_ptr and the
 * rest of the ThreadX data segment -- out of the link.
 *
 * NOT a general NetX Duo packet pool: there is no suspension list, because
 * nothing here can suspend on one.  An exhausted pool answers NX_NO_PACKET
 * and the caller fails the record, which is the honest answer on a library
 * that owns exactly these blocks.
 *
 * No lock either.  A pool belongs to one TLSConnection, and a TLSConnection
 * is driven by one task: its NX_SECURE_TLS_SESSION is no more shareable than
 * its free list is.  TLSWaitSelect() is the only call another task may make
 * against a connection and it reads, it does not allocate.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

/* Round up to NX_PACKET_ALIGNMENT, which nx_api.h defaults to sizeof(ULONG)
   and the build raises to 8. */
#define TLS_PKT_ALIGN(n) \
    (((n) + (NX_PACKET_ALIGNMENT - 1UL)) & ~(NX_PACKET_ALIGNMENT - 1UL))

/* See tls_internal.h for why the count follows TLSA_RecordBuffer. */
ULONG tls_packet_pool_count(ULONG record_bytes)
{
    ULONG chain = (record_bytes + TLS_PACKET_PAYLOAD - 1UL) / TLS_PACKET_PAYLOAD;

    if (chain == 0)
        chain = 1;

    return chain + TLS_PACKET_SPARE;
}

ULONG tls_packet_pool_bytes(ULONG packets)
{
    return packets *
           (TLS_PKT_ALIGN(sizeof(NX_PACKET)) + TLS_PKT_ALIGN(TLS_PACKET_PAYLOAD));
}

/*
 * `memory` must be tls_packet_pool_bytes(packets) long and longword aligned,
 * which AllocVec() guarantees.  The pool control block belongs to the caller.
 */
UINT tls_packet_pool_create(NX_PACKET_POOL *pool, VOID *memory, ULONG packets)
{
    UCHAR *cursor = (UCHAR *)memory;
    ULONG  header = TLS_PKT_ALIGN(sizeof(NX_PACKET));
    ULONG  payload = TLS_PKT_ALIGN(TLS_PACKET_PAYLOAD);
    ULONG  i;

    if (pool == NX_NULL || memory == NX_NULL || packets == 0)
        return NX_PTR_ERROR;

    tls_bzero(pool, sizeof(*pool));

    pool->nx_packet_pool_name         = (CHAR *)"tls.library";
    pool->nx_packet_pool_start        = (CHAR *)memory;
    pool->nx_packet_pool_size         = tls_packet_pool_bytes(packets);
    pool->nx_packet_pool_payload_size = payload;
    pool->nx_packet_pool_total        = packets;
    pool->nx_packet_pool_available    = packets;

    for (i = 0; i < packets; i++)
    {
        NX_PACKET *packet = (NX_PACKET *)(VOID *)cursor;
        UCHAR     *data   = cursor + header;

        tls_bzero(packet, sizeof(*packet));

        packet->nx_packet_pool_owner = pool;
        packet->nx_packet_data_start = data;
        packet->nx_packet_data_end   = data + payload;

        packet->nx_packet_queue_next = pool->nx_packet_pool_available_list;
        pool->nx_packet_pool_available_list = packet;

        cursor = data + payload;
    }

    /* Last, and only on a pool that is fully built: _nx_packet_allocate()
       reads this to decide whether it was handed a pool at all. */
    pool->nx_packet_pool_id = NX_PACKET_POOL_ID;

    return NX_SUCCESS;
}

VOID tls_packet_pool_delete(NX_PACKET_POOL *pool)
{
    if (pool != NX_NULL)
        pool->nx_packet_pool_id = 0;
}

/* ------------------------------------------------------------- allocate --- */

UINT _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                         ULONG packet_type, ULONG wait_option)
{
    NX_PACKET *packet;

    (VOID)wait_option;      /* nothing here can suspend; see the file head */

    if (pool_ptr == NX_NULL || packet_ptr == NX_NULL)
        return NX_PTR_ERROR;
    if (pool_ptr->nx_packet_pool_id != NX_PACKET_POOL_ID)
        return NX_PTR_ERROR;

    /* The same bound _nx_packet_allocate() applies: a prepend offset past the
       end of the payload is a caller error, not a short packet. */
    if (pool_ptr->nx_packet_pool_payload_size < packet_type)
        return NX_OPTION_ERROR;

    packet = pool_ptr->nx_packet_pool_available_list;
    if (packet == NX_NULL)
    {
        pool_ptr->nx_packet_pool_empty_requests++;
        return NX_NO_PACKET;
    }

    pool_ptr->nx_packet_pool_available_list = packet->nx_packet_queue_next;
    pool_ptr->nx_packet_pool_available--;

    packet->nx_packet_queue_next   = NX_NULL;
    packet->nx_packet_next         = NX_NULL;
    packet->nx_packet_last         = NX_NULL;
    packet->nx_packet_length       = 0;
    packet->nx_packet_prepend_ptr  = packet->nx_packet_data_start + packet_type;
    packet->nx_packet_append_ptr   = packet->nx_packet_prepend_ptr;
    packet->nx_packet_ip_version   = 0;
    packet->nx_packet_ip_header    = NX_NULL;
    packet->nx_packet_address.nx_packet_interface_ptr = NX_NULL;
    packet->nx_packet_union_next.nx_packet_tcp_queue_next = NX_NULL;

    *packet_ptr = packet;

    return NX_SUCCESS;
}

/* -------------------------------------------------------------- release --- */

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    NX_PACKET_POOL *pool;

    if (packet_ptr == NX_NULL)
        return NX_PTR_ERROR;

    while (packet_ptr != NX_NULL)
    {
        NX_PACKET *next = packet_ptr->nx_packet_next;

        pool = packet_ptr->nx_packet_pool_owner;
        if (pool == NX_NULL || pool->nx_packet_pool_id != NX_PACKET_POOL_ID)
            return NX_PTR_ERROR;

        packet_ptr->nx_packet_next   = NX_NULL;
        packet_ptr->nx_packet_last   = NX_NULL;
        packet_ptr->nx_packet_length = 0;

        packet_ptr->nx_packet_queue_next    = pool->nx_packet_pool_available_list;
        pool->nx_packet_pool_available_list = packet_ptr;
        pool->nx_packet_pool_available++;

        packet_ptr = next;
    }

    return NX_SUCCESS;
}

/* --------------------------------------------------------------- append --- */

UINT _nx_packet_data_append(NX_PACKET *packet_ptr, VOID *data_start,
                            ULONG data_size, NX_PACKET_POOL *pool_ptr,
                            ULONG wait_option)
{
    const UCHAR *src = (const UCHAR *)data_start;
    NX_PACKET   *last;
    ULONG        remaining = data_size;

    if (packet_ptr == NX_NULL || data_start == NX_NULL || pool_ptr == NX_NULL)
        return NX_PTR_ERROR;

    last = (packet_ptr->nx_packet_last != NX_NULL)
           ? packet_ptr->nx_packet_last : packet_ptr;

    while (remaining > 0)
    {
        ULONG room = (ULONG)(last->nx_packet_data_end - last->nx_packet_append_ptr);
        ULONG chunk;

        if (room == 0)
        {
            NX_PACKET *extra = NX_NULL;
            UINT       status;

            /* A chained packet carries payload only: no prepend offset, so
               the whole block is usable and the chain stays short. */
            status = _nx_packet_allocate(pool_ptr, &extra, 0, wait_option);
            if (status != NX_SUCCESS)
                return status;

            last->nx_packet_next       = extra;
            packet_ptr->nx_packet_last = extra;
            last                       = extra;
            continue;
        }

        chunk = (remaining < room) ? remaining : room;

        tls_memcpy(last->nx_packet_append_ptr, src, chunk);

        last->nx_packet_append_ptr += chunk;
        src                        += chunk;
        remaining                  -= chunk;

        packet_ptr->nx_packet_length += chunk;
    }

    return NX_SUCCESS;
}

/* ------------------------------------------------------- extract offset --- */

UINT _nx_packet_data_extract_offset(NX_PACKET *packet_ptr, ULONG offset,
                                    VOID *buffer_start, ULONG buffer_length,
                                    ULONG *bytes_copied)
{
    const NX_PACKET *current = packet_ptr;
    UCHAR           *dst     = (UCHAR *)buffer_start;
    ULONG            copied  = 0;
    ULONG            skip    = offset;

    if (packet_ptr == NX_NULL || bytes_copied == NX_NULL)
        return NX_PTR_ERROR;

    *bytes_copied = 0;

    if (offset >= packet_ptr->nx_packet_length)
        return NX_SUCCESS;

    while (current != NX_NULL && copied < buffer_length)
    {
        ULONG have = (ULONG)(current->nx_packet_append_ptr -
                             current->nx_packet_prepend_ptr);
        ULONG take;

        if (skip >= have)
        {
            skip   -= have;
            current = current->nx_packet_next;
            continue;
        }

        take = have - skip;
        if (take > (buffer_length - copied))
            take = buffer_length - copied;

        tls_memcpy(&dst[copied], current->nx_packet_prepend_ptr + skip, take);

        copied += take;
        skip    = 0;
        current = current->nx_packet_next;
    }

    *bytes_copied = copied;

    return NX_SUCCESS;
}
