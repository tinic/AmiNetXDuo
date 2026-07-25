/*
 * AmiNetXDuo tools -- talking to the live NetX Duo instance.
 *
 * Only ShowNetStatus, netstat and ping need this; the rest of the tools stay
 * clear of ThreadX and NetX Duo entirely.
 *
 * The rule that shapes everything here: NetX Duo APIs suspend "the calling
 * thread", so an Exec Process must be ADOPTED as a TX_THREAD before it calls
 * any of them (docs/RESEARCH.md 6.3). While adopted the Process holds the
 * ThreadX baton and must not block on anything else -- no dos.library, no
 * IORequest. So the tools adopt, take a snapshot into plain memory, orphan,
 * and only then print.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOLS_NX_H
#define AMINETXDUO_TOOLS_NX_H

#include "tools.h"
#include "aminetxduo/sana2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Adopt/orphan the calling Process. 0 on success, negative on failure. */
LONG tool_stack_enter(VOID);
VOID tool_stack_leave(VOID);

/* ----------------------------------------------------------- snapshots -- */

#define TOOL_MAX_IF     NX_MAX_PHYSICAL_INTERFACES
#define TOOL_MAX_SOCK   32

typedef struct ToolIfInfo
{
    UWORD           nx_index;
    BOOL            attached;        /* NX_INTERFACE is valid                */
    BOOL            link_up;
    ULONG           address;         /* host byte order                      */
    ULONG           netmask;
    ULONG           mtu;
    UBYTE           mac[AMI_ETH_ADDR_SIZE];
    const char     *nx_name;
    /* From the SANA-II shim, when the interface has one attached. */
    BOOL            have_sana2;
    BOOL            sana2_online;
    ULONG           bps;
    AmiSana2Stats   stats;
} ToolIfInfo;

typedef struct ToolSockInfo
{
    BOOL    is_tcp;
    UWORD   local_port;
    UWORD   peer_port;
    ULONG   peer_address;
    UINT    state;                   /* TCP only */
    ULONG   queued;                  /* UDP receive queue depth */
} ToolSockInfo;

typedef struct ToolSnapshot
{
    ToolIfInfo      iface[TOOL_MAX_IF];
    UWORD           iface_count;
    ToolSockInfo    sock[TOOL_MAX_SOCK];
    UWORD           sock_count;
    BOOL            sock_truncated;
    ULONG           gateway;
    BOOL            have_gateway;
} ToolSnapshot;

/*
 * One adopt/snapshot/orphan cycle. `want_sockets` is honoured only when the
 * caller needs the connection table, because walking it is the expensive
 * part. Returns 0, or a negative code after printing a message.
 */
LONG tool_snapshot(NX_IP *ip, ToolSnapshot *out, BOOL want_sockets);

/* TCP state number -> the name netstat prints. */
const char *tool_tcp_state_name(UINT state);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TOOLS_NX_H */
