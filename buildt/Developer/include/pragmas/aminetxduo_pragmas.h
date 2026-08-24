/* Automatically generated header (sfdc 1.11e)! Do not edit! */
#ifndef PRAGMAS_AMINETXDUO_PRAGMAS_H
#define PRAGMAS_AMINETXDUO_PRAGMAS_H

/*
**   $VER: aminetxduo_pragmas.h 1.1 $Id: aminetxduo_lib.sfd,v 1.1 2026-07-31 $
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
 #pragma libcall SocketBase NetStackQuery 366 281004
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(SocketBase, 0x366, NetStackQuery(d0,d1,a0,d2))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall SocketBase NetStackControl 36c 281004
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(SocketBase, 0x36c, NetStackControl(d0,d1,a0,d2))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall SocketBase if_nametoindex 372 801
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(SocketBase, 0x372, if_nametoindex(a0))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall SocketBase if_indextoname 378 8002
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(SocketBase, 0x378, if_indextoname(d0,a0))
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall SocketBase if_nameindex 37e 00
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(SocketBase, 0x37e, if_nameindex())
#endif /* __CLIB_PRAGMA_AMICALL */
#ifdef __CLIB_PRAGMA_LIBCALL
 #pragma libcall SocketBase if_freenameindex 384 801
#endif /* __CLIB_PRAGMA_LIBCALL */
#ifdef __CLIB_PRAGMA_AMICALL
 #pragma amicall(SocketBase, 0x384, if_freenameindex(a0))
#endif /* __CLIB_PRAGMA_AMICALL */

#endif /* PRAGMAS_AMINETXDUO_PRAGMAS_H */
