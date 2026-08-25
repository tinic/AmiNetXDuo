/*
 * toolsock, bsdsocket.library through its published vectors.
 *
 * Hazard: every stub must declare d1/a0/a1 written (an AmigaOS library call
 * clobbers d0/d1/a0/a1), and nothing may be called between the first register
 * variable and the `jsr` -- compute such values into a plain local first.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

#include "toolbudget.h"

VOID tool_fd_zero(ToolFdSet *set)
{
    ULONG i;

    for (i = 0; i < (ULONG)TOOL_FD_WORDS; i++)
        set->w[i] = 0;
}

VOID tool_fd_add(ToolFdSet *set, LONG fd)
{
    if (fd < 0 || fd >= (LONG)(TOOL_FD_WORDS * 32))
        return;

    set->w[(ULONG)fd / 32UL] |= 1UL << ((ULONG)fd % 32UL);
}

BOOL tool_fd_isset(const ToolFdSet *set, LONG fd)
{
    if (fd < 0 || fd >= (LONG)(TOOL_FD_WORDS * 32))
        return FALSE;

    return (set->w[(ULONG)fd / 32UL] & (1UL << ((ULONG)fd % 32UL))) != 0
               ? TRUE : FALSE;
}

struct Library *tool_socket_open(VOID)
{
    struct Library *base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);

    if (base == NULL)
    {
        if (tool_stack_installed())
            tool_error("the network did not start");
        else
            tool_error("no bsdsocket.library");
        tool_explain_no_stack();
    }

    return base;
}

/* LVO -0x01e */
LONG tool_sock_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = domain;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

/*
 * A sockaddr_in starts with its own length, 16.  This NDK's sockaddr_in6 has
 * no length byte and starts with the family, 23.  So byte 0 tells the two
 * apart and there is no third case to get wrong.
 */
static LONG tool_sock_len(const ToolSockAddrAny *sa)
{
    return (sa->in6.sin6_family == (UBYTE)TOOL_AF_INET6)
               ? (LONG)sizeof(ToolSockAddr6) : (LONG)sizeof(ToolSockAddr);
}

/* LVO -0x024 */
LONG tool_sock_bind(struct Library *base, LONG s, const ToolSockAddrAny *sa)
{
    LONG salen = tool_sock_len(sa);     /* a call, so it goes first */

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)sa;
    register LONG            d1  __asm("d1") = salen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

/* LVO -0x02a */
LONG tool_sock_listen(struct Library *base, LONG s, LONG backlog)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = backlog;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-42:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1)
                      : "a0", "a1", "cc", "memory");
    return res;
}

/* LVO -0x030 */
LONG tool_sock_accept(struct Library *base, LONG s, ToolSockAddrAny *from)
{
    LONG namelen = (LONG)sizeof(*from);

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)from;
    register APTR            a1  __asm("a1") = (APTR)&namelen;
    register LONG            res __asm("d0");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-48:W)"
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "d1", "cc", "memory");
    return res;
}

/* LVO -0x036 */
LONG tool_sock_connect(struct Library *base, LONG s, const ToolSockAddrAny *sa)
{
    LONG salen = tool_sock_len(sa);

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)sa;
    register LONG            d1  __asm("d1") = salen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

/* LVO -0x03c */
LONG tool_sock_sendto(struct Library *base, LONG s, const void *buf, LONG len,
                      const ToolSockAddrAny *to)
{
    LONG tolen = tool_sock_len(to);

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register CONST_APTR      a1  __asm("a1") = (CONST_APTR)to;
    register LONG            d3  __asm("d3") = tolen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-60:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (d3)
                      : "cc", "memory");
    return res;
}

/* LVO -0x042 */
LONG tool_sock_send(struct Library *base, LONG s, const void *buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-66:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

LONG tool_sock_send_full(struct Library *base, LONG s, const void *buf,
                         LONG len)
{
    const UBYTE *p = (const UBYTE *)buf;
    LONG         sent = 0;

    while (sent < len)
    {
        LONG n = tool_sock_send(base, s, &p[sent], len - sent);

        if (n > 0)
        {
            sent += n;
            continue;
        }

        if (n < 0 && tool_sock_errno(base) == TOOL_EINTR)
            continue;

        return -1;
    }

    return sent;
}

/* LVO -0x048 */
LONG tool_sock_recvfrom(struct Library *base, LONG s, void *buf, LONG len,
                        ToolSockAddrAny *from)
{
    LONG namelen = (LONG)sizeof(*from);

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register APTR            a1  __asm("a1") = (APTR)from;
    register APTR            a2  __asm("a2") = (APTR)&namelen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-72:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (a2)
                      : "cc", "memory");
    return res;
}

