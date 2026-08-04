/*
 * toolsock, bsdsocket.library through its published vectors.
 *
 * See toolsock.h for why these are called by hand rather than through the NDK
 * inlines.  Every stub below is written the same way: the arguments go into
 * the registers the ABI names, a6 holds the library base, and the call is a
 * `jsr a6@(-LVO:W)`.  The LVOs are docs/RESEARCH.md 3.2's and the register
 * assignments are src/bsdsocket/bsdsocket_vectors.h's; a disagreement between
 * the two is a bug, and shows up here.
 *
 * Hazard: every stub must declare d1/a0/a1 written.  An AmigaOS library call
 * clobbers d0, d1, a0 and a1, and GCC may assume an input-only operand is left
 * alone, so a stub passing an argument in d1 without declaring d1 written lets
 * GCC keep a value there across the `jsr` and then reuse or spill whatever the
 * library left behind.  This turned IoctlSocket(FIONBIO) into a call with a
 * garbage request code and wedged a test for a day (docs/RESEARCH.md 42).  The
 * `_clob_*` dummies bound to those registers and listed as outputs are the
 * NDK's own idiom, from inline/macros.h.
 *
 * Hazard, the other half: nothing may be CALLED between the first register
 * variable and the `jsr`.  A local register variable lives in its hard
 * register from its initialiser onwards, and GCC does not reload it after a
 * call, so a call in a later initialiser returns having clobbered d0/d1/a0/a1
 * the earlier arguments, and the library is entered with the callee's
 * leftovers.  bind(), connect() and sendto() each computed the sockaddr
 * length that way.  At -Os tool_sock_len() inlines and nothing shows; at -O0
 * it is a real `jsr` and sendto() went to the library with d0 holding 16
 * instead of the descriptor, which the stack answered with EBADF.  Compute
 * anything that needs a call into a plain local FIRST.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

/* ------------------------------------------------------------- fd_set ---- */

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

/* ---------------------------------------------------------- the library --- */

struct Library *tool_socket_open(VOID)
{
    struct Library *base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);

    if (base == NULL)
    {
        if (tool_stack_installed())
            tool_error("the network would not start");
        else
            tool_error("no bsdsocket.library");
        tool_explain_no_stack();
    }

    return base;
}

/* ------------------------------------------------------------- vectors ---- */

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
 * How long a sockaddr is on the wire, from the sockaddr itself.
 *
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
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = (APTR)readfds;
    register APTR            a1  __asm("a1") = (APTR)writefds;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = (APTR)tv;
    register ULONG          *d1  __asm("d1") = NULL;
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
 * The three at the far end of the table.  Register assignment comes from the
 * NDK pragma, not from the C prototype:
 *
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

/* ------------------------------------------------------------- helpers ---- */

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

    /*
     * Through the library rather than a formatter of our own: an IPv6 address
     * only ever gets here from a library that produced it, so the one that
     * knows RFC 5952 is the one that has it.
     */
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

    tool_error("%s: no IPv6 on this machine",
               (LONG)host);
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

BOOL tool_sock_resolve_af(struct Library *base, const char *host, LONG want,
                          ToolAddr *out)
{
    char          unbracketed[TOOL_ADDR_STRLEN];
    ToolAddrInfo  hints;
    ToolAddrInfo *list = NULL;
    ToolAddrInfo *ai;
    ULONG         address = 0;
    BOOL          literal;
    BOOL          got = FALSE;

    host = tool_host_unbracket(host, unbracketed, sizeof(unbracketed));

    if (want != TOOL_AF_INET6 && ami_config_parse_ip(host, &address))
    {
        tool_addr_v4(out, address);
        return TRUE;
    }

    literal = tool_looks_v6(host);

    if (!tool_sock_have_lvo(base, 0x330UL))
    {
        /*
         * No getaddrinfo in this library's table.  A name can still be looked
         * up the old way; an IPv6 literal cannot be used at all.
         */
        if (!literal && want != TOOL_AF_INET6 &&
            tool_resolve_old(base, host, out))
            return TRUE;

        if (literal)
            tool_no_ipv6(base, host);
        else
        {
            tool_error("cannot resolve \"%s\"", (LONG)host);
            tool_explain_resolve(host, AMI_NET_ERR_NONAME);
        }

        return FALSE;
    }

    hints.ai_flags     = literal ? TOOL_AI_NUMERICHOST : 0;
    hints.ai_family    = want;
    hints.ai_socktype  = TOOL_SOCK_STREAM;
    hints.ai_protocol  = 0;
    hints.ai_addrlen   = 0;
    hints.ai_addr      = NULL;
    hints.ai_canonname = NULL;
    hints.ai_next      = NULL;

    if (tool_sock_getaddrinfo(base, host, NULL, &hints, &list) == 0)
    {
        /* The library orders IPv6 first, so the first usable answer wins. */
        for (ai = list; ai != NULL && !got; ai = ai->ai_next)
        {
            if (ai->ai_addr == NULL)
                continue;

            got = tool_sock_addr_get(ai->ai_addr, out);
        }

        tool_sock_freeaddrinfo(base, list);
    }

    if (got)
        return TRUE;

    if (literal)
    {
        tool_no_ipv6(base, host);
        return FALSE;
    }

    tool_error("cannot resolve \"%s\"", (LONG)host);
    tool_explain_resolve(host, AMI_NET_ERR_NONAME);

    return FALSE;
}

BOOL tool_sock_resolve(struct Library *base, const char *host, ToolAddr *out)
{
    return tool_sock_resolve_af(base, host, TOOL_AF_UNSPEC, out);
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

/*
 * The unnamed case gets the number.  The errnos that turn up in bug reports are
 * the ones not in the list below, so printing the number beats a generic "the
 * network refused".
 */
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

VOID tool_sock_fail(struct Library *base, const char *what,
                    const ToolAddr *addr, UWORD port)
{
    LONG err = tool_sock_errno(base);
    char text[TOOL_ADDR_STRLEN];

    tool_addr_text(base, addr, text, sizeof(text));

    tool_error("cannot %s %s port %ld: %s", (LONG)what, (LONG)text,
               (LONG)port, (LONG)tool_sock_errstr(err));
}

/* ------------------------------------------------------------- console ---- */

VOID tool_input_open(ToolInput *in, BOOL want_raw)
{
    in->fh          = Input();
    in->interactive = (IsInteractive(in->fh) != 0) ? TRUE : FALSE;
    in->raw         = FALSE;
    in->eof         = FALSE;

    if (in->interactive && want_raw)
    {
        /*
         * SetMode(fh, 1) is the console's RAW mode: no line editing, no local
         * echo, every keystroke available as it is pressed.  It is a property
         * of the console, not of this process, so every caller must reach
         * tool_input_close() on every path including Ctrl-C or the user's Shell
         * is left unusable.
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
 * Straight to standard output, past dos.library's buffer.
 *
 * These commands print two kinds of thing to the same stream: their own lines
 * through VPrintf(), which dos.library buffers, and socket bytes through
 * Write(), which it does not.  Without the Flush() the second overtakes the
 * first and the transcript comes out interleaved.  Observed.
 */
LONG tool_output_write(const UBYTE *buf, LONG len)
{
    BPTR out = Output();

    if (len <= 0)
        return 0;

    (VOID)Flush(out);

    return Write(out, (APTR)buf, len);
}
