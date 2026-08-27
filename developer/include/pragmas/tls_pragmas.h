/* Automatically generated header (sfdc 1.11e)! Do not edit! */
#ifndef PRAGMAS_TLS_PRAGMAS_H
#define PRAGMAS_TLS_PRAGMAS_H

/*
**   $VER: tls_pragmas.h 1.1 $Id: tls_lib.sfd,v 1.1 2026-08-27 $
**
**   Direct ROM interface (pragma) definitions.
**
**   Copyright (c) 2026 AmiNetXDuo contributors.  SPDX-License-Identifier: MIT
*/

#if defined(LATTICE) || defined(__SASC) || defined(_DCC)
#ifndef __CLIB_PRAGMA_LIBCALL
#define __CLIB_PRAGMA_LIBCALL
#endif /* __CLIB_PRAGMA_LIBCALL */
#else /* __MAXON__, __STORM__ or AZTEC_C */
#ifndef __CLIB_PRAGMA_AMICALL
#define __CLIB_PRAGMA_AMICALL
#endif /* __CLIB_PRAGMA_AMICALL */
#endif /* */

#if defined(__SASC_60) || defined(__STORM__)
#ifndef __CLIB_PRAGMA_TAGCALL
#define __CLIB_PRAGMA_TAGCALL
#endif /* __CLIB_PRAGMA_TAGCALL */
#endif /* __MAXON__, __STORM__ or AZTEC_C */

#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall TLSBase TLSOpenA 1e 90803
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(TLSBase, 0x1e, TLSOpenA(a0,d0,a1))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_TAGCALL
 #ifdef __CLIB_PRAGMA_LIBCALL
  #pragma tagcall TLSBase TLSOpenTags 1e 90803
 #endif /* __CLIB_PRAGMA_LIBCALL */
 #ifdef __CLIB_PRAGMA_AMICALL
  #pragma tagcall(TLSBase, 0x1e, TLSOpenTags(a0,d0,a1))
 #endif /* __CLIB_PRAGMA_AMICALL */
#endif /* __CLIB_PRAGMA_TAGCALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall TLSBase TLSClose 24 801
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(TLSBase, 0x24, TLSClose(a0))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall TLSBase TLSRead 2a 09803
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(TLSBase, 0x2a, TLSRead(a0,a1,d0))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall TLSBase TLSWrite 30 09803
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(TLSBase, 0x30, TLSWrite(a0,a1,d0))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall TLSBase TLSPending 36 801
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(TLSBase, 0x36, TLSPending(a0))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall TLSBase TLSInfo 3c 9802
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(TLSBase, 0x3c, TLSInfo(a0,a1))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall TLSBase TLSErrorString 42 001
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(TLSBase, 0x42, TLSErrorString(d0))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall TLSBase TLSWaitSelect 48 801
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(TLSBase, 0x48, TLSWaitSelect(a0))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall TLSBase TLSRandom 4e 0802
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(TLSBase, 0x4e, TLSRandom(a0,d0))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall TLSBase TLSBuffered 54 801
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(TLSBase, 0x54, TLSBuffered(a0))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall TLSBase TLSGetALPN 5a 09803
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(TLSBase, 0x5a, TLSGetALPN(a0,a1,d0))
#endif /* __CLIB_PRAGMA_AMICALL */

#endif /* PRAGMAS_TLS_PRAGMAS_H */