/* LVO -0x04e */
LONG tool_sock_recv(struct Library *base, LONG s, void *buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-78:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

/* LVO -0x054 */
LONG tool_sock_shutdown(struct Library *base, LONG s, LONG how)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = how;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-84:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1)
                      : "a0", "a1", "cc", "memory");
    return res;
}

/* LVO -0x05a */
LONG tool_sock_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
                          const void *val, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = level;
    register LONG            d2  __asm("d2") = name;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)val;
    register LONG            d3  __asm("d3") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-90:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (d3)
                      : "a1", "cc", "memory");
    return res;
}

/* LVO -0x060 */
LONG tool_sock_getsockopt(struct Library *base, LONG s, LONG level, LONG name,
                          void *val, LONG *len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = level;
    register LONG            d2  __asm("d2") = name;
    register APTR            a0  __asm("a0") = (APTR)val;
    register APTR            a1  __asm("a1") = (APTR)len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-96:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (a1)
                      : "cc", "memory");
    return res;
}

/* LVO -0x066 */
LONG tool_sock_getsockname(struct Library *base, LONG s, ToolSockAddrAny *sa)
{
    LONG namelen = (LONG)sizeof(*sa);

    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)sa;
    register APTR            a1  __asm("a1") = (APTR)&namelen;
    register LONG            res __asm("d0");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-102:W)"
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "d1", "cc", "memory");
    return res;
}

/* LVO -0x072 */
LONG tool_sock_ioctl(struct Library *base, LONG s, ULONG req, void *argp)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register ULONG           d1  __asm("d1") = req;
    register APTR            a0  __asm("a0") = (APTR)argp;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-114:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

/* LVO -0x078 */
LONG tool_sock_close(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* LVO -0x07e */
LONG tool_sock_select(struct Library *base, LONG nfds, ToolFdSet *readfds,
                      ToolFdSet *writefds, ToolTimeval *tv)
{
    return tool_sock_select_sigs(base, nfds, readfds, writefds, tv, NULL);
}

/*
 * The mask is in and out: on return it holds the signals that were received,
 * and those have been cleared from the task.  So a caller passes only the
 */
LONG tool_sock_select_sigs(struct Library *base, LONG nfds, ToolFdSet *readfds,
                           ToolFdSet *writefds, ToolTimeval *tv,
                           ULONG *sigmask)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = (APTR)readfds;
    register APTR            a1  __asm("a1") = (APTR)writefds;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = (APTR)tv;
    register ULONG          *d1  __asm("d1") = sigmask;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return res;
}

/* LVO -0x0a2 */
LONG tool_sock_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* LVO -0x0d2 */
ToolHostEnt *tool_sock_gethostbyname(struct Library *base, const char *name)
{
    register struct Library *a6  __asm("a6") = base;
    register const char     *a0  __asm("a0") = name;
    register ToolHostEnt    *res __asm("d0");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return res;
}

/* LVO -0x0ea */
ToolServEnt *tool_sock_getservbyname(struct Library *base, const char *name,
                                     const char *proto)
{
    register struct Library *a6  __asm("a6") = base;
    register const char     *a0  __asm("a0") = name;
    register const char     *a1  __asm("a1") = proto;
    register ToolServEnt    *res __asm("d0");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-234:W)"
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (a1)
                      : "d1", "cc", "memory");
    return res;
}

/*
 *   pragmas/bsdsocket_pragmas.h  inet_ntop(d0,a0,a1,d1)
 *                                freeaddrinfo(a0)
 *                                getaddrinfo(a0,a1,a2,a3)
 */

/* LVO -0x258 */
char *tool_sock_ntop(struct Library *base, LONG af, const void *src,
                     char *dst, LONG size)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = af;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)src;
    register APTR            a1  __asm("a1") = (APTR)dst;
    register LONG            d1  __asm("d1") = size;
    register char           *res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-600:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (d1)
                      : "cc", "memory");
    return res;
}

/* LVO -0x324 */
VOID tool_sock_freeaddrinfo(struct Library *base, ToolAddrInfo *ai)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = (APTR)ai;
    register LONG _clob_d0 __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-804:W)"
                      : "=r" (_clob_d0), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0)
                      : "cc", "memory");
}

