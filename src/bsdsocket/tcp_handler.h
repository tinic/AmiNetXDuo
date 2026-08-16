/*
 * bsdsocket.library, the TCP: AmigaDOS handler.
 *
 * See tcp_handler.c. Kept in its own header rather than in
 * bsdsocket_internal.h so the handler stays a self-contained addition.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_TCP_HANDLER_H
#define AMINETXDUO_BSDSOCKET_TCP_HANDLER_H

#include <exec/types.h>

struct AmiSocketBase;

/*
 * Start the handler process and publish "TCP:" in the DOS device list. Called
 * once, from the first bsd_lib_open(). Later calls are no-ops. Failure is not
 * fatal to the library. The machine then has no TCP: device.
 */
VOID bsd_tcp_handler_start(struct AmiSocketBase *master);

/* TRUE while the handler process exists. See bsd_lib_expunge(). */
BOOL bsd_tcp_handler_alive(VOID);

#endif /* AMINETXDUO_BSDSOCKET_TCP_HANDLER_H */
