/*
 * RpcProbe: the ONE request/response an RPC client makes before anything else,
 * done the way an RPC client does it, through the published LVOs and nothing
 * of ours.
 *
 * WHY IT EXISTS.  bifat reported against 0.26.5 that ch_nfsmount fails with
 * `RPC: Port mapper failure - Unable to receive` on this stack while the same
 * setup works on AmiTCP4, AmiTCP_NG and Roadshow.  There is no RPC, portmap or
 * NFS coverage anywhere in this tree, so nothing here could have caught it and
 * nothing here can currently reproduce it.
 *
 * WHAT AN RPC CLIENT DOES THAT A DNS CLIENT DOES NOT, which is the whole point
 * of the two arms below:
 *
 *   resv   binds a RESERVED local port first -- bindresvport() walks down from
 *          1023 -- because an NFS server may refuse a request that did not
 *          come from one.  The reply then has to come back to THAT port.
 *   ephem  binds port 0 and lets the stack choose, which is what the resolver
 *          does and what this tree already exercises.
 *
 * THE CONTROL ARM IS NOT DECORATION.  If `resv` fails and `ephem` passes, the
 * defect is in binding or demultiplexing a low local port.  If BOTH fail, it
 * is the receive path for a bound, unconnected UDP socket.  If BOTH pass, this
 * stack is not where the mount is failing and the next place to look is what
 * ch_nfs does with the reply -- a pass here is a real answer, not a shrug.
 *
 * The payload is a genuine PMAPPROC_GETPORT call (RFC 1057), so this also
 * works unchanged against a real rpcbind if one is ever put on the peer.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

typedef struct ProbeAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} ProbeAddr;

#define P_AF_INET       2
#define P_SOCK_DGRAM    2

/* PMAPPROG/PMAPVERS and the call this makes, from RFC 1057 appendix A. */
#define PMAP_PROG       100000UL
#define PMAP_VERS       2UL
#define PMAP_GETPORT    3UL
#define NFS_PROG        100003UL
#define NFS_VERS        3UL
#define IPPROTO_UDP_    17UL

/* ------------------------------------------------------------- vectors ---- */

static LONG p_socket(struct Library *base, LONG domain, LONG type, LONG proto)
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

static LONG p_bind(struct Library *base, LONG s, const ProbeAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(*sa);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_sendto(struct Library *base, LONG s, const void *buf, LONG len,
                     const ProbeAddr *to)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register CONST_APTR      a1  __asm("a1") = (CONST_APTR)to;
    register LONG            d3  __asm("d3") = (LONG)sizeof(*to);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-60:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (d3)
                      : "cc", "memory");
    return res;
}

static LONG p_recvfrom(struct Library *base, LONG s, void *buf, LONG len,
                       ProbeAddr *from, LONG *fromlen)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register APTR            a1  __asm("a1") = (APTR)from;
    register APTR            a2  __asm("a2") = (APTR)fromlen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");
    register LONG _clob_a2 __asm("a2");

    __asm __volatile ("jsr a6@(-72:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (a2)
                      : "cc", "memory");
    return res;
}

static LONG p_getsockname(struct Library *base, LONG s, ProbeAddr *sa,
                          LONG *len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)sa;
    register APTR            a1  __asm("a1") = (APTR)len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-102:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

/* timeval as bsdsocket takes it: two LONGs, seconds then microseconds. */
typedef struct ProbeTime { LONG tv_sec; LONG tv_usec; } ProbeTime;

static LONG p_waitselect(struct Library *base, LONG nfds, ULONG *readfds,
                         ProbeTime *tv)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = (APTR)readfds;
    register APTR            a1  __asm("a1") = NULL;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = (APTR)tv;
    register LONG            d1  __asm("d1") = 0;   /* no signal mask */
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");
    register LONG _clob_a2 __asm("a2");
    register LONG _clob_a3 __asm("a3");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2), "=r" (_clob_a3)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return res;
}

static LONG p_close(struct Library *base, LONG s)
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

static LONG p_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static ULONG p_inet_addr(struct Library *base, const char *cp)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)cp;
    register ULONG           res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-180:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

/* ----------------------------------------------------------- the payload -- */

static UBYTE  call_buf[56];
static UBYTE  reply_buf[512];
static ULONG  call_xid;

static VOID put32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v >> 24);
    p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);
    p[3] = (UBYTE)v;
}

static ULONG get32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8)  | (ULONG)p[3];
}

