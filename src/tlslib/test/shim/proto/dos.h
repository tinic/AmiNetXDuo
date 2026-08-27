/*
 * <proto/dos.h> for the tls.library host tests: the four calls
 * src/tlslib/tls_resume.c makes to read and write its session mirror, and
 * nothing else.  test_tls_resume.c defines them over stdio, so the file
 * format is exercised against a real file on a real disk.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLS_TEST_PROTO_DOS_H
#define AMINETXDUO_TLS_TEST_PROTO_DOS_H

#include <exec/types.h>
#include <dos/dos.h>

BPTR Open(STRPTR name, LONG mode);
VOID Close(BPTR fh);
LONG Read(BPTR fh, APTR buffer, LONG length);
LONG Write(BPTR fh, const void *buffer, LONG length);

/* tls_runtime.c only, for the NetX Duo tick conversion nothing on the host
   waits on. */
VOID Delay(LONG ticks);

#endif
