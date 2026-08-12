/*
 * <exec/memory.h> for the tls.library host tests.  src/tlslib/tls_runtime.c
 * asks AllocVec() for MEMF_PUBLIC | MEMF_CLEAR; the values are the real ones
 * so the argument the test sees is the argument the Amiga sees.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLS_TEST_EXEC_MEMORY_H
#define AMINETXDUO_TLS_TEST_EXEC_MEMORY_H

#include <exec/types.h>

#define MEMF_ANY        0UL
#define MEMF_PUBLIC     (1UL << 0)
#define MEMF_CLEAR      (1UL << 16)

#endif