/* LVO -0x32a */
LONG tool_sock_getaddrinfo(struct Library *base, const char *node,
                           const char *service, const ToolAddrInfo *hints,
                           ToolAddrInfo **res)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)node;
    register CONST_APTR      a1  __asm("a1") = (CONST_APTR)service;
    register CONST_APTR      a2  __asm("a2") = (CONST_APTR)hints;
    register APTR            a3  __asm("a3") = (APTR)res;
    register LONG            ret __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-810:W)"
                      : "=r" (ret), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (a1), "r" (a2), "r" (a3)
                      : "cc", "memory");
    return ret;
}

BOOL tool_sock_have_lvo(struct Library *base, ULONG lvo)
{
    return (base != NULL && (ULONG)base->lib_NegSize >= lvo) ? TRUE : FALSE;
}

BOOL tool_sock_have_ipv6(struct Library *base)
{
    LONG s = tool_sock_socket(base, TOOL_AF_INET6, TOOL_SOCK_DGRAM, 0);

    if (s < 0)
        return FALSE;

    (VOID)tool_sock_close(base, s);

    return TRUE;
}

VOID tool_addr_v4(ToolAddr *addr, ULONG v4)
{
    ULONG i;

    addr->ta_Family  = (UWORD)TOOL_AF_INET;
    addr->ta_Pad     = 0;
    addr->ta_V4      = v4;
    addr->ta_ScopeId = 0;

    for (i = 0; i < 16UL; i++)
        addr->ta_V6[i] = 0;
}

VOID tool_addr_any(ToolAddr *addr, LONG family)
{
    tool_addr_v4(addr, 0);

    if (family == TOOL_AF_INET6)
        addr->ta_Family = (UWORD)TOOL_AF_INET6;
}

LONG tool_sock_addr(ToolSockAddrAny *sa, const ToolAddr *addr, UWORD port)
{
    ULONG i;

    if (TOOL_ADDR_IS6(addr))
    {
        sa->in6.sin6_family   = (UBYTE)TOOL_AF_INET6;
        sa->in6.sin6_pad      = 0;
        sa->in6.sin6_port     = port;   /* big-endian host: already network */
        sa->in6.sin6_flowinfo = 0;
        sa->in6.sin6_scope_id = addr->ta_ScopeId;

        for (i = 0; i < 16UL; i++)
            sa->in6.sin6_addr[i] = addr->ta_V6[i];

        return (LONG)sizeof(ToolSockAddr6);
    }

    for (i = 0; i < (ULONG)sizeof(sa->in.sin_zero); i++)
        sa->in.sin_zero[i] = 0;

    sa->in.sin_len    = (UBYTE)sizeof(ToolSockAddr);
    sa->in.sin_family = (UBYTE)TOOL_AF_INET;
    sa->in.sin_port   = port;
    sa->in.sin_addr   = addr->ta_V4;

    return (LONG)sizeof(ToolSockAddr);
}

LONG tool_sock_addr_v4(ToolSockAddrAny *sa, ULONG v4, UWORD port)
{
    ToolAddr addr;

    tool_addr_v4(&addr, v4);

    return tool_sock_addr(sa, &addr, port);
}

BOOL tool_sock_addr_get(const ToolSockAddrAny *sa, ToolAddr *out)
{
    ULONG i;

    if (sa->in6.sin6_family == (UBYTE)TOOL_AF_INET6)
    {
        out->ta_Family  = (UWORD)TOOL_AF_INET6;
        out->ta_Pad     = 0;
        out->ta_V4      = 0;
        out->ta_ScopeId = sa->in6.sin6_scope_id;

        for (i = 0; i < 16UL; i++)
            out->ta_V6[i] = sa->in6.sin6_addr[i];

        return TRUE;
    }

    if (sa->in.sin_family != (UBYTE)TOOL_AF_INET)
        return FALSE;

    tool_addr_v4(out, sa->in.sin_addr);

    return TRUE;
}

UWORD tool_sock_addr_port(const ToolSockAddrAny *sa)
{
    return (sa->in6.sin6_family == (UBYTE)TOOL_AF_INET6)
               ? sa->in6.sin6_port : sa->in.sin_port;
}

BOOL tool_addr_same(const ToolAddr *a, const ToolAddr *b)
{
    ULONG i;

    if (a->ta_Family != b->ta_Family)
        return FALSE;

    if (!TOOL_ADDR_IS6(a))
        return (a->ta_V4 == b->ta_V4) ? TRUE : FALSE;

    for (i = 0; i < 16UL; i++)
    {
        if (a->ta_V6[i] != b->ta_V6[i])
            return FALSE;
    }

    return TRUE;
}

