/*
 * fetchcore, one http: or https: GET and the redirects it is sent through,
 * with the response handed to a sink.
 *
 * The transfer half of `fetch`.  Its own file so that a second command can
 * fetch into something other than a file or standard output: the sink is
 * what differs, the request, the TLS and the framing do not.  Everything here
 * reports its own failures through tool_error() under the caller's tool_name.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_FETCHCORE_H
#define AMINETXDUO_FETCHCORE_H

#include "tools.h"

#include "fetchurl.h"

/*
 * Where the answer goes.  open runs once, on a final 2xx, before any body;
 * NULL skips it.  write gets the header block when FetchCore.headers is set,
 * then the body, in order.  Either one returning FALSE has reported why, and
 * the transfer stops with RETURN_FAIL.
 */
typedef struct FetchSink
{
    BOOL  (*open)(APTR ctx);
    BOOL  (*write)(APTR ctx, const UBYTE *data, LONG len);
    APTR    ctx;
} FetchSink;

typedef struct FetchCore
{
    FetchUrl    url;        /* the target                                */
    ULONG       timeout;    /* seconds                                   */
    LONG        family;     /* -4/-6, else TOOL_AF_UNSPEC                */
    BOOL        headers;    /* the header block goes to the sink too     */
    BOOL        quiet;      /* no TLS summary, no redirect lines         */
    BOOL        noverify;
    FetchSink   sink;

    /* Set by every return after bsdsocket.library opened. */
    LONG        status;     /* the final status, 0 for none              */
    ULONG       total;      /* body bytes the sink took                  */
    FetchHead   head;       /* the final response's header block         */
} FetchCore;

/*
 * Runs it.  RETURN_OK when an answer arrived whole, whatever its status;
 * otherwise the return code for the failure, already reported.  Not
 * reentrant: the buffers are static, 4 KB of chunk and FETCH_HEAD_MAX of head.
 */
LONG fetch_core_run(FetchCore *fc);

#endif /* AMINETXDUO_FETCHCORE_H */
