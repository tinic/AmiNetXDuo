/*
 * bsdsocket.library -- putting the calling task into ThreadX context.
 *
 * WHY
 *
 * NetX Duo checks who is calling. Roughly forty of its entry points are
 * wrapped in NX_THREADS_ONLY_CALLER_CHECKING, which returns NX_CALLER_ERROR
 * unless tx_thread_identify() is non-NULL -- that is bind, listen, unlisten,
 * accept, relisten, unaccept, the client bind/unbind/connect, disconnect,
 * send, receive, bytes_available, peer_info_get, port_get and both
 * socket_delete flavours. An Exec Task that ThreadX has never adopted fails
 * every one of them, and bsd_status_map[] turns NX_CALLER_ERROR into EINVAL,
 * so the failure surfaces as "listen(): Invalid argument" rather than as
 * anything that points at the real cause.
 *
 * Only nx_tcp_socket_create/nx_udp_socket_create (INIT_AND_THREADS) and the
 * nx_packet_* helpers (no check at all) tolerate a plain Task -- which is
 * exactly why socket() used to be the one thing that worked.
 *
 * GRANULARITY: PER CALL
 *
 * The port's adoption model (port/threadx-amiga/src/tx_amiga_adopt.c) makes
 * an adopted Task the holder of the ThreadX baton: while it is adopted, no
 * other ThreadX thread runs, including the NetX Duo IP thread and the
 * periodic timer. Adopting once per OpenLibrary() and orphaning at
 * CloseLibrary() would therefore park the baton inside application code for
 * the lifetime of the base -- one Wait() on an Intuition port and the entire
 * stack stops. Per-call brackets are the model the port documents, the model
 * netstack_dns.c already uses, and the only one that is correct here; the
 * fast path in tx_amiga_adopt_thread() takes a free baton without a scheduler
 * round trip, so the cost is an AllocSignal(), a _tx_thread_create() and
 * their inverses, with no Exec context switch.
 *
 * The one call this shape does not fit is WaitSelect(), which blocks in Exec
 * Wait() for as long as the caller asked for. It brackets each poll pass and
 * drops out of ThreadX context before parking -- see select.c.
 *
 * NESTING
 *
 * Vectors call other vectors' internals (CloseSocket -> bsd_socket_release,
 * accept -> relisten), and a base's task is inside at most one vector at a
 * time, so a plain depth counter in the base is enough. Depth > 0 means the
 * task already holds the baton.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>

/*
 * Adopted application tasks run below the IP thread (1) and the SANA-II
 * readers (2), so a caller inside the stack never starves the stack. Same
 * value as AMI_CALLER_PRIORITY in src/netstack/netstack_internal.h.
 */
#define BSD_CALLER_PRIORITY     16

static char bsd_caller_name[] = "bsdsocket caller";

LONG bsd_nx_enter(struct AmiSocketBase *base)
{
    UINT status;

    if (base == NULL)
        return -1;

    if (base->sb_NxNest > 0)
    {
        base->sb_NxNest++;
        return 0;
    }

    if (tx_amiga_kernel_running() != TX_TRUE)
        return -1;

    if (tx_thread_identify() != TX_NULL)
    {
        /*
         * Already a ThreadX thread -- either a task adopted further up the
         * call chain (netstack_startup() brackets itself) or, in principle, a
         * thread ThreadX created. Borrow the context; do not orphan it.
         */
        base->sb_NxAdopted = FALSE;
    }
    else
    {
        status = tx_amiga_adopt_thread(&base->sb_NxThread, bsd_caller_name,
                                       BSD_CALLER_PRIORITY);
        if (status != TX_SUCCESS)
        {
            AMI_ERROR("bsdsocket: cannot adopt calling task (%ld)",
                      (long)status);
            return -1;
        }

        base->sb_NxAdopted = TRUE;
    }

    base->sb_NxNest = 1;

    return 0;
}

VOID bsd_nx_leave(struct AmiSocketBase *base)
{
    if (base == NULL || base->sb_NxNest <= 0)
        return;

    if (--base->sb_NxNest > 0)
        return;

    if (base->sb_NxAdopted)
    {
        base->sb_NxAdopted = FALSE;
        (VOID)tx_amiga_orphan_thread(&base->sb_NxThread);
    }
}
