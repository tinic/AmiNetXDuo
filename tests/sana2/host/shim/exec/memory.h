/*
 * <exec/memory.h> for the sana2 host tests.  src/config/test/shim has an empty
 * one; sana2_rx.c names MEMF_PUBLIC when it allocates a reader stack, so the
 * values are here, and they are the real ones.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SANA2_TEST_EXEC_MEMORY_H
#define AMINETXDUO_SANA2_TEST_EXEC_MEMORY_H

#include <exec/types.h>

#define MEMF_ANY        0UL
#define MEMF_PUBLIC     (1UL << 0)
#define MEMF_CLEAR      (1UL << 16)

#endif