BOOL tool_sock_addr_same(const ToolSockAddrAny *a, const ToolSockAddrAny *b)
{
    ToolAddr x;
    ToolAddr y;

    if (tool_sock_addr_port(a) != tool_sock_addr_port(b))
        return FALSE;

    if (!tool_sock_addr_get(a, &x) || !tool_sock_addr_get(b, &y))
        return FALSE;

    return tool_addr_same(&x, &y);
}

VOID tool_addr_text(struct Library *base, const ToolAddr *addr,
                    char *buf, ULONG buflen)
{
    if (buflen == 0)
        return;

    if (!TOOL_ADDR_IS6(addr))
    {
        ami_config_format_ip(addr->ta_V4, buf, buflen);
        return;
    }

    buf[0] = '\0';
    if (!tool_sock_have_lvo(base, 0x25eUL) ||
        tool_sock_ntop(base, TOOL_AF_INET6, addr->ta_V6, buf, (LONG)buflen)
            == NULL)
    {
        tool_copy_string(buf, buflen, "?");
    }
}

VOID tool_sock_addr_text(struct Library *base, const ToolSockAddrAny *sa,
                         char *buf, ULONG buflen)
{
    ToolAddr addr;

    if (!tool_sock_addr_get(sa, &addr))
    {
        tool_copy_string(buf, buflen, "?");
        return;
    }

    tool_addr_text(base, &addr, buf, buflen);
}

const char *tool_host_unbracket(const char *host, char *buf, ULONG buflen)
{
    ULONG len = 0;
    ULONG i;

    if (host == NULL || host[0] != '[')
        return host;

    while (host[len + 1] != '\0' && host[len + 1] != ']')
        len++;

    if (host[len + 1] != ']' || host[len + 2] != '\0' || len + 1 >= buflen)
        return host;

    for (i = 0; i < len; i++)
        buf[i] = host[i + 1];
    buf[len] = '\0';

    return buf;
}

/* An address with a colon in it can only be meant as an IPv6 literal. */
static BOOL tool_looks_v6(const char *host)
{
    ULONG i;

    for (i = 0; host[i] != '\0'; i++)
    {
        if (host[i] == ':')
            return TRUE;
    }

    return FALSE;
}

static VOID tool_no_ipv6(struct Library *base, const char *host)
{
    if (!tool_sock_have_lvo(base, 0x330UL))
    {
        tool_error("cannot use \"%s\": the bsdsocket.library on this machine "
                   "has no getaddrinfo", (LONG)host);
        return;
    }

    if (tool_sock_have_ipv6(base))
    {
        tool_error("%s: not an address",
                   (LONG)host);
        return;
    }

    tool_error("%s: this machine's network has no IPv6",
               (LONG)host);
    tool_no_ipv6_note();
}

/* gethostbyname(), for a library whose table stops short of getaddrinfo. */
static BOOL tool_resolve_old(struct Library *base, const char *host,
                             ToolAddr *out)
{
    ToolHostEnt *he = tool_sock_gethostbyname(base, host);
    ULONG        address = 0;
    ULONG        i;

    if (he == NULL || he->h_addr_list == NULL ||
        he->h_addr_list[0] == NULL || he->h_length != 4)
        return FALSE;

    for (i = 0; i < 4UL; i++)
        address = (address << 8) | (ULONG)(UBYTE)he->h_addr_list[0][i];

    tool_addr_v4(out, address);

    return TRUE;
}

/* "IPv4"/"IPv6" and "-4"/"-6" for a pinned family, for the messages below. */
static const char *tool_family_name(LONG want)
{
    return (want == TOOL_AF_INET6) ? "IPv6" : "IPv4";
}

static const char *tool_family_flag(LONG want)
{
    return (want == TOOL_AF_INET6) ? "-6" : "-4";
}

/* One address in the list already. */
static BOOL tool_list_has(const ToolAddrList *list, const ToolAddr *addr)
{
    ULONG i;

    for (i = 0; i < list->count; i++)
    {
        if (tool_addr_same(&list->addr[i], addr))
            return TRUE;
    }

    return FALSE;
}

/*
 * One getaddrinfo(), no output.  Returns 0 with *out filled, or the EAI_ code.
 * An answer with nothing usable in it counts as EAI_NONAME: the caller only
 * tells a name that is not there from one worth asking about again.
 */
