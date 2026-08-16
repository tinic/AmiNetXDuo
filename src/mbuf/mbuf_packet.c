/*
 * AmiNetXDuo, the NX_PACKET <-> mbuf boundary.
 *
 * Both directions copy. See the ownership decision at the top of
 * include/aminetxduo/mbuf.h for why an mbuf must not reference the payload of
 * an NX_PACKET. dtom() is ABI and requires the data to live inside the mbuf,
 * and NX_PACKETs are a pinned pool resource that must not escape to
 * applications.
 *
 * tx_api.h / nx_api.h must come before any exec header. exec/types.h turns
 * VOID into a macro, which breaks the `typedef void VOID` in tx_port.h. Same
 * rule as src/sana2/sana2_internal.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "nx_api.h"

#include "mbuf_internal.h"

struct mbuf *ami_mbuf_from_packet(NX_PACKET *packet, BOOL want_pkthdr)
{
    struct mbuf  *head = NULL;
    struct mbuf  *tail = NULL;
    NX_PACKET    *p;
    ULONG         total = 0;

    if (packet == NULL)
        return NULL;

    /*
     * Each NX_PACKET in the chain contributes [prepend_ptr, append_ptr).
     * nx_packet_length on the first packet is the whole chain, but the
     * per-packet windows are what has to be copied.
     */
    for (p = packet; p != NULL; p = p->nx_packet_next)
    {
        ULONG        len;
        struct mbuf *piece;

        if (p->nx_packet_append_ptr <= p->nx_packet_prepend_ptr)
            continue;

        len = (ULONG)(p->nx_packet_append_ptr - p->nx_packet_prepend_ptr);

        piece = ami_mbuf_build(p->nx_packet_prepend_ptr, len,
                               (head == NULL) ? want_pkthdr : FALSE);
        if (piece == NULL)
            goto fail;

        if (head == NULL)
        {
            head = piece;
        }
        else
        {
            tail->m_next = piece;
        }

        while (piece->m_next != NULL)
            piece = piece->m_next;
        tail = piece;

        total += len;
    }

    if (head == NULL)
    {
        head = ami_mbuf_build(NULL, 0, want_pkthdr);
        if (head == NULL)
            return NULL;
    }

    if (want_pkthdr && (head->m_flags & M_PKTHDR) != 0)
        head->m_pkthdr.len = (LONG)total;

    return head;

fail:
    ami_mbuf_freem(head);
    return NULL;
}

UINT ami_mbuf_to_packet(struct mbuf *m, NX_PACKET_POOL *pool, ULONG wait_option,
                        NX_PACKET **out)
{
    NX_PACKET *packet = NX_NULL;
    UINT       status;

    if (m == NULL || pool == NX_NULL || out == NX_NULL)
        return NX_PTR_ERROR;

    status = nx_packet_allocate(pool, &packet, NX_PHYSICAL_HEADER, wait_option);
    if (status != NX_SUCCESS)
        return status;

    for (; m != NULL; m = m->m_next)
    {
        if (m->m_len <= 0)
            continue;

        status = nx_packet_data_append(packet, m->m_data, (ULONG)m->m_len,
                                       pool, wait_option);
        if (status != NX_SUCCESS)
        {
            nx_packet_release(packet);
            return status;
        }
    }

    *out = packet;

    return NX_SUCCESS;
}
