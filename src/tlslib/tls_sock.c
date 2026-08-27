/*
 * tls.library, the published bsdsocket.library vectors.  See tls_sock.h.
 *
 * a0 and a1 are "=r" outputs and not plain inputs on every call that uses
 * them: d0, d1, a0 and a1 are scratch in the AmigaOS ABI, and as inputs the
 * compiler reuses whatever the previous call left in them.  Same rule as the
 * inline stubs in include/aminetxduo/tlslib.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_sock.h"

#ifdef TLSLIB_HOST_TEST

/*
 * The host build of this file is not a simulation of bsdsocket.library: it is
 * the same call sequence against POSIX, so src/tlslib/test/test_tls_transport.c
 * can drive tls_netx.c's transport over a real socketpair.  `base` is ignored
 * here and is the library base on the Amiga.
 */

#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

LONG tls_sock_send(APTR base, LONG fd, const void *buf, LONG len)
{
    (void)base;
    return (LONG)send((int)fd, buf, (size_t)len, 0);
}

LONG tls_sock_recv(APTR base, LONG fd, void *buf, LONG len)
{
    (void)base;
    return (LONG)recv((int)fd, buf, (size_t)len, 0);
}

LONG tls_sock_errno(APTR base)
{
    (void)base;

    switch (errno)
    {
    case EINTR:         return TLS_SOCK_EINTR;
    case EWOULDBLOCK:   return TLS_SOCK_EWOULDBLOCK;
    default:            return errno;
    }
}

LONG tls_sock_getsockopt(APTR base, LONG fd, LONG level, LONG name,
                         void *val, LONG *len)
{
    socklen_t   n;
    int         rc;
    int         host_level = (level == TLS_SOCK_SOL_SOCKET) ? SOL_SOCKET : (int)level;
    int         host_name  = (name  == TLS_SOCK_SO_TYPE)    ? SO_TYPE    : (int)name;

    (void)base;

    if (len == NULL)
        return -1;

    n  = (socklen_t)*len;
    rc = getsockopt((int)fd, host_level, host_name, val, &n);
    *len = (LONG)n;

    return (rc == 0) ? 0 : -1;
}

LONG tls_sock_getpeername(APTR base, LONG fd, void *sa, LONG *len)
{
    socklen_t n;
    int       rc;

    (void)base;

    if (len == NULL)
        return -1;

    n  = (socklen_t)*len;
    rc = getpeername((int)fd, (struct sockaddr *)sa, &n);
    *len = (LONG)n;

    return (rc == 0) ? 0 : -1;
}

LONG tls_sock_wait(APTR base, LONG fd, BOOL write, const TLSSockTimeval *tv)
{
    fd_set          set;
    struct timeval  timeout;
    struct timeval *timeout_ptr = NULL;
    int             rc;

    (void)base;

    if (fd < 0 || fd >= FD_SETSIZE)
        return -1;

    FD_ZERO(&set);
    FD_SET((int)fd, &set);

    if (tv != NULL)
    {
        timeout.tv_sec  = tv->tv_secs;
        timeout.tv_usec = tv->tv_micro;
        timeout_ptr     = &timeout;
    }

    rc = select((int)fd + 1,
                write ? NULL : &set,
                write ? &set : NULL,
                NULL, timeout_ptr);

    return (LONG)rc;
}

BOOL tls_sock_have_lvo(APTR base, ULONG lvo)
{
    (void)base;
    (void)lvo;
    return TRUE;
}

#else /* !TLSLIB_HOST_TEST */

#include <exec/libraries.h>

LONG tls_sock_send(APTR base, LONG fd, const void *buf, LONG len)
{
    register APTR       a6  __asm("a6") = base;
    register LONG       d0  __asm("d0") = fd;
    register CONST_APTR a0  __asm("a0") = (CONST_APTR)buf;
    register LONG       d1  __asm("d1") = len;
    register LONG       d2  __asm("d2") = 0;        /* flags */
    register LONG       res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-66:W)"              /* -0x042 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

LONG tls_sock_recv(APTR base, LONG fd, void *buf, LONG len)
{
    register APTR a6  __asm("a6") = base;
    register LONG d0  __asm("d0") = fd;
    register APTR a0  __asm("a0") = (APTR)buf;
    register LONG d1  __asm("d1") = len;
    register LONG d2  __asm("d2") = 0;              /* flags */
    register LONG res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-78:W)"              /* -0x04e */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