static LONG tool_gai_list(struct Library *base, const char *host, LONG want,
                          BOOL literal, ToolAddrList *out)
{
    ToolAddrInfo  hints;
    ToolAddrInfo *list = NULL;
    ToolAddrInfo *ai;
    LONG          rc;

    out->count = 0;

    hints.ai_flags     = literal ? TOOL_AI_NUMERICHOST : 0;
    hints.ai_family    = want;
    hints.ai_socktype  = TOOL_SOCK_STREAM;
    hints.ai_protocol  = 0;
    hints.ai_addrlen   = 0;
    hints.ai_addr      = NULL;
    hints.ai_canonname = NULL;
    hints.ai_next      = NULL;

    rc = tool_sock_getaddrinfo(base, host, NULL, &hints, &list);
    if (rc == 0)
    {
        /* The library's order is kept: IPv6 first, so a dual-stack name still
           prefers the AAAA and the A is what it falls back to. */
        for (ai = list; ai != NULL && out->count < TOOL_ADDR_TRIES;
             ai = ai->ai_next)
        {
            ToolAddr one;

            if (ai->ai_addr == NULL)
                continue;
            if (!tool_sock_addr_get(ai->ai_addr, &one))
                continue;
            if (tool_list_has(out, &one))
                continue;

            out->addr[out->count++] = one;
        }

        tool_sock_freeaddrinfo(base, list);
    }

    return (rc == 0 && out->count == 0) ? (LONG)TOOL_EAI_NONAME : rc;
}

/* The same, for a caller that only wants to know whether there is one. */
static LONG tool_gai_try(struct Library *base, const char *host, LONG want,
                         BOOL literal, ToolAddr *out)
{
    ToolAddrList list;
    LONG         rc = tool_gai_list(base, host, want, literal, &list);

    if (rc == 0)
        *out = list.addr[0];

    return rc;
}

BOOL tool_sock_family_absent(struct Library *base, const char *host, LONG want)
{
    ToolAddr other;
    LONG     alt;

    if (want == TOOL_AF_UNSPEC)
        return FALSE;

    alt = (want == TOOL_AF_INET) ? TOOL_AF_INET6 : TOOL_AF_INET;

    return (BOOL)(tool_gai_try(base, host, alt, FALSE, &other) == 0);
}

VOID tool_sock_say_no_family(const char *host, LONG want)
{
    tool_error("%s has no %s address, and %s was given",
               (LONG)host, (LONG)tool_family_name(want),
               (LONG)tool_family_flag(want));
}

BOOL tool_sock_resolve_list(struct Library *base, const char *host, LONG want,
                            ToolAddrList *out)
{
    char          unbracketed[TOOL_ADDR_STRLEN];
    ULONG         address = 0;
    BOOL          literal;
    LONG          rc;

    out->count = 0;

    host = tool_host_unbracket(host, unbracketed, sizeof(unbracketed));

    literal = tool_looks_v6(host);

    if (ami_config_parse_ip(host, &address))
    {
        if (want == TOOL_AF_INET6)
        {
            tool_error("%s is an IPv4 address, and -6 was given",
                       (LONG)host);
            return FALSE;
        }

        tool_addr_v4(&out->addr[0], address);
        out->count = 1;
        return TRUE;
    }

    if (literal && want == TOOL_AF_INET)
    {
        tool_error("%s is an IPv6 address, and -4 was given", (LONG)host);
        return FALSE;
    }

    if (!tool_sock_have_lvo(base, 0x330UL))
    {
        /*
         * No getaddrinfo in this library's table.  A name can still be looked
         * up the old way.  An IPv6 literal cannot be used at all, and neither
         * can -6, because gethostbyname() only ever answers with an A.
         */
        if (!literal && want != TOOL_AF_INET6 &&
            tool_resolve_old(base, host, &out->addr[0]))
        {
            out->count = 1;
            return TRUE;
        }

        if (literal || want == TOOL_AF_INET6)
            tool_no_ipv6(base, host);
        else
        {
            tool_error("cannot resolve \"%s\"", (LONG)host);
            tool_explain_resolve(host, AMI_NET_ERR_NONAME);
        }

        return FALSE;
    }

    if (want == TOOL_AF_INET6 && !tool_sock_have_ipv6(base))
    {
        tool_error("%s: this machine's network has no IPv6", (LONG)host);
        tool_no_ipv6_note();
        return FALSE;
    }

    rc = tool_gai_list(base, host, want, literal, out);
    if (rc == 0)
        return TRUE;

    if (literal)
    {
        tool_no_ipv6(base, host);
        return FALSE;
    }

    /* The probe costs a second lookup, so it is only made when the first one
       came back with a verdict.  EAI_AGAIN means nobody answered, and asking a
       silent name server twice doubles the wait to say the same thing. */
    if (rc == (LONG)TOOL_EAI_NONAME &&
        tool_sock_family_absent(base, host, want))
    {
        tool_sock_say_no_family(host, want);
        return FALSE;
    }

    tool_error("cannot resolve \"%s\"", (LONG)host);
    tool_explain_resolve(host,
                         (rc == (LONG)TOOL_EAI_AGAIN) ? AMI_NET_ERR_TIMEOUT
                                                      : AMI_NET_ERR_NONAME);

    return FALSE;
}

