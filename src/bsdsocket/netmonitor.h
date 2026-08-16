/*
 * bsdsocket.library, monitoring hook dispatch, for the call sites.
 *
 * netmonitor.c holds the hook lists. The calls that consult them live
 * elsewhere (socket.c for connect() and bind()), so the dispatch is declared
 * here rather than kept static.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_NETMONITOR_H
#define AMINETXDUO_BSDSOCKET_NETMONITOR_H

#include "bsdsocket_internal.h"

/*
 * Run every hook of `type` over `message`. Stops at the first non-zero result
 * and returns it. Returns 0 when there are no hooks, which means "continue"
 * for both result vocabularies:
 *
 * for the call-site types (MHT_Connect, MHT_Bind, MHT_Send) a non-zero result
 * is an errno the call must fail with. For the in-stack types it is an MA_*
 * action. See the header of netmonitor.c for why one function serves both.
 */
LONG bsd_netmon_dispatch(LONG type, APTR message);

/*
 * TRUE when a hook is installed for `type`. A call site can then skip building
 * a monitor message, at the cost of one test.
 */
BOOL bsd_netmon_have(LONG type);

/* The name the caller chose to be known by, or NULL, see netmonitor.c. */
STRPTR bsd_netmon_caller(struct AmiSocketBase *base);

/*
 * TRUE while any hook is installed. bsd_lib_expunge() then declines. This is
 * the autodoc's "the library will stay in memory indefinitely", not a leak.
 */
BOOL bsd_netmon_busy(VOID);

#endif /* AMINETXDUO_BSDSOCKET_NETMONITOR_H */
