/* Automatically generated header (sfdc 1.11e)! Do not edit! */

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

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif /* !__INLINE_MACROS_H */

#ifndef TLS_BASE_NAME
#define TLS_BASE_NAME TLSBase
#endif /* !TLS_BASE_NAME */

#define TLSOpenA(___socketBase, ___sock, ___tags) \
      LP3(0x1e, struct TLSConnection *, TLSOpenA , APTR, ___socketBase, a0, LONG, ___sock, d0, const struct TagItem *, ___tags, a1,\
      , TLS_BASE_NAME)

#ifndef NO_INLINE_STDARG
#define TLSOpenTags(___socketBase, ___sock, ___tag1, ...) \
    ({_sfdc_vararg _tags[] = { ___tag1, __VA_ARGS__ }; TLSOpenA((___socketBase), (___sock), (const struct TagItem *) _tags); })
#endif /* !NO_INLINE_STDARG */

#define TLSClose(___conn) \
      LP1NR(0x24, TLSClose , struct TLSConnection *, ___conn, a0,\
      , TLS_BASE_NAME)

#define TLSRead(___conn, ___buffer, ___length) \
      LP3(0x2a, LONG, TLSRead , struct TLSConnection *, ___conn, a0, APTR, ___buffer, a1, LONG, ___length, d0,\
      , TLS_BASE_NAME)

#define TLSWrite(___conn, ___buffer, ___length) \
      LP3(0x30, LONG, TLSWrite , struct TLSConnection *, ___conn, a0, CONST_APTR, ___buffer, a1, LONG, ___length, d0,\
      , TLS_BASE_NAME)

#define TLSPending(___conn) \
      LP1(0x36, LONG, TLSPending , struct TLSConnection *, ___conn, a0,\
      , TLS_BASE_NAME)

#define TLSInfo(___conn, ___info) \
      LP2(0x3c, LONG, TLSInfo , struct TLSConnection *, ___conn, a0, struct TLSInfo *, ___info, a1,\
      , TLS_BASE_NAME)

#define TLSErrorString(___code) \
      LP1(0x42, CONST_STRPTR, TLSErrorString , LONG, ___code, d0,\
      , TLS_BASE_NAME)

#define TLSWaitSelect(___sel) \
      LP1(0x48, LONG, TLSWaitSelect , struct TLSSelect *, ___sel, a0,\
      , TLS_BASE_NAME)

#define TLSRandom(___buffer, ___length) \
      LP2(0x4e, LONG, TLSRandom , APTR, ___buffer, a0, LONG, ___length, d0,\
      , TLS_BASE_NAME)

#define TLSBuffered(___conn) \
      LP1(0x54, LONG, TLSBuffered , struct TLSConnection *, ___conn, a0,\
      , TLS_BASE_NAME)

#define TLSGetALPN(___conn, ___buffer, ___size) \
      LP3(0x5a, LONG, TLSGetALPN , struct TLSConnection *, ___conn, a0, APTR, ___buffer, a1, LONG, ___size, d0,\
      , TLS_BASE_NAME)

#endif /* !_INLINE_TLS_H */
