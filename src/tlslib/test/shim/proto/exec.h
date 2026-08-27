/*
 * <proto/exec.h> for the tls.library host tests: the calls
 * src/tlslib/tls_resume.c and src/tlslib/tls_runtime.c make, and nothing
 * else.  test_tls_resume.c defines them.
 *
 * The semaphore is a counter there rather than a no-op, because every entry
 * point in tls_resume.c takes tb_Lock, and a path that returns while it still
 * holds tb_Lock deadlocks the next connection on a machine where nothing else
 * will ever release it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLS_TEST_PROTO_EXEC_H
#define AMINETXDUO_TLS_TEST_PROTO_EXEC_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>

VOID            InitSemaphore(struct SignalSemaphore *sem);
VOID            ObtainSemaphore(struct SignalSemaphore *sem);
VOID            ReleaseSemaphore(struct SignalSemaphore *sem);
LONG            AttemptSemaphore(struct SignalSemaphore *sem);

/* tls_netx.c brackets the mutex table with these; the tests make them
   counters, because a path that leaves the machine in Forbid() is a hang. */
VOID            Forbid(VOID);
VOID            Permit(VOID);

APTR            AllocVec(ULONG size, ULONG requirements);
VOID            FreeVec(APTR memory);

struct Library *OpenLibrary(STRPTR name, ULONG version);
VOID            CloseLibrary(struct Library *library);

#endif
