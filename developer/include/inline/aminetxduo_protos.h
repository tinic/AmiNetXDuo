/* Automatically generated header (sfdc 1.14)! Do not edit! */

#ifndef _INLINE_AMINETXDUO_H
#define _INLINE_AMINETXDUO_H

#ifndef _SFDC_VARARG_DEFINED
#define _SFDC_VARARG_DEFINED
#ifdef __HAVE_IPTR_ATTR__
typedef APTR _sfdc_vararg __attribute__((iptr));
#else
typedef ULONG _sfdc_vararg;
#endif /* __HAVE_IPTR_ATTR__ */
#endif /* _SFDC_VARARG_DEFINED */

LONG __NetStackQuery(__reg("a6") struct Library * , __reg("d0") ULONG magic , __reg("d1") ULONG what , __reg("a0") APTR buffer , __reg("d2") ULONG size ) = "\tjsr\t-870(a6)";
#define NetStackQuery(magic, what, buffer, size) __NetStackQuery(SocketBase, (magic), (what), (buffer), (size))

LONG __NetStackControl(__reg("a6") struct Library * , __reg("d0") ULONG magic , __reg("d1") ULONG op , __reg("a0") APTR arg , __reg("d2") ULONG size ) = "\tjsr\t-876(a6)";
#define NetStackControl(magic, op, arg, size) __NetStackControl(SocketBase, (magic), (op), (arg), (size))

ULONG __if_nametoindex(__reg("a6") struct Library * , __reg("a0") const char *ifname ) = "\tjsr\t-882(a6)";
#define if_nametoindex(ifname) __if_nametoindex(SocketBase, (ifname))

char * __if_indextoname(__reg("a6") struct Library * , __reg("d0") ULONG ifindex , __reg("a0") char *ifname ) = "\tjsr\t-888(a6)";
#define if_indextoname(ifindex, ifname) __if_indextoname(SocketBase, (ifindex), (ifname))

struct if_nameindex * __if_nameindex(__reg("a6") struct Library * ) = "\tjsr\t-894(a6)";
#define if_nameindex() __if_nameindex(SocketBase)

VOID __if_freenameindex(__reg("a6") struct Library * , __reg("a0") struct if_nameindex *ptr ) = "\tjsr\t-900(a6)";
#define if_freenameindex(ptr) __if_freenameindex(SocketBase, (ptr))

#endif /* !_INLINE_AMINETXDUO_H */
