/*
 * The socket table, out of NETSTATUS_SOCKETS and NETSTATUS_TCPSTALL.
 *
 * Its own file so that a command can read the table through a base it opened
 * itself without linking tool_nx.c: netstat and ShowNetStatus get it through
 * tool_snapshot(), CreateAmiNetXDuoStatusReport through the resident library
 * it is limited to.  Both print the same rows.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"

/* Static: a Shell command starts with a 4 KB stack. */
static union
{
    struct { NetStatusHeader hdr; NetStatusSocket   e[TOOL_MAX_SOCK]; } sock;
    struct { NetStatusHeader hdr; NetStatusTcpStall e[TOOL_MAX_SOCK]; } stall;
} ts_answer;

UWORD tool_sockets(struct Library *base, ToolSockInfo *sock, BOOL *truncated)
{
    UWORD count = 0;
    LONG  n;
    LONG  i;

    *truncated = FALSE;

    n = tool_netstatus_query(base, NETSTATUS_SOCKETS, &ts_answer,
                             sizeof(ts_answer.sock), sizeof(NetStatusSocket));
    if (n > 0)
    {
        if (ts_answer.sock.hdr.nsh_Available > ts_answer.sock.hdr.nsh_Count)
            *truncated = TRUE;

        for (i = 0; i < n && i < (LONG)TOOL_MAX_SOCK; i++)
        {
            const NetStatusSocket *src = &ts_answer.sock.e[i];
            ToolSockInfo          *si  = &sock[i];

            si->is_tcp = (src->nso_Flags & NETSTATUS_SOCK_TCP) ? TRUE : FALSE;
            si->local_port      = src->nso_LocalPort;
            si->peer_port       = src->nso_PeerPort;
            si->peer_address    = src->nso_PeerAddress;
            si->state           = (UINT)src->nso_State;
            si->queued          = src->nso_Queued;
            si->stalled_ms      = 0;
            si->retransmits     = 0;
            si->rto_ms          = 0;
            si->user_timeout_ms = 0;
        }

        count = (UWORD)i;
    }

    /*
     * Second call, because the stall numbers are their own table, into the
     * same buffer, so after the copy above.  A library that predates the
     * selector answers an error and every row keeps its zeros.
     *
     * Joined on the tuple rather than by index: the two walks are the same
     * order at the same instant, but these are two calls and a connection
     * can arrive or leave between them.
     */
    n = tool_netstatus_query(base, NETSTATUS_TCPSTALL, &ts_answer,
                             sizeof(ts_answer.stall), sizeof(NetStatusTcpStall));
    for (i = 0; i < n && i < (LONG)TOOL_MAX_SOCK; i++)
    {
        const NetStatusTcpStall *src = &ts_answer.stall.e[i];
        UWORD                    sock_i;

        for (sock_i = 0; sock_i < count; sock_i++)
        {
            ToolSockInfo *si = &sock[sock_i];

            if (si->is_tcp &&
                si->local_port == src->nst_LocalPort &&
                si->peer_port == src->nst_PeerPort &&
                si->peer_address == src->nst_PeerAddress)
            {
                si->stalled_ms      = src->nst_Stalled;
                si->retransmits     = src->nst_Retransmits;
                si->rto_ms          = src->nst_Rto;
                si->user_timeout_ms = src->nst_UserTimeout;
                break;
            }
        }
    }

    return count;
}

const char *tool_tcp_state_name(UINT state)
{
    switch (state)
    {
        case NETSTATUS_TCP_CLOSED:          return "CLOSED";
        case NETSTATUS_TCP_LISTEN:          return "LISTEN";
        case NETSTATUS_TCP_SYN_SENT:        return "SYN_SENT";
        case NETSTATUS_TCP_SYN_RECEIVED:    return "SYN_RCVD";
        case NETSTATUS_TCP_ESTABLISHED:     return "ESTABLISHED";
        case NETSTATUS_TCP_CLOSE_WAIT:      return "CLOSE_WAIT";
        case NETSTATUS_TCP_FIN_WAIT_1:      return "FIN_WAIT_1";
        case NETSTATUS_TCP_FIN_WAIT_2:      return "FIN_WAIT_2";
        case NETSTATUS_TCP_CLOSING:         return "CLOSING";
        case NETSTATUS_TCP_TIMED_WAIT:      return "TIME_WAIT";
        case NETSTATUS_TCP_LAST_ACK:        return "LAST_ACK";
        default:                            return "UNKNOWN";
    }
}
