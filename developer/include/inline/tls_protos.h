/* Automatically generated header (sfdc 1.14)! Do not edit! */

#ifndef _INLINE_TLS_H
#define _INLINE_TLS_H

#ifndef _SFDC_VARARG_DEFINED
#define _SFDC_VARARG_DEFINED
#ifdef __HAVE_IPTR_ATTR__
typedef APTR _sfdc_vararg __attribute__((iptr));
#else
typedef ULONG _sfdc_vararg;
#endif /* __HAVE_IPTR_ATTR__ */
#endif /* _SFDC_VARARG_DEFINED */

struct TLSConnection * __TLSOpenA(__reg("a6") struct Library * , __reg("a0") APTR socketBase , __reg("d0") LONG sock , __reg("a1") const struct TagItem * tags ) = "\tjsr\t-30(a6)";
#define TLSOpenA(socketBase, sock, tags) __TLSOpenA(TLSBase, (socketBase), (sock), (tags))

#if !defined(NO_INLINE_STDARG) && (__STDC__ == 1L) && (__STDC_VERSION__ >= 199901L)
struct TLSConnection * __TLSOpenTags(__reg("a6") struct Library * , __reg("a0") APTR socketBase , __reg("d0") LONG sock , ULONG tag1 , ... ) = "\tmove.l\ta1,-(a7)\n\tlea\t4(a7),a1\n\tjsr\t-30(a6)\n\tmovea.l\t(a7)+,a1";
#define TLSOpenTags(socketBase, sock, ...) __TLSOpenTags(TLSBase, (socketBase), (sock), __VA_ARGS__)
#endif

VOID __TLSClose(__reg("a6") struct Library * , __reg("a0") struct TLSConnection * conn ) = "\tjsr\t-36(a6)";
#define TLSClose(conn) __TLSClose(TLSBase, (conn))

LONG __TLSRead(__reg("a6") struct Library * , __reg("a0") struct TLSConnection * conn , __reg("a1") APTR buffer , __reg("d0") LONG length ) = "\tjsr\t-42(a6)";
#define TLSRead(conn, buffer, length) __TLSRead(TLSBase, (conn), (buffer), (length))

LONG __TLSWrite(__reg("a6") struct Library * , __reg("a0") struct TLSConnection * conn , __reg("a1") CONST_APTR buffer , __reg("d0") LONG length ) = "\tjsr\t-48(a6)";
#define TLSWrite(conn, buffer, length) __TLSWrite(TLSBase, (conn), (buffer), (length))

LONG __TLSPending(__reg("a6") struct Library * , __reg("a0") struct TLSConnection * conn ) = "\tjsr\t-54(a6)";
#define TLSPending(conn) __TLSPending(TLSBase, (conn))

LONG __TLSInfo(__reg("a6") struct Library * , __reg("a0") struct TLSConnection * conn , __reg("a1") struct TLSInfo * info ) = "\tjsr\t-60(a6)";
#define TLSInfo(conn, info) __TLSInfo(TLSBase, (conn), (info))

CONST_STRPTR __TLSErrorString(__reg("a6") struct Library * , __reg("d0") LONG code ) = "\tjsr\t-66(a6)";
#define TLSErrorString(code) __TLSErrorString(TLSBase, (code))

LONG __TLSWaitSelect(__reg("a6") struct Library * , __reg("a0") struct TLSSelect * sel ) = "\tjsr\t-72(a6)";
#define TLSWaitSelect(sel) __TLSWaitSelect(TLSBase, (sel))

LONG __TLSRandom(__reg("a6") struct Library * , __reg("a0") APTR buffer , __reg("d0") LONG length ) = "\tjsr\t-78(a6)";
#define TLSRandom(buffer, length) __TLSRandom(TLSBase, (buffer), (length))

LONG __TLSBuffered(__reg("a6") struct Library * , __reg("a0") struct TLSConnection * conn ) = "\tjsr\t-84(a6)";
#define TLSBuffered(conn) __TLSBuffered(TLSBase, (conn))

LONG __TLSGetALPN(__reg("a6") struct Library * , __reg("a0") struct TLSConnection * conn , __reg("a1") APTR buffer , __reg("d0") LONG size ) = "\tjsr\t-90(a6)";
#define TLSGetALPN(conn, buffer, size) __TLSGetALPN(TLSBase, (conn), (buffer), (size))

#endif /* !_INLINE_TLS_H */
