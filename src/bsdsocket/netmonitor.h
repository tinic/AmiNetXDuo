/*
 * bsdsocket.library -- what the monitoring hooks publish to their call sites.
 *
 * netmonitor.c owns the hook lists; the calls that have to consult them live
 * elsewhere (socket.c for connect() and bind()), so the dispatch is here
 * rather than static.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_NETMONITOR_H
#define AMINETXDUO_BSDSOCKET_NETMONITOR_H

#include "bsdsocket_internal.h"

/*
 * Run every hook of `type` over `message`, stopping at the first that does
 * not answer 0, and return what it said. 0 when there are no hooks, which is
 * "continue" in both of the API's two vocabularies.
 *
 * For the call-site types (MHT_Connect, MHT_Bind, MHT_Send) a non-zero result
 * is an ERRNO the call must fail with. For the in-stack types it is an MA_*
 * action. See the header of netmonitor.c for why one function serves both.
 */
LONG bsd_netmon_dispatch(LONG type, APTR message);

/*
 * Whether anything is listening for `type`. The point is to keep the cost of
 * an uninstalled hook down to one test at the call sites, so that building
 * a monitor message is only paid for when somebody wants it.
 */
BOOL bsd_netmon_have(LONG type);

/* The name the caller chose to be known by, or NULL -- see netmonitor.c. */
STRPTR bsd_netmon_caller(struct AmiSocketBase *base);

/*
 * TRUE while any hook is installed. bsd_lib_expunge() declines then, which is
 * the autodoc's "the library will stay in memory indefinitely" -- a
 * description of the behaviour rather than a warning about a leak.
 */
BOOL bsd_netmon_busy(VOID);

#endif /* AMINETXDUO_BSDSOCKET_NETMONITOR_H */
