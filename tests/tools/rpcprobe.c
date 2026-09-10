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
 * of the three arms below:
 *
 *   ephem  binds port 0 and lets the stack choose, which is what the resolver
 *          does and what this tree already exercises.  The control.
 *   resv   binds a RESERVED local port first -- bindresvport() walks down from
 *          1023 -- because an NFS server may refuse a request that did not
 *          come from one.  The reply then has to come back to THAT port.
 *   conn   does that and then connect()s the datagram socket, which is the
 *          other shape an RPC client takes; the receive filter is different.
 *
 * AND EACH ARM EXCHANGES TWICE ON ONE SOCKET.  An RPC client retries on the
 * socket it already has, so a stack that carries the first reply and loses the
 * second fails a mount while passing anything that asks once.
 *
 * THE CONTROL ARM IS NOT DECORATION.  ephem passing while resv fails is bind
 * or demultiplex of a low local port; an unconnected arm passing while conn
 * fails is the connected-UDP receive filter; none passing is the receive path
 * itself.  All passing is a real answer and not a shrug: it says the mount is
 * not failing in this stack and the next place to look is what ch_nfs does
 * with the reply.
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
                         ProbeTime *tv, ULONG *sigs)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = (APTR)readfds;
    register APTR            a1  __asm("a1") = NULL;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = (APTR)tv;
    register APTR            d1  __asm("d1") = (APTR)sigs;
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

