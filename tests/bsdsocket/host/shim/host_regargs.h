/*
 * Force-included ahead of everything, and the order inside it is the point.
 *
 * src/sana2/sana2_copy.c declares its two hooks in the m68k register
 * convention, `register APTR to __asm("a0")`, which no host compiler can
 * honour. Neutering `__asm` as a function-like macro compiles them as they
 * ship, rather than putting a host #ifdef into a file that runs at interrupt
 * level in somebody else's driver.
 *
 * The system headers are pulled in first, before the macro exists, so nothing
 * in libc can be reached by it. They arrive anyway, NetX Duo's linux
 * nx_port.h includes all three, and this only fixes when.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SANA2_TEST_HOST_REGARGS_H
#define AMINETXDUO_SANA2_TEST_HOST_REGARGS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/time.h>
#include <sys/types.h>

#define __asm(x)

#endif