LONG tls_sock_errno(APTR base)
{
    register APTR a6  __asm("a6") = base;
    register LONG res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"             /* -0x0a2 */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

LONG tls_sock_getsockopt(APTR base, LONG fd, LONG level, LONG name,
                         void *val, LONG *len)
{
    register APTR  a6  __asm("a6") = base;
    register LONG  d0  __asm("d0") = fd;
    register LONG  d1  __asm("d1") = level;
    register LONG  d2  __asm("d2") = name;
    register APTR  a0  __asm("a0") = (APTR)val;
    register LONG *a1  __asm("a1") = len;
    register LONG  res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-96:W)"              /* -0x060 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (a1)
                      : "cc", "memory");
    return res;
}

LONG tls_sock_getpeername(APTR base, LONG fd, void *sa, LONG *len)
{
    register APTR  a6  __asm("a6") = base;
    register LONG  d0  __asm("d0") = fd;
    register APTR  a0  __asm("a0") = (APTR)sa;
    register LONG *a1  __asm("a1") = len;
    register LONG  res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-108:W)"             /* -0x06c */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG tls_sock_waitselect(APTR base, LONG nfds, APTR readfds,
                                APTR writefds, const TLSSockTimeval *tv)
{
    register APTR  a6  __asm("a6") = base;
    register LONG  d0  __asm("d0") = nfds;
    register APTR  a0  __asm("a0") = readfds;
    register APTR  a1  __asm("a1") = writefds;
    register APTR  a2  __asm("a2") = NULL;          /* exceptfds */
    register CONST_APTR a3 __asm("a3") = (CONST_APTR)tv;
    register ULONG *d1 __asm("d1") = NULL;          /* signal mask */
    register LONG  res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-126:W)"             /* -0x07e */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return res;
}

LONG tls_sock_wait(APTR base, LONG fd, BOOL write, const TLSSockTimeval *tv)
{
    ULONG set[TLS_SOCK_FD_MAX / 32];
    ULONG words;
    ULONG i;

    if (fd < 0 || fd >= TLS_SOCK_FD_MAX)
        return -1;

    /* Only the words the descriptor reaches are cleared and handed over.
       WaitSelect() reads nfds bits and no more. */
    words = ((ULONG)fd / 32UL) + 1UL;
    for (i = 0; i < words; i++)
        set[i] = 0;
    set[(ULONG)fd / 32UL] = 1UL << ((ULONG)fd % 32UL);

    return tls_sock_waitselect(base, fd + 1,
                               write ? NULL : (APTR)set,
                               write ? (APTR)set : NULL,
                               tv);
}

BOOL tls_sock_have_lvo(APTR base, ULONG lvo)
{
    const struct Library *lib = (const struct Library *)base;

    if (lib == NULL)
        return FALSE;

    /* MakeLibrary() stops at the table's (APTR)-1 terminator, so a library
       that does not implement a vector does not have the negative size to
       hold it either. */
    return (BOOL)((ULONG)lib->lib_NegSize >= lvo);
}

#endif /* TLSLIB_HOST_TEST */

/* ----------------------------------------------------- the descriptor --- */

BOOL tls_sock_is_connected_tcp(APTR base, LONG fd, UWORD *port, UWORD *family)
{
    /* Big enough for a sockaddr_in6 in either spelling; nothing here reads
       past the first four bytes plus, for AF_INET6, none at all. */
    UBYTE sa[32];
    LONG  length;
    LONG  type = 0;
    LONG  i;

    if (base == NULL || fd < 0)
        return FALSE;

    length = (LONG)sizeof(type);
    if (tls_sock_getsockopt(base, fd, TLS_SOCK_SOL_SOCKET, TLS_SOCK_SO_TYPE,
                            &type, &length) != 0)
        return FALSE;
    if (type != TLS_SOCK_SOCK_STREAM)
        return FALSE;

    for (i = 0; i < (LONG)sizeof(sa); i++)
        sa[i] = 0;

    /* getpeername() fails on a descriptor that was never connected, which is
       the other half of what the private context used to check. */
    length = (LONG)sizeof(sa);
    if (tls_sock_getpeername(base, fd, sa, &length) != 0)
        return FALSE;
    if (length < 4)
        return FALSE;

    /*
     * Two spellings of the same sixteen bytes.  A 4.4BSD sockaddr leads with
     * sa_len and then sa_family; this NDK's sockaddr_in6 has no length byte
     * and leads with the family.  A sockaddr_in's byte 0 is 16 and never 2 or
     * 23, so the family is whichever of the first two bytes is one of those.
     */
    if (family != NULL || port != NULL)
    {
        UWORD fam = sa[1];

        if (sa[0] == TLS_SOCK_AF_INET || sa[0] == TLS_SOCK_AF_INET6)
            fam = sa[0];

        if (family != NULL)
            *family = fam;
        if (port != NULL)
            *port = (UWORD)(((UWORD)sa[2] << 8) | (UWORD)sa[3]);
    }

    return TRUE;
}
