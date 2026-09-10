/*
 * NfsProbe -- the RPC an NFS client actually makes, credentials and all.
 *
 * WHAT THIS COVERS THAT NOTHING ELSE DOES.  tests/tools/rpcprobe.c asks
 * whether a portmap reply reaches a bound UDP socket: the transport half.
 * This asks the other half -- whether this stack can produce and be
 * understood through a real NFS call sequence:
 *
 *   usergroup.library  ug_SetupContextTagList, geteuid, getegid, getgroups
 *                      and getpwnam.  Every RPC an NFS client sends carries
 *                      AUTH_UNIX credentials -- uid, gid and the
 *                      supplementary group list -- and they come from here.
 *                      Nothing else in this tree makes a guest produce them.
 *                      0.26.5 could not get credentials out of this library
 *                      for a task that had not opened it, which is what
 *                      ch_nfsc does.
 *   bsdsocket.library  a RESERVED source port (an NFS server refuses a call
 *                      from a port above 1023), then GETPORT, MNT, LOOKUP and
 *                      READ, each a real XDR-encoded exchange.
 *
 * AND THE BYTES ARE COMPARED.  A mount that returns a file handle proves the
 * call reached a server; only reading the file back and matching its content
 * proves the reply was decoded.  The peer serves a known string and this
 * checks it byte for byte.
 *
 * tests/tools/nfspeer.py is the server and checks the credentials from its
 * side, so a uid this stack gets wrong fails at the peer as well as here.
 *
 * NFS version 2 (RFC 1094) rather than 3: fixed 32-byte file handles make the
 * XDR small enough to read, and the credential path -- the point -- is
 * identical in both.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/macros.h>

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




/* ----------------------------------------------------------- the payload -- */

/* ------------------------------------------------- usergroup.library ------ */
/*
 * Offsets from the library's own vector table; tests/libraries/library_test.c
 * carries the same ones and is the file that proved them against a real
 * ch_nfsc call sequence.
 */
#define UGT_ERRNOLPTR   0x80000004UL
#define UGT_INTRMASK    0x80000010UL

struct UgPasswd                 /* only the fields this reads */
{
    char  *pw_name;
    char  *pw_passwd;
    ULONG  pw_uid;
    ULONG  pw_gid;
};

static LONG ug_setup_context(struct Library *base, STRPTR name,
                             struct TagItem *tags)
{
    return LP2(0x1e, LONG, ug_setup_context,
               STRPTR, name, a0, struct TagItem *, tags, a1, , base);
}

static LONG ug_geteuid(struct Library *base)
{
    return LP0(0x36, LONG, ug_geteuid, , base);
}

static LONG ug_getegid(struct Library *base)
{
    return LP0(0x4e, LONG, ug_getegid, , base);
}

static LONG ug_getgroups(struct Library *base, LONG count, LONG *groups)
{
    return LP2(0x60, LONG, ug_getgroups,
               LONG, count, d0, LONG *, groups, a1, , base);
}

static struct UgPasswd *ug_getpwnam(struct Library *base, STRPTR name)
{
    return LP1(0x72, struct UgPasswd *, ug_getpwnam,
               STRPTR, name, a1, , base);
}

/* ------------------------------------------------------------- XDR -------- */

#define MNT_PROG        100005UL
#define MNT_VERS        1UL
#define MNTPROC_MNT     1UL
#define NFS2_PROG       100003UL
#define NFS2_VERS       2UL
#define NFSPROC_LOOKUP  4UL
#define NFSPROC_READ    6UL
#define AUTH_UNIX_      1UL
#define FHSIZE          32

#define CALL_MAX        1024
#define REPLY_MAX       2048

static UBYTE  call_buf[CALL_MAX];
static UBYTE  reply_buf[REPLY_MAX];
static ULONG  call_len;
static ULONG  call_xid;

static VOID put32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v >> 24); p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);  p[3] = (UBYTE)v;
}

