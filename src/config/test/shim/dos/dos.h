/*
 * Host test shim -- the handful of dos.library names a structure definition
 * needs. No AmigaDOS call is reachable from anything built with this.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_DOS_DOS_H
#define AMINETXDUO_TEST_DOS_DOS_H

#include <exec/types.h>

typedef long BPTR;

struct DosLibrary;

#endif