BOOL tool_sock_resolve_af(struct Library *base, const char *host, LONG want,
                          ToolAddr *out)
{
    ToolAddrList list;

    if (!tool_sock_resolve_list(base, host, want, &list))
        return FALSE;

    *out = list.addr[0];

    return TRUE;
}

BOOL tool_sock_resolve(struct Library *base, const char *host, ToolAddr *out)
{
    return tool_sock_resolve_af(base, host, TOOL_AF_UNSPEC, out);
}

LONG tool_sock_connect_timed(struct Library *base, LONG s,
                             const ToolSockAddrAny *sa, ULONG timeout,
                             LONG *why)
{
    ToolFdSet   writefds;
    ToolTimeval tv;
    LONG        nonblock = 1;
    LONG        err = 0;
    LONG        errlen = (LONG)sizeof(err);
    LONG        ready;

    *why = 0;

    if (timeout == 0)
    {
        if (tool_sock_connect(base, s, sa) == 0)
            return 0;

        *why = tool_sock_errno(base);
        return -1;
    }

    if (tool_sock_ioctl(base, s, TOOL_FIONBIO, &nonblock) != 0)
    {
        /* No non-blocking mode: the blocking call is still correct, it just
           cannot be cut short. */
        if (tool_sock_connect(base, s, sa) == 0)
            return 0;

        *why = tool_sock_errno(base);
        return -1;
    }

    if (tool_sock_connect(base, s, sa) != 0)
    {
        LONG e = tool_sock_errno(base);

        if (e != TOOL_EINPROGRESS && e != TOOL_EWOULDBLOCK)
        {
            *why = e;
            return -1;
        }

        tool_fd_zero(&writefds);
        tool_fd_add(&writefds, s);

        tv.tv_secs  = (LONG)timeout;
        tv.tv_micro = 0;

        ready = tool_sock_select(base, s + 1, NULL, &writefds, &tv);
        if (ready == 0)
            return TOOL_CONNECT_TIMEDOUT;
        if (ready < 0)
        {
            *why = tool_sock_errno(base);
            return -1;
        }

        if (tool_sock_getsockopt(base, s, TOOL_SOL_SOCKET, TOOL_SO_ERROR,
                                 &err, &errlen) != 0)
        {
            /*
             * No SO_ERROR to read.  A second connect() reports EISCONN if the
             * socket is already through, and the real reason if it failed.
             */
            err = (tool_sock_connect(base, s, sa) == 0)
                      ? 0 : tool_sock_errno(base);
            if (err == TOOL_EISCONN)
                err = 0;
        }

        if (err != 0)
        {
            *why = err;
            return -1;
        }
    }

    nonblock = 0;
    (VOID)tool_sock_ioctl(base, s, TOOL_FIONBIO, &nonblock);

    return 0;
}

/*
 * ami_millis() is monotonic and wraps at 2^32, so the unsigned difference is
 * right across the wrap.  It answers 0 throughout on a machine where
 */
#define TOOL_ELAPSED(t0)    ((ULONG)((ami_millis() - (t0)) / 1000UL))

/* -p on nc: the wildcard of the family being connected to, plus a port. */
static BOOL tool_bind_local(struct Library *base, LONG sock,
                            const ToolAddr *peer, UWORD localport)
{
    ToolSockAddrAny local;
    ToolAddr        any = *peer;
    LONG            one = 1;
    ULONG           b;

    (VOID)tool_sock_setsockopt(base, sock, TOOL_SOL_SOCKET, TOOL_SO_REUSEADDR,
                               &one, (LONG)sizeof(one));

    any.ta_V4 = 0;
    for (b = 0; b < 16UL; b++)
        any.ta_V6[b] = 0;

    (VOID)tool_sock_addr(&local, &any, localport);

    return (BOOL)(tool_sock_bind(base, sock, &local) == 0);
}