static LONG p_connect(struct Library *base, LONG s, const ProbeAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(*sa);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_send(struct Library *base, LONG s, const void *buf, LONG len)
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

static LONG p_recv(struct Library *base, LONG s, void *buf, LONG len)
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

#define ARM_EPHEM  0    /* bind 0, sendto/recvfrom -- what a resolver does   */
#define ARM_RESV   1    /* bindresvport, sendto/recvfrom -- what RPC does    */
#define ARM_CONN   2    /* bindresvport + connect(), send/recv               */
#define ARM_SIG    3    /* as ARM_CONN, but WaitSelect gets a SIGNAL MASK    */

/*
 * SIGBREAKF_CTRL_C.  AmiTCP's net.lib hands WaitSelect() a signal mask so a
 * hung RPC call can be broken with Ctrl-C, and that is the one argument a
 * probe written from the BSD side never passes.  A WaitSelect() that returned
 * early because a mask was present would look, to an RPC client, exactly like
 * a reply that never came: "Unable to receive".
 */
#define P_SIGBREAKF_CTRL_C  0x1000UL

/*
 * ONE call and its reply on an already-open socket.  Returns 1 when the reply
 * came back and matched.  `round` is only for the key names, so a retry is
 * distinguishable from the first try in the log.
 */
static LONG exchange(struct Library *sb, const char *tag, LONG round, LONG s,
                     LONG mode, const ProbeAddr *to)
{
    ProbeAddr from;
    ULONG     readfds[8];
    ProbeTime tv;
    ULONG     sigs;
    LONG      rc, n, i, fromlen;

    call_xid += 1UL;
    build_call(call_xid);

    if (mode == ARM_CONN || mode == ARM_SIG)
        n = p_send(sb, s, call_buf, (LONG)sizeof(call_buf));
    else
        n = p_sendto(sb, s, call_buf, (LONG)sizeof(call_buf), to);

    Printf((CONST_STRPTR)"%s_r%ld_send=%ld\n", (LONG)tag, round, n);
    if (n != (LONG)sizeof(call_buf))
    {
        Printf((CONST_STRPTR)"%s_r%ld_send_errno=%ld\n", (LONG)tag, round,
               p_errno(sb));
        return 0;
    }

    for (i = 0; i < 8; i++)
        readfds[i] = 0UL;
    readfds[s / 32] |= 1UL << (s % 32);

    tv.tv_sec  = 5;
    tv.tv_usec = 0;

    sigs = (mode == ARM_SIG) ? P_SIGBREAKF_CTRL_C : 0UL;
    rc = p_waitselect(sb, s + 1, readfds, &tv,
                      (mode == ARM_SIG) ? &sigs : NULL);
    Printf((CONST_STRPTR)"%s_r%ld_waitselect=%ld\n", (LONG)tag, round, rc);
    if (mode == ARM_SIG)
        Printf((CONST_STRPTR)"%s_r%ld_signals_out=%ld\n", (LONG)tag, round,
               (LONG)sigs);
    if (rc <= 0)
    {
        Printf((CONST_STRPTR)"%s_r%ld_waitselect_errno=%ld\n", (LONG)tag,
               round, (LONG)((rc < 0) ? p_errno(sb) : 0));
        Printf((CONST_STRPTR)"%s_r%ld_error=nothing became readable in 5 s\n",
               (LONG)tag, round);
        return 0;
    }

    fromlen = (LONG)sizeof(from);
    from.sin_port = 0;
    if (mode == ARM_CONN || mode == ARM_SIG)
        n = p_recv(sb, s, reply_buf, (LONG)sizeof(reply_buf));
    else
        n = p_recvfrom(sb, s, reply_buf, (LONG)sizeof(reply_buf), &from,
                       &fromlen);

    Printf((CONST_STRPTR)"%s_r%ld_recv=%ld\n", (LONG)tag, round, n);
    if (n < 0)
    {
        Printf((CONST_STRPTR)"%s_r%ld_recv_errno=%ld\n", (LONG)tag, round,
               p_errno(sb));
        return 0;
    }

    if (mode != ARM_CONN && mode != ARM_SIG)
        Printf((CONST_STRPTR)"%s_r%ld_from_port=%ld\n", (LONG)tag, round,
               (LONG)from.sin_port);

    if (n < 24)
    {
        Printf((CONST_STRPTR)"%s_r%ld_error=reply is %ld bytes, too short\n",
               (LONG)tag, round, n);
        return 0;
    }

    {
        ULONG xid   = get32(&reply_buf[0]);
        ULONG mtype = get32(&reply_buf[4]);

        Printf((CONST_STRPTR)"%s_r%ld_xid_match=%ld\n", (LONG)tag, round,
               (LONG)((xid == call_xid) ? 1 : 0));
        return (LONG)((xid == call_xid && mtype == 1UL) ? 1 : 0);
    }
}

/*
 * TWO EXCHANGES ON ONE SOCKET, not one.  An RPC client retries on the socket
 * it already has -- that is what a timeout does -- so a stack that carries the
 * first reply and loses the second fails a mount while passing any test that
 * asks once.  Both rounds must land for the arm to pass.
 */
static LONG arm(struct Library *sb, const char *tag, LONG mode, ULONG dest,
                UWORD dport)
{
    ProbeAddr sa, to;
    LONG      s, rc, i;
    LONG      bound = -1;
    LONG      namelen;
    LONG      r1, r2;

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

    if (mode == ARM_EPHEM)
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
    else
    {
        UWORD port;

        for (port = 1023; port >= 900; port--)
        {
            sa.sin_len    = (UBYTE)sizeof(sa);
            sa.sin_family = P_AF_INET;
            sa.sin_port   = port;
            sa.sin_addr   = 0UL;

            if (p_bind(sb, s, &sa) == 0)
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

    namelen = (LONG)sizeof(sa);
    if (p_getsockname(sb, s, &sa, &namelen) == 0)
        Printf((CONST_STRPTR)"%s_local_port=%ld\n", (LONG)tag,
               (LONG)sa.sin_port);

    for (i = 0; i < (LONG)sizeof(to.sin_zero); i++)
        to.sin_zero[i] = 0;
    to.sin_len    = (UBYTE)sizeof(to);
    to.sin_family = P_AF_INET;
    to.sin_port   = dport;
    to.sin_addr   = dest;

    if (mode == ARM_CONN || mode == ARM_SIG)
    {
        rc = p_connect(sb, s, &to);
        Printf((CONST_STRPTR)"%s_connect_rc=%ld\n", (LONG)tag, rc);
        if (rc != 0)
        {
            Printf((CONST_STRPTR)"%s_connect_errno=%ld\n", (LONG)tag,
                   p_errno(sb));
            Printf((CONST_STRPTR)"%s_RESULT=FAIL\n", (LONG)tag);
            (VOID)p_close(sb, s);
            return 0;
        }
    }

    r1 = exchange(sb, tag, 1, s, mode, &to);
    r2 = exchange(sb, tag, 2, s, mode, &to);

    Printf((CONST_STRPTR)"%s_RESULT=%s\n", (LONG)tag,
           (r1 && r2) ? (LONG)"PASS" : (LONG)"FAIL");

    (VOID)p_close(sb, s);
    return (LONG)((r1 && r2) ? 1 : 0);
}

int main(int argc, char **argv)
{
    struct Library *sb;
    ULONG           dest;
    LONG            dport = 111;
    LONG            resv_ok, ephem_ok, conn_ok, sig_ok;

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

    ephem_ok = arm(sb, "ephem", ARM_EPHEM, dest, (UWORD)dport);
    Delay(25);
    resv_ok  = arm(sb, "resv",  ARM_RESV,  dest, (UWORD)dport);
    Delay(25);
    conn_ok  = arm(sb, "conn",  ARM_CONN,  dest, (UWORD)dport);
    Delay(25);
    sig_ok   = arm(sb, "sig",   ARM_SIG,   dest, (UWORD)dport);

    /*
     * EACH COMBINATION NAMES A DIFFERENT PLACE TO LOOK, said here so a reader
     * of the log does not have to reconstruct it.
     */
    if (ephem_ok && resv_ok && conn_ok && sig_ok)
        Printf((CONST_STRPTR)"verdict=all four arms carried a portmap "
                             "request and its reply, twice each on one "
                             "socket, with and without a WaitSelect signal "
                             "mask; this stack is not where the mount "
                             "fails\n");
    else if (ephem_ok && !resv_ok)
        Printf((CONST_STRPTR)"verdict=an ephemeral source port works and a "
                             "RESERVED one does not: bind or demultiplex of "
                             "a low local port\n");
    else if (conn_ok && !sig_ok)
        Printf((CONST_STRPTR)"verdict=the same socket works WITHOUT a "
                             "WaitSelect signal mask and fails with one: "
                             "the signal path in WaitSelect()\n");
    else if ((ephem_ok || resv_ok) && !conn_ok)
        Printf((CONST_STRPTR)"verdict=an unconnected socket works and a "
                             "connect()ed one does not: the connected-UDP "
                             "receive filter\n");
    else if (!ephem_ok && !resv_ok && !conn_ok && !sig_ok)
        Printf((CONST_STRPTR)"verdict=no arm received: the receive path for "
                             "a bound UDP socket\n");
    else
        Printf((CONST_STRPTR)"verdict=a mixed result; read the per-round "
                             "keys above, r1 against r2\n");

    CloseLibrary(sb);
    return (resv_ok && ephem_ok && conn_ok && sig_ok) ? RETURN_OK
                                                      : RETURN_WARN;
}
