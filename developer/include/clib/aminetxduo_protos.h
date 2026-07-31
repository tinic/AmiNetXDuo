/* Automatically generated header (sfdc 1.11f)! Do not edit! */

#ifndef CLIB_AMINETXDUO_PROTOS_H
#define CLIB_AMINETXDUO_PROTOS_H

/*
**   $VER: aminetxduo_protos.h 1.1 $Id: aminetxduo_lib.sfd,v 1.1 2026-07-31 $
**
**   C prototypes. For use with 32 bit integers only.
**
**   Copyright (c) 2026 AmiNetXDuo contributors.  SPDX-License-Identifier: MIT
*/

#include <exec/types.h>
#include <aminetxduo/ifindex.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* "bsdsocket.library" -- what AmiNetXDuo adds past the end of the NDK's SFD. */
/* Not a fork of bsdsocket_lib.sfd: same base variable, separate file, so the */
/* two generated header sets mix and neither has to be re-merged. */
/* Bias 882 puts the first entry at -0x372.  Entry n is at -(882 + 6n). */
/* src/bsdsocket/bsdsocket_vectors.c is the authority for the whole table; */
/* docs/NDK-ADDENDUM.md has the reasoning.  Generated headers come from */
/* tools/gen-developer.sh -- do not edit them. */
/* RFC 3493 section 4 -- interface identification.  bsdsocket.library revision */
/* 3 and up.  These four LVOs are fixed forever. */
ULONG if_nametoindex(const char * ifname);
char * if_indextoname(ULONG ifindex, char * ifname);
struct if_nameindex * if_nameindex(void);
VOID if_freenameindex(struct if_nameindex * ptr);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CLIB_AMINETXDUO_PROTOS_H */