/* One address and one time budget.  The caller owns the order and retries. */
static LONG tool_connect_one(struct Library *base, const ToolConnect *how,
                             const ToolAddr *address, ULONG budget,
                             BOOL again, ToolAddr *chosen, LONG *why)
{
    ToolSockAddrAny sa;
    LONG            sock;
    LONG            result;

    *chosen = *address;

    if (how->announce)
    {
        char dotted[TOOL_ADDR_STRLEN];

        tool_addr_text(base, address, dotted, sizeof(dotted));

        tool_printf(again ? "Retrying %s port %ld...\n"
                          : "Trying %s port %ld...\n",
                    (LONG)dotted, (LONG)how->port);
    }

    sock = tool_sock_socket(base, (LONG)address->ta_Family,
                            how->socktype, 0);
    if (sock < 0)
    {
        *why = tool_sock_errno(base);
        return TOOL_CONNECT_NOSOCKET;
    }

    if (how->localport != 0 &&
        !tool_bind_local(base, sock, address, how->localport))
    {
        *why = tool_sock_errno(base);
        (VOID)tool_sock_close(base, sock);
        return TOOL_CONNECT_NOBIND;
    }

    (VOID)tool_sock_addr(&sa, address, how->port);

    result = tool_sock_connect_timed(base, sock, &sa, budget, why);
    if (result == 0)
        return sock;

    (VOID)tool_sock_close(base, sock);

    if (result == TOOL_CONNECT_TIMEDOUT)
    {
        /* So a caller that prints only the errno still says something true. */
        *why = TOOL_ETIMEDOUT;
        return TOOL_CONNECT_TIMEDOUT;
    }

    return TOOL_CONNECT_FAILED;
}

LONG tool_sock_connect_host(struct Library *base, const char *host,
                            const ToolConnect *how, ToolAddr *chosen,
                            LONG *why)
{
    ToolAddrList list;
    ToolBudget   budget;
    ULONG        started;
    ULONG        i;
    LONG         rc = TOOL_CONNECT_FAILED;

    *why = 0;
    tool_addr_v4(chosen, 0);            /* printable even if nothing resolved */

    if (!tool_sock_resolve_list(base, host, how->family, &list))
        return TOOL_CONNECT_NORESOLVE;

    /* Opens timer.device before anything is timed, as ping.c does. */
    started = ami_millis();

    tool_budget_init(&budget, how->timeout, list.count);

    /* First pass, in the resolver's order.  toolbudget.h has the rules that
       decide what each address is given; nothing here re-decides them. */
    for (i = 0; i < list.count; i++)
    {
        ULONG secs;
        LONG  result;
        int   cut;

        if (!tool_budget_first(&budget, i, TOOL_ELAPSED(started), &secs, &cut))
            break;

        result = tool_connect_one(base, how, &list.addr[i], secs, FALSE,
                                  chosen, why);
        if (result >= 0)
            return result;
        if (result == TOOL_CONNECT_NOBIND)
            return result;

        rc = result;

        tool_budget_done(&budget, i, secs,
                         (result == TOOL_CONNECT_TIMEDOUT) ? 1 : 0, cut);
    }

    /* Second pass: the addresses that were cut short, out of whatever the
       rest of the list left behind. */
    for (i = 0; i < list.count; i++)
    {
        ULONG secs = tool_budget_again(&budget, i, TOOL_ELAPSED(started));
        LONG  result;

        if (secs == 0UL)
            continue;

        result = tool_connect_one(base, how, &list.addr[i], secs, TRUE,
                                  chosen, why);
        if (result >= 0)
            return result;
        if (result == TOOL_CONNECT_NOBIND)
            return result;

        rc = result;

        tool_budget_done(&budget, i, secs,
                         (result == TOOL_CONNECT_TIMEDOUT) ? 1 : 0, 0);
    }

    return rc;
}

BOOL tool_arg_family(LONG four, LONG six, LONG *want)
{
    if (four != 0 && six != 0)
    {
        tool_error("-4 and -6 cannot both be given");
        return FALSE;
    }

    *want = (four != 0) ? TOOL_AF_INET
          : (six  != 0) ? TOOL_AF_INET6
                        : TOOL_AF_UNSPEC;

    return TRUE;
}