static ULONG get32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8)  | (ULONG)p[3];
}

static VOID xdr_put32(ULONG v)
{
    if (call_len + 4UL <= CALL_MAX) { put32(&call_buf[call_len], v); }
    call_len += 4UL;
}

static ULONG xdr_pad(ULONG n) { return (4UL - (n % 4UL)) % 4UL; }

static VOID xdr_put_bytes(const UBYTE *b, ULONG n)
{
    ULONG i, pad = xdr_pad(n);
    for (i = 0; i < n; i++)
        if (call_len + i < CALL_MAX) call_buf[call_len + i] = b[i];
    call_len += n;
    for (i = 0; i < pad; i++)
        if (call_len + i < CALL_MAX) call_buf[call_len + i] = 0;
    call_len += pad;
}

static ULONG cstr_len(const char *s)
{
    ULONG n = 0; while (s[n] != '\0') n++; return n;
}

static VOID xdr_put_string(const char *s)
{
    ULONG n = cstr_len(s);
    xdr_put32(n);
    xdr_put_bytes((const UBYTE *)s, n);
}

/*
 * THE CREDENTIALS, WHICH ARE THE POINT.  RFC 1057 section 9.2: stamp, machine
 * name, uid, gid, then the supplementary list.  An NFS server authorises on
 * these and on nothing else in the packet.
 */
static LONG cred_uid, cred_gid, cred_ngroups;
static LONG cred_groups[16];
static char cred_machine[32] = "aminetxduo";

static VOID build_call(ULONG prog, ULONG vers, ULONG proc, LONG auth_unix)
{
    ULONG i, namelen, credlen;

    call_len = 0;
    xdr_put32(++call_xid);
    xdr_put32(0UL);                 /* CALL */
    xdr_put32(2UL);                 /* RPC version 2 */
    xdr_put32(prog);
    xdr_put32(vers);
    xdr_put32(proc);

    if (!auth_unix)
    {
        xdr_put32(0UL); xdr_put32(0UL);         /* cred AUTH_NULL */
    }
    else
    {
        namelen = cstr_len(cred_machine);
        credlen = 4UL + 4UL + namelen + xdr_pad(namelen)
                  + 4UL + 4UL + 4UL + (ULONG)cred_ngroups * 4UL;
        xdr_put32(AUTH_UNIX_);
        xdr_put32(credlen);
        xdr_put32(0UL);                          /* stamp */
        xdr_put_string(cred_machine);
        xdr_put32((ULONG)cred_uid);
        xdr_put32((ULONG)cred_gid);
        xdr_put32((ULONG)cred_ngroups);
        for (i = 0; i < (ULONG)cred_ngroups; i++)
            xdr_put32((ULONG)cred_groups[i]);
    }

    xdr_put32(0UL); xdr_put32(0UL);              /* verf AUTH_NULL */
}

/*
 * One exchange, retried once.  UDP RPC has no delivery guarantee and a client
 * that gives up on the first timeout reports a stack defect that is a dropped
 * datagram.  Returns the offset of the reply body, or -1.
 */
