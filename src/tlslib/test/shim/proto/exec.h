/*
 * <proto/exec.h> for the tls.library host tests: the calls
 * src/tlslib/tls_resume.c and src/tlslib/tls_runtime.c make, and nothing
 * else.  test_tls_resume.c defines them.
 *
 * The semaphore is a counter there rather than a no-op, because every entry
 * point in tls_resume.c takes tb_Lock and a path that returns while still
 * holding it deadlocks the next connection on a machine where nothing else
 * will ever release it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLS_TEST_PROTO_EXEC_H
#define AMINETXDUO_TLS_TEST_PROTO_EXEC_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>

VOID            ObtainSemaphore(struct SignalSemaphore *sem);
VOID            ReleaseSemaphore(struct SignalSemaphore *sem);

APTR            AllocVec(ULONG size, ULONG requirements);
VOID            FreeVec(APTR memory);

struct Library *OpenLibrary(STRPTR name, ULONG version);
VOID            CloseLibrary(struct Library *library);

#endif