UWORD tool_sock_port(struct Library *base, const char *text, const char *proto)
{
    ToolServEnt *se;
    ULONG        value = 0;
    ULONG        i;

    if (text == NULL || text[0] == '\0')
    {
        tool_error("no port given");
        return 0;
    }

    if (text[0] >= '0' && text[0] <= '9')
    {
        for (i = 0; text[i] != '\0'; i++)
        {
            if (text[i] < '0' || text[i] > '9')
            {
                tool_error("\"%s\" is not a port number", (LONG)text);
                return 0;
            }
            value = (value * 10UL) + (ULONG)(text[i] - '0');
            if (value > 65535UL)
            {
                tool_error("port %s is out of range", (LONG)text);
                return 0;
            }
        }

        if (value == 0)
        {
            tool_error("port 0 is not a port");
            return 0;
        }

        return (UWORD)value;
    }

    /*
     * A service name, through the library's own getservbyname(), so the answer
     * comes from DEVS:Internet/services like every other program's.
     */
    se = tool_sock_getservbyname(base, text, proto);
    if (se == NULL)
    {
        tool_error("%s: no such service: %s",
                   (LONG)proto, (LONG)text);
        return 0;
    }

    return (UWORD)se->s_port;           /* network order == host order here */
}

static char tool_sock_errbuf[64];

static const char *tool_sock_unnamed(LONG err)
{
    static const char prefix[] = "the stack reported error ";
    char  digits[12];
    ULONG n = 0;
    ULONG o = 0;
    ULONG v;

    while (prefix[o] != '\0')
    {
        tool_sock_errbuf[o] = prefix[o];
        o++;
    }

    v = (err < 0) ? (ULONG)(-err) : (ULONG)err;
    if (err < 0)
        tool_sock_errbuf[o++] = '-';

    if (v == 0)
        digits[n++] = '0';
    while (v > 0)
    {
        digits[n++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }
    while (n > 0)
        tool_sock_errbuf[o++] = digits[--n];

    tool_sock_errbuf[o] = '\0';
    return tool_sock_errbuf;
}

const char *tool_sock_errstr(LONG err)
{
    switch (err)
    {
        case 0:                  return "no error";
        case TOOL_EINTR:         return "interrupted";
        case TOOL_EPIPE:         return "broken pipe";
        case TOOL_EWOULDBLOCK:   return "would block";
        case TOOL_EINPROGRESS:   return "still connecting";
        case TOOL_EADDRINUSE:    return "address already in use";
        case TOOL_ENETUNREACH:   return "network unreachable";
        case TOOL_ECONNRESET:    return "connection reset by peer";
        case TOOL_ETIMEDOUT:     return "connection timed out";
        case TOOL_ECONNREFUSED:  return "connection refused";
        case TOOL_EHOSTUNREACH:  return "host unreachable";
        default:                 return tool_sock_unnamed(err);
    }
}

VOID tool_sock_fail_why(struct Library *base, const char *what,
                        const ToolAddr *addr, UWORD port, LONG err)
{
    char text[TOOL_ADDR_STRLEN];

    tool_addr_text(base, addr, text, sizeof(text));

    tool_error("cannot %s %s port %ld: %s", (LONG)what, (LONG)text,
               (LONG)port, (LONG)tool_sock_errstr(err));
}

VOID tool_sock_fail(struct Library *base, const char *what,
                    const ToolAddr *addr, UWORD port)
{
    tool_sock_fail_why(base, what, addr, port, tool_sock_errno(base));
}

VOID tool_input_open(ToolInput *in, BOOL want_raw)
{
    in->fh          = Input();
    in->interactive = (IsInteractive(in->fh) != 0) ? TRUE : FALSE;
    in->raw         = FALSE;
    in->eof         = FALSE;

    if (in->interactive && want_raw)
    {
        /*
         * RAW mode is a property of the console, not of this process: every
         * caller must reach tool_input_close() on every path including Ctrl-C
         * or the user's Shell is left unusable.
         */
        if (SetMode(in->fh, 1L) != 0)
            in->raw = TRUE;
    }
}

VOID tool_input_close(ToolInput *in)
{
    if (in->raw)
    {
        (VOID)SetMode(in->fh, 0L);
        in->raw = FALSE;
    }
}

LONG tool_input_read(ToolInput *in, UBYTE *buf, LONG len, ULONG micros)
{
    LONG n;

    if (in->eof)
        return 0;

    if (in->interactive)
    {
        if (WaitForChar(in->fh, (LONG)micros) == 0)
            return -1;              /* nothing pressed yet */
    }

    n = Read(in->fh, (APTR)buf, len);
    if (n <= 0)
    {
        in->eof = TRUE;
        return 0;
    }

    return n;
}

/*
 * Straight to standard output, past dos.library's buffer.  VPrintf() is
 * buffered and Write() is not, so without the Flush() the two interleave.
 */
LONG tool_output_write(const UBYTE *buf, LONG len)
{
    BPTR out = Output();

    if (len <= 0)
        return 0;

    (VOID)Flush(out);

    return Write(out, (APTR)buf, len);
}