static LONG exchange(struct Library *sb, LONG s, const char *what,
                     ULONG dest, UWORD dport)
{
    ProbeAddr to;
    ProbeAddr from;
    LONG      fromlen = (LONG)sizeof(from);
    LONG      n, try_;
    ULONG     rfds, xid_sent = call_xid;
    ProbeTime tv;

    for (try_ = 0; try_ < 2; try_++)
    {
        to.sin_len = (UBYTE)sizeof(to); to.sin_family = P_AF_INET;
        to.sin_port = dport; to.sin_addr = dest;
        for (n = 0; n < 8; n++) to.sin_zero[n] = 0;

        if (p_sendto(sb, s, call_buf, (LONG)call_len, &to) < 0)
        {
            Printf((CONST_STRPTR)"  %s: sendto errno=%ld\n", (LONG)what, p_errno(sb));
            continue;
        }

        rfds = 1UL << s;
        tv.tv_sec = 5; tv.tv_usec = 0;
        if (p_waitselect(sb, s + 1, &rfds, &tv, NULL) <= 0)
        {
            Printf((CONST_STRPTR)"  %s: no reply within 5s (try %ld)\n",
                   (LONG)what, (LONG)(try_ + 1));
            continue;
        }

        n = p_recvfrom(sb, s, reply_buf, (LONG)sizeof(reply_buf), &from,
                       &fromlen);
        if (n < 24)
        {
            Printf((CONST_STRPTR)"  %s: short reply %ld\n", (LONG)what, n);
            continue;
        }
        if (get32(&reply_buf[0]) != xid_sent)
        {
            Printf((CONST_STRPTR)"  %s: xid mismatch\n", (LONG)what);
            continue;
        }
        if (get32(&reply_buf[4]) != 1UL)   /* REPLY */
        {
            Printf((CONST_STRPTR)"  %s: not a reply\n", (LONG)what);
            return -1;
        }
        if (get32(&reply_buf[8]) != 0UL)   /* MSG_ACCEPTED */
        {
            Printf((CONST_STRPTR)"  %s: rejected by the server\n", (LONG)what);
            return -1;
        }
        /* accepted: verf flavour, verf len, then accept_stat */
        {
            ULONG vlen = get32(&reply_buf[16]);
            ULONG off  = 20UL + vlen + xdr_pad(vlen);
            if ((ULONG)n < off + 4UL) { Printf((CONST_STRPTR)"  %s: truncated\n", (LONG)what); return -1; }
            if (get32(&reply_buf[off]) != 0UL)
            {
                Printf((CONST_STRPTR)"  %s: accept_stat=%lu\n", (LONG)what,
                       get32(&reply_buf[off]));
                return -1;
            }
            return (LONG)(off + 4UL);
        }
    }
    return -1;
}

static LONG atol_(const char *s)
{
    LONG v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}


/* ------------------------------------------------------------- main ------- */

static const char *g_content = "AmiNetXDuo NFS payload, 0123456789\n";

