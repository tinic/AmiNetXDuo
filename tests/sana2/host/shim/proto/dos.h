/*
 * <proto/dos.h> for the sana2 host tests: Delay(), which the test that needs
 * it defines.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SANA2_TEST_PROTO_DOS_H
#define AMINETXDUO_SANA2_TEST_PROTO_DOS_H

#include <exec/types.h>

#ifndef TICKS_PER_SECOND
#define TICKS_PER_SECOND    50
#endif

VOID Delay(LONG ticks);

#endif