/* A PMAPPROC_GETPORT call asking where NFS v3 over UDP lives. */
static VOID build_call(ULONG xid)
{
    put32(&call_buf[0],  xid);
    put32(&call_buf[4],  0UL);            /* msg_type = CALL              */
    put32(&call_buf[8],  2UL);            /* rpcvers                      */
    put32(&call_buf[12], PMAP_PROG);
    put32(&call_buf[16], PMAP_VERS);
    put32(&call_buf[20], PMAP_GETPORT);
    put32(&call_buf[24], 0UL);            /* cred flavour AUTH_NULL       */
    put32(&call_buf[28], 0UL);            /* cred length                  */
    put32(&call_buf[32], 0UL);            /* verf flavour AUTH_NULL       */
    put32(&call_buf[36], 0UL);            /* verf length                  */
    put32(&call_buf[40], NFS_PROG);
    put32(&call_buf[44], NFS_VERS);
    put32(&call_buf[48], IPPROTO_UDP_);
    put32(&call_buf[52], 0UL);            /* port, 0 in a query           */
}

/* --------------------------------------------------------------- an arm --- */

/* `resv` walks down from 1023 the way bindresvport() does; `ephem` asks for 0
   and takes what it is given. */
static LONG arm(struct Library *sb, const char *tag, BOOL reserved,
                ULONG dest, UWORD dport)
{
    ProbeAddr sa, to, from;
    ULONG     readfds[8];
    ProbeTime tv;
    LONG      s, rc, n, i, fromlen, namelen;
    LONG      bound = -1;
    LONG      ok = 0;

    for (i = 0; i < (LONG)sizeof(sa.sin_zero); i++)
        sa.sin_zero[i] = 0;

    s = p_socket(sb, P_AF_INET, P_SOCK_DGRAM, 0);
    Printf((CONST_STRPTR)"%s_socket=%ld\n", (LONG)tag, s);
    if (s < 0)
    {
        Printf((CONST_STRPTR)"%s_socket_errno=%ld\n", (LONG)tag, p_errno(sb));
        Printf((CONST_STRPTR)"%s_RESULT=FAIL\n", (LONG)tag);
        return 0;
    }

    if (reserved)
    {
        UWORD port;

        for (port = 1023; port >= 900; port--)
        {
            sa.sin_len    = (UBYTE)sizeof(sa);
            sa.sin_family = P_AF_INET;
            sa.sin_port   = port;
            sa.sin_addr   = 0UL;            /* INADDR_ANY */

            rc = p_bind(sb, s, &sa);
            if (rc == 0)
            {
                bound = (LONG)port;
                break;
            }
        }
        Printf((CONST_STRPTR)"%s_bind_reserved=%ld\n", (LONG)tag, bound);
        if (bound < 0)
        {
            Printf((CONST_STRPTR)"%s_bind_errno=%ld\n", (LONG)tag,
                   p_errno(sb));
            Printf((CONST_STRPTR)"%s_RESULT=FAIL\n", (LONG)tag);
            (VOID)p_close(sb, s);
            return 0;
        }
    }
    else
    {
        sa.sin_len    = (UBYTE)sizeof(sa);
        sa.sin_family = P_AF_INET;
        sa.sin_port   = 0;
        sa.sin_addr   = 0UL;

        rc = p_bind(sb, s, &sa);
        Printf((CONST_STRPTR)"%s_bind_rc=%ld\n", (LONG)tag, rc);
        if (rc != 0)
        {
            Printf((CONST_STRPTR)"%s_bind_errno=%ld\n", (LONG)tag,
                   p_errno(sb));
            Printf((CONST_STRPTR)"%s_RESULT=FAIL\n", (LONG)tag);
            (VOID)p_close(sb, s);
            return 0;
        }
    }

    /* What the stack thinks the socket is bound to, which is the port the
       reply has to come back to. */
    namelen = (LONG)sizeof(sa);
    if (p_getsockname(sb, s, &sa, &namelen) == 0)
        Printf((CONST_STRPTR)"%s_local_port=%ld\n", (LONG)tag,
               (LONG)sa.sin_port);
    else
        Printf((CONST_STRPTR)"%s_local_port=unknown\n", (LONG)tag);

    for (i = 0; i < (LONG)sizeof(to.sin_zero); i++)
        to.sin_zero[i] = 0;
    to.sin_len    = (UBYTE)sizeof(to);
    to.sin_family = P_AF_INET;
    to.sin_port   = dport;
    to.sin_addr   = dest;

    call_xid += 1UL;
    build_call(call_xid);

    n = p_sendto(sb, s, call_buf, (LONG)sizeof(call_buf), &to);
    Printf((CONST_STRPTR)"%s_sendto=%ld\n", (LONG)tag, n);
    if (n != (LONG)sizeof(call_buf))
    {
        Printf((CONST_STRPTR)"%s_sendto_errno=%ld\n", (LONG)tag, p_errno(sb));
        Printf((CONST_STRPTR)"%s_RESULT=FAIL\n", (LONG)tag);
        (VOID)p_close(sb, s);
        return 0;
    }

    for (i = 0; i < 8; i++)
        readfds[i] = 0UL;
    readfds[s / 32] |= 1UL << (s % 32);

    tv.tv_sec  = 5;
    tv.tv_usec = 0;

    rc = p_waitselect(sb, s + 1, readfds, &tv);
    Printf((CONST_STRPTR)"%s_waitselect=%ld\n", (LONG)tag, rc);
    if (rc <= 0)
    {
        Printf((CONST_STRPTR)"%s_waitselect_errno=%ld\n", (LONG)tag,
               (LONG)((rc < 0) ? p_errno(sb) : 0));
        Printf((CONST_STRPTR)"%s_error=nothing became readable in 5 s: the "
                             "reply never reached the socket\n", (LONG)tag);
        Printf((CONST_STRPTR)"%s_RESULT=FAIL\n", (LONG)tag);
        (VOID)p_close(sb, s);
        return 0;
    }

    fromlen = (LONG)sizeof(from);
    n = p_recvfrom(sb, s, reply_buf, (LONG)sizeof(reply_buf), &from, &fromlen);
    Printf((CONST_STRPTR)"%s_recvfrom=%ld\n", (LONG)tag, n);
    if (n < 0)
    {
        Printf((CONST_STRPTR)"%s_recvfrom_errno=%ld\n", (LONG)tag,
               p_errno(sb));
        Printf((CONST_STRPTR)"%s_RESULT=FAIL\n", (LONG)tag);
        (VOID)p_close(sb, s);
        return 0;
    }

    Printf((CONST_STRPTR)"%s_from_port=%ld\n", (LONG)tag, (LONG)from.sin_port);

    if (n >= 24)
    {
        ULONG xid   = get32(&reply_buf[0]);
        ULONG mtype = get32(&reply_buf[4]);

        Printf((CONST_STRPTR)"%s_reply_xid_match=%ld\n", (LONG)tag,
               (LONG)((xid == call_xid) ? 1 : 0));
        Printf((CONST_STRPTR)"%s_reply_is_reply=%ld\n", (LONG)tag,
               (LONG)((mtype == 1UL) ? 1 : 0));
        if (xid == call_xid && mtype == 1UL)
            ok = 1;
    }
    else
    {
        Printf((CONST_STRPTR)"%s_error=the reply is %ld bytes, too short for "
                             "an RPC reply header\n", (LONG)tag, n);
    }

    Printf((CONST_STRPTR)"%s_RESULT=%s\n", (LONG)tag,
           ok ? (LONG)"PASS" : (LONG)"FAIL");

    (VOID)p_close(sb, s);
    return ok;
}