int main(int argc, char **argv)
{
    struct Library *sb = NULL;
    struct Library *ug = NULL;
    struct TagItem  tags[3];
    struct UgPasswd *pw;
    ProbeAddr       me;
    LONG            melen = (LONG)sizeof(me);
    LONG            s = -1, body, i, rc = 20;
    ULONG           dest, mnt_port, nfs_port;
    UBYTE           dirfh[FHSIZE], fh[FHSIZE];
    const char     *host = (argc > 1) ? argv[1] : "127.0.0.1";
    LONG            pport = (argc > 2) ? (LONG)atol_(argv[2]) : 111;
    const char     *path = (argc > 3) ? argv[3] : "/export";
    const char     *file = (argc > 4) ? argv[4] : "payload.txt";
    LONG            ug_errno = -1;

    Printf((CONST_STRPTR)"NfsProbe: peer %s port %ld path %s file %s\n",
           (LONG)host, pport, (LONG)path, (LONG)file);

    ug = OpenLibrary((CONST_STRPTR)"usergroup.library", 0UL);
    if (ug == NULL) { Printf((CONST_STRPTR)"nfsprobe: no usergroup.library\n"); return 20; }

    tags[0].ti_Tag = UGT_ERRNOLPTR; tags[0].ti_Data = (ULONG)&ug_errno;
    tags[1].ti_Tag = TAG_DONE;      tags[1].ti_Data = 0;
    if (ug_setup_context(ug, (STRPTR)"NfsProbe", tags) != 0)
    {
        Printf((CONST_STRPTR)"credentials=FAIL ug_SetupContextTagList errno=%ld\n", ug_errno);
        CloseLibrary(ug);
        return 20;
    }

    cred_uid = ug_geteuid(ug);
    cred_gid = ug_getegid(ug);
    for (i = 0; i < 16; i++) cred_groups[i] = 0;
    cred_ngroups = ug_getgroups(ug, 16, cred_groups);
    if (cred_ngroups < 0) cred_ngroups = 0;
    if (cred_ngroups > 16) cred_ngroups = 16;

    /*
     * getpwnam() as well, and not for the uid: it is the vector that reads
     * the passwd FILE, which 0.26.6 taught to accept AmiTCP 4's own
     * pipe-delimited records.  A stack that cannot parse the database an
     * AmiTCP install already has cannot mount as anyone but the default.
     */
    pw = ug_getpwnam(ug, (STRPTR)"root");
    Printf((CONST_STRPTR)"credentials=ok uid=%ld gid=%ld ngroups=%ld getpwnam_root=%s\n",
           cred_uid, cred_gid, cred_ngroups,
           (LONG)((pw != NULL) ? "found" : "absent"));
    for (i = 0; i < cred_ngroups; i++)
        Printf((CONST_STRPTR)"  group[%ld]=%ld\n", i, cred_groups[i]);

    sb = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (sb == NULL)
    {
        Printf((CONST_STRPTR)"nfsprobe: no bsdsocket.library\n");
        CloseLibrary(ug);
        return 20;
    }

    dest = p_inet_addr(sb, host);
    if (dest == 0xFFFFFFFFUL) { Printf((CONST_STRPTR)"nfsprobe: bad address %s\n", (LONG)host); goto out; }

    s = p_socket(sb, P_AF_INET, P_SOCK_DGRAM, 0);
    if (s < 0) { Printf((CONST_STRPTR)"nfsprobe: socket errno=%ld\n", p_errno(sb)); goto out; }

    /*
     * A RESERVED SOURCE PORT.  A real NFS server drops a call from a port
     * above 1023, so this is not a detail of the test: it is the thing the
     * mount depends on.  Walk down from 1023 the way an RPC library does.
     */
    {
        LONG bound = 0;
        UWORD p;
        for (p = 1023; p >= 512; p--)
        {
            ProbeAddr sa;
            sa.sin_len = (UBYTE)sizeof(sa); sa.sin_family = P_AF_INET;
            sa.sin_port = p; sa.sin_addr = 0;
            for (i = 0; i < 8; i++) sa.sin_zero[i] = 0;
            if (p_bind(sb, s, &sa) == 0) { bound = 1; break; }
        }
        if (!bound)
        {
            Printf((CONST_STRPTR)"reserved_port=FAIL no port in 512..1023 could be bound\n");
            goto out;
        }
        if (p_getsockname(sb, s, &me, &melen) == 0)
            Printf((CONST_STRPTR)"reserved_port=ok port=%ld\n", (LONG)me.sin_port);
    }

    /* PORTMAP GETPORT for MOUNT.  AUTH_NULL: the portmapper does not care. */
    build_call(PMAP_PROG, PMAP_VERS, PMAP_GETPORT, 0);
    xdr_put32(MNT_PROG); xdr_put32(MNT_VERS);
    xdr_put32(IPPROTO_UDP_); xdr_put32(0UL);
    body = exchange(sb, s, "getport(mount)", dest, (UWORD)pport);
    if (body < 0) { Printf((CONST_STRPTR)"getport=FAIL\n"); goto out; }
    mnt_port = get32(&reply_buf[body]);
    Printf((CONST_STRPTR)"getport=ok mount_port=%lu\n", mnt_port);
    if (mnt_port == 0UL) { Printf((CONST_STRPTR)"mount=FAIL program not registered\n"); goto out; }

    /* MOUNT MNT, with credentials from here on. */
    build_call(MNT_PROG, MNT_VERS, MNTPROC_MNT, 1);
    xdr_put_string(path);
    body = exchange(sb, s, "mnt", dest, (UWORD)mnt_port);
    if (body < 0) { Printf((CONST_STRPTR)"mount=FAIL\n"); goto out; }
    if (get32(&reply_buf[body]) != 0UL)
    {
        Printf((CONST_STRPTR)"mount=FAIL status=%lu\n", get32(&reply_buf[body]));
        goto out;
    }
    for (i = 0; i < FHSIZE; i++) dirfh[i] = reply_buf[body + 4 + i];
    Printf((CONST_STRPTR)"mount=ok path=%s\n", (LONG)path);

    /* NFS is on the same peer; ask the portmapper for it too. */
    build_call(PMAP_PROG, PMAP_VERS, PMAP_GETPORT, 0);
    xdr_put32(NFS2_PROG); xdr_put32(NFS2_VERS);
    xdr_put32(IPPROTO_UDP_); xdr_put32(0UL);
    body = exchange(sb, s, "getport(nfs)", dest, (UWORD)pport);
    if (body < 0) { Printf((CONST_STRPTR)"getport_nfs=FAIL\n"); goto out; }
    nfs_port = get32(&reply_buf[body]);
    Printf((CONST_STRPTR)"getport_nfs=ok nfs_port=%lu\n", nfs_port);

    /* LOOKUP */
    build_call(NFS2_PROG, NFS2_VERS, NFSPROC_LOOKUP, 1);
    xdr_put_bytes(dirfh, FHSIZE);
    xdr_put_string(file);
    body = exchange(sb, s, "lookup", dest, (UWORD)nfs_port);
    if (body < 0) { Printf((CONST_STRPTR)"lookup=FAIL\n"); goto out; }
    if (get32(&reply_buf[body]) != 0UL)
    {
        Printf((CONST_STRPTR)"lookup=FAIL status=%lu\n", get32(&reply_buf[body]));
        goto out;
    }
    for (i = 0; i < FHSIZE; i++) fh[i] = reply_buf[body + 4 + i];
    Printf((CONST_STRPTR)"lookup=ok file=%s\n", (LONG)file);

    /* READ, and compare the bytes. */
    build_call(NFS2_PROG, NFS2_VERS, NFSPROC_READ, 1);
    xdr_put_bytes(fh, FHSIZE);
    xdr_put32(0UL);                     /* offset */
    xdr_put32(512UL);                   /* count  */
    xdr_put32(0UL);                     /* totalcount, unused in v2 */
    body = exchange(sb, s, "read", dest, (UWORD)nfs_port);
    if (body < 0) { Printf((CONST_STRPTR)"read=FAIL\n"); goto out; }
    if (get32(&reply_buf[body]) != 0UL)
    {
        Printf((CONST_STRPTR)"read=FAIL status=%lu\n", get32(&reply_buf[body]));
        goto out;
    }
    {
        ULONG dlen_off = (ULONG)body + 4UL + 68UL;   /* status + fattr */
        ULONG dlen, want = cstr_len(g_content), bad = 0;
        if (dlen_off + 4UL > REPLY_MAX) { Printf((CONST_STRPTR)"read=FAIL truncated\n"); goto out; }
        dlen = get32(&reply_buf[dlen_off]);
        if (dlen != want)
        {
            Printf((CONST_STRPTR)"read=FAIL length got=%lu want=%lu\n", dlen, want);
            goto out;
        }
        for (i = 0; i < (LONG)dlen; i++)
            if (reply_buf[dlen_off + 4UL + (ULONG)i] != (UBYTE)g_content[i])
                bad++;
        if (bad != 0UL)
        {
            Printf((CONST_STRPTR)"read=FAIL %lu of %lu bytes differ\n", bad, dlen);
            goto out;
        }
        Printf((CONST_STRPTR)"read=ok bytes=%lu verified\n", dlen);
    }

    Printf((CONST_STRPTR)"RESULT=PASS\n");
    rc = 0;

out:
    if (rc != 0) Printf((CONST_STRPTR)"RESULT=FAIL\n");
    if (s >= 0) p_close(sb, s);
    if (sb != NULL) CloseLibrary(sb);
    if (ug != NULL) CloseLibrary(ug);
    return (int)rc;
}
