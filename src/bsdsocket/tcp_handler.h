/*
 * bsdsocket.library -- the TCP: AmigaDOS handler.
 *
 * See tcp_handler.c. Declared in its own header rather than in
 * bsdsocket_internal.h so that the handler is additive to the library and
 * touches nothing else.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_TCP_HANDLER_H
#define AMINETXDUO_BSDSOCKET_TCP_HANDLER_H

#include <exec/types.h>

struct AmiSocketBase;

/*
 * Start the handler process and publish "TCP:" in the DOS device list. Called
 * once, from the first bsd_lib_open(); every later call is a no-op. Failure is
 * not fatal to the library -- the machine simply has no TCP: device.
 */
VOID bsd_tcp_handler_start(struct AmiSocketBase *master);

/* TRUE while the handler process exists; see bsd_lib_expunge(). */
BOOL bsd_tcp_handler_alive(VOID);

#endif /* AMINETXDUO_BSDSOCKET_TCP_HANDLER_H */