int main(int argc, char **argv)
{
    struct Library *sb;
    ULONG           dest;
    LONG            dport = 111;
    LONG            resv_ok, ephem_ok;

    if (argc < 2)
    {
        Printf((CONST_STRPTR)"usage: RpcProbe <peer-address> [port]\n");
        return RETURN_FAIL;
    }

    sb = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (sb == NULL)
    {
        Printf((CONST_STRPTR)"no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    dest = p_inet_addr(sb, argv[1]);
    if (dest == 0xffffffffUL)
    {
        Printf((CONST_STRPTR)"peer=%s is not a dotted quad\n", (LONG)argv[1]);
        CloseLibrary(sb);
        return RETURN_FAIL;
    }

    if (argc >= 3)
    {
        const char *p = argv[2];
        LONG        v = 0;

        while (*p >= '0' && *p <= '9')
            v = (v * 10) + (*p++ - '0');
        if (v > 0 && v < 65536)
            dport = v;
    }

    Printf((CONST_STRPTR)"peer=%s\n", (LONG)argv[1]);
    Printf((CONST_STRPTR)"port=%ld\n", dport);

    call_xid = 0x52504331UL;    /* "RPC1" */

    ephem_ok = arm(sb, "ephem", FALSE, dest, (UWORD)dport);
    Delay(25);
    resv_ok  = arm(sb, "resv",  TRUE,  dest, (UWORD)dport);

    /*
     * THE TWO ARMS TOGETHER ARE THE ANSWER, and each combination names a
     * different place to look.  Said here so a reader of the log does not have
     * to reconstruct it.
     */
    if (resv_ok && ephem_ok)
        Printf((CONST_STRPTR)"verdict=both arms carried a portmap "
                             "request and its reply; this stack is not where "
                             "the mount fails\n");
    else if (ephem_ok && !resv_ok)
        Printf((CONST_STRPTR)"verdict=an ephemeral source port works and a "
                             "RESERVED one does not: bind or demultiplex of a "
                             "low local port\n");
    else if (!ephem_ok && !resv_ok)
        Printf((CONST_STRPTR)"verdict=neither arm received: the receive path "
                             "for a bound unconnected UDP socket\n");
    else
        Printf((CONST_STRPTR)"verdict=the reserved arm works and the "
                             "ephemeral one does not, which is backwards and "
                             "means the peer answered only one of them\n");

    Printf((CONST_STRPTR)"RESULT=%s\n",
           (resv_ok && ephem_ok) ? (LONG)"PASS" : (LONG)"FAIL");

    CloseLibrary(sb);
    return (resv_ok && ephem_ok) ? RETURN_OK : RETURN_WARN;
}
