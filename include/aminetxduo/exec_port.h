/* Exec operations which shipping code may use without depending on private
 * ExecBase layout.  Implemented by the ThreadX AmigaOS port.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_EXEC_PORT_H
#define AMINETXDUO_EXEC_PORT_H

#include "tx_api.h"

/* Exec has no public operation that validates an arbitrary Task pointer.  The
   hosted services nevertheless need to retire registrations left by programs
   which exited without closing them.  Keep the necessary scheduler-list walk
   in the Exec port rather than teaching each service ExecBase's private
   layout.  The signal operation makes validation and Signal() one atomic
   transaction; a separate alive check followed by Signal() would race exit. */
UINT tx_amiga_exec_task_alive(VOID *task);
UINT tx_amiga_exec_task_signal(VOID *task, ULONG sigmask);

#endif /* AMINETXDUO_EXEC_PORT_H */
