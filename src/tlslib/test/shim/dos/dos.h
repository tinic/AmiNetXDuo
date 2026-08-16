/*
 * <dos/dos.h> for the tls.library host tests.  src/config/test/shim has the
 * same file without the two modes, because nothing there opens anything.
 * tls_resume.c's disk mirror does, so they are here.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLS_TEST_DOS_DOS_H
#define AMINETXDUO_TLS_TEST_DOS_DOS_H

#include <exec/types.h>

typedef long BPTR;

struct DosLibrary;

#define MODE_OLDFILE    1005L
#define MODE_NEWFILE    1006L

#endif
