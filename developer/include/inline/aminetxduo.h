/* Automatically generated header (sfdc 1.11f)! Do not edit! */

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

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif /* !__INLINE_MACROS_H */

#ifndef AMINETXDUO_BASE_NAME
#define AMINETXDUO_BASE_NAME SocketBase
#endif /* !AMINETXDUO_BASE_NAME */

#define NetStackQuery(___magic, ___what, ___buffer, ___size) \
      LP4(0x366, LONG, NetStackQuery , ULONG, ___magic, d0, ULONG, ___what, d1, APTR, ___buffer, a0, ULONG, ___size, d2,\
      , AMINETXDUO_BASE_NAME)

#define NetStackControl(___magic, ___op, ___arg, ___size) \
      LP4(0x36c, LONG, NetStackControl , ULONG, ___magic, d0, ULONG, ___op, d1, APTR, ___arg, a0, ULONG, ___size, d2,\
      , AMINETXDUO_BASE_NAME)

#define if_nametoindex(___ifname) \
      LP1(0x372, ULONG, if_nametoindex , const char *, ___ifname, a0,\
      , AMINETXDUO_BASE_NAME)

#define if_indextoname(___ifindex, ___ifname) \
      LP2(0x378, char *, if_indextoname , ULONG, ___ifindex, d0, char *, ___ifname, a0,\
      , AMINETXDUO_BASE_NAME)

#define if_nameindex() \
      LP0(0x37e, struct if_nameindex *, if_nameindex ,\
      , AMINETXDUO_BASE_NAME)

#define if_freenameindex(___ptr) \
      LP1NR(0x384, if_freenameindex , struct if_nameindex *, ___ptr, a0,\
      , AMINETXDUO_BASE_NAME)

#endif /* !_INLINE_AMINETXDUO_H */
