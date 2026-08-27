/* Automatically generated header (sfdc 1.11e)! Do not edit! */

#ifndef CLIB_TLS_PROTOS_H
#define CLIB_TLS_PROTOS_H

/*
**   $VER: tls_protos.h 1.1 $Id: tls_lib.sfd,v 1.1 2026-08-27 $
**
**   C prototypes. For use with 32 bit integers only.
**
**   Copyright (c) 2026 AmiNetXDuo contributors.  SPDX-License-Identifier: MIT
*/

#include <exec/types.h>
#include <utility/tagitem.h>
#include <aminetxduo/tlslib.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* "tls.library", TLS 1.2 and 1.3 over a bsdsocket.library descriptor. */
/* Bias 30 puts the first entry at -0x01e.  Entry n is at -(30 + 6n), and */
/* src/tlslib/tls_vectors.c is the authority for the table. */
/*
 WHY THIS FILE EXISTS.  The library's entry points were GCC extended-asm
*/
/* stubs in <aminetxduo/tlslib.h> and nothing else, so SAS/C, vbcc, StormC and */
/* Aztec could not call the library at all -- no .fd, no SFD, no pragmas.  This */
/* is the description every one of those compilers reads, through the headers */
/* tools/gen-developer.sh generates from it. */
/*
 The tags, the error codes, struct TLSInfo and struct TLSSelect stay in
*/
/* <aminetxduo/tlslib.h>, which is included below and shipped in the drawer. */
/* Same split as bsdsocket.library's: data in aminetxduo/, vectors here. */
/*
 Generated headers come from tools/gen-developer.sh, do not edit them.
*/
/* Since library version 1. */
struct TLSConnection * TLSOpenA(APTR socketBase, LONG sock, const struct TagItem * tags);
struct TLSConnection * TLSOpenTags(APTR socketBase, LONG sock, ULONG tag1, ...);
VOID TLSClose(struct TLSConnection * conn);
LONG TLSRead(struct TLSConnection * conn, APTR buffer, LONG length);
LONG TLSWrite(struct TLSConnection * conn, CONST_APTR buffer, LONG length);
LONG TLSPending(struct TLSConnection * conn);
LONG TLSInfo(struct TLSConnection * conn, struct TLSInfo * info);
CONST_STRPTR TLSErrorString(LONG code);
LONG TLSWaitSelect(struct TLSSelect * sel);

/* Since library version 2.  OpenLibrary("tls.library", 2) before calling these. */
LONG TLSRandom(APTR buffer, LONG length);
LONG TLSBuffered(struct TLSConnection * conn);

/* Since library version 3.  RFC 7301, and OpenLibrary(..., 3). */
LONG TLSGetALPN(struct TLSConnection * conn, APTR buffer, LONG size);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CLIB_TLS_PROTOS_H */
