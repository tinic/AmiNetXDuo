/*
 * McastRecv, a co-bound BSD listener on the mDNS port.
 *
 * It sets SO_REUSEPORT and binds UDP 5353 alongside the built-in mDNS
 * responder, joins 224.0.0.251, and prints every datagram it receives with its
 * source address.  A harness points a foreign multicast at the guest while
 * this is running; the lines it prints are the evidence that the fan-out in
 * _nx_udp_packet_receive cloned the datagram to a BSD sharer, not just to the
 * internal responder.
 *
 * It prints one fixed marker line once it is listening ("MCASTSHARE-READY")
 * so a host harness can wait for it instead of guessing boot time, then waits
 * a bounded number of one-second rounds.
 *
 * Like McastProbe it calls bsdsocket.library straight through the LVO table,
 * linked against nothing of ours.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <devices/timer.h>      /* struct timeval, for WaitSelect */
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/macros.h>

/* The READY marker goes to the SERIAL port, not the DOS console: a host
   harness can watch build/amiberry-serial-*.log for it in real time, exactly
   the way amiberry-run.sh watches for the ANXD-RUN token, where Printf output
   goes to DH0:stdout.txt and is only read after the run. */
#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

static VOID r_serial(const char *s)
{
    while (*s != '\0')
        RawPutChar((UBYTE)*s++);
}

typedef struct RecvAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} RecvAddr;

typedef struct RecvMreq
{
    ULONG   imr_multiaddr;
    ULONG   imr_interface;
} RecvMreq;

#define R_AF_INET           2
#define R_SOCK_DGRAM        2
#define R_IPPROTO_IP        0
#define R_SOL_SOCKET        0xffff
#define R_SO_REUSEADDR      0x0004
#define R_SO_REUSEPORT      0x0200
#define R_IP_ADD_MEMBERSHIP 12

#define R_MDNS_GROUP    ((224UL << 24) | (0UL << 16) | (0UL << 8) | 251UL)
#define R_MDNS_PORT     5353
#define R_ROUNDS        40                  /* 40 x 1s of listening */

static LONG r_socket(struct Library *base, LONG domain, LONG type, LONG proto)
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

static LONG r_bind(struct Library *base, LONG s, const void *name, LONG namelen)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)name;
    register LONG            d1  __asm("d1") = namelen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG r_recvfrom(struct Library *base, LONG s, void *buf, LONG len,
                       void *from, LONG *fromlen)
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

static LONG r_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
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

static LONG r_waitselect(struct Library *base, LONG nfds, ULONG *readfds,
                         struct timeval *tv)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = (APTR)readfds;
    register APTR            a1  __asm("a1") = NULL;
    register APTR            a2  __asm("a2") = NULL;
    register APTR            a3  __asm("a3") = (APTR)tv;
    register APTR            d1  __asm("d1") = NULL;
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

static LONG r_close(struct Library *base, LONG s)
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

static LONG r_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static UBYTE r_rxbuf[1500];

int main(void)
{
    struct Library *sb;
    LONG            s;
    LONG            one = 1;
    LONG            rc;
    RecvAddr        local;
    RecvMreq        mreq;
    ULONG           i;
    LONG            rounds;
    LONG            seen = 0;

    sb = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (sb == NULL)
    {
        Printf((CONST_STRPTR)"mcastrecv: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    s = r_socket(sb, R_AF_INET, R_SOCK_DGRAM, R_IPPROTO_IP);
    if (s < 0)
    {
        Printf((CONST_STRPTR)"mcastrecv: socket failed, errno %ld\n",
               r_errno(sb));
        CloseLibrary(sb);
        return RETURN_FAIL;
    }

    /* Opt in BEFORE bind, exactly as the built-in responder does. */
    rc = r_setsockopt(sb, s, R_SOL_SOCKET, R_SO_REUSEADDR, &one,
                      (LONG)sizeof(one));
    if (rc != 0)
        Printf((CONST_STRPTR)"mcastrecv: SO_REUSEADDR failed, errno %ld\n",
               r_errno(sb));

    rc = r_setsockopt(sb, s, R_SOL_SOCKET, R_SO_REUSEPORT, &one,
                      (LONG)sizeof(one));
    if (rc != 0)
        Printf((CONST_STRPTR)"mcastrecv: SO_REUSEPORT failed, errno %ld\n",
               r_errno(sb));

    for (i = 0; i < (ULONG)sizeof(local.sin_zero); i++)
        local.sin_zero[i] = 0;
    local.sin_len    = (UBYTE)sizeof(local);
    local.sin_family = R_AF_INET;
    local.sin_port   = R_MDNS_PORT;
    local.sin_addr   = 0;                       /* INADDR_ANY */

    rc = r_bind(sb, s, &local, (LONG)sizeof(local));
    if (rc != 0)
    {
        Printf((CONST_STRPTR)"mcastrecv: bind 5353 failed, errno %ld\n",
               r_errno(sb));
        (VOID)r_close(sb, s);
        CloseLibrary(sb);
        return RETURN_FAIL;
    }

    mreq.imr_multiaddr = R_MDNS_GROUP;
    mreq.imr_interface = 0UL;

    rc = r_setsockopt(sb, s, R_IPPROTO_IP, R_IP_ADD_MEMBERSHIP, &mreq,
                      (LONG)sizeof(mreq));
    if (rc != 0)
        Printf((CONST_STRPTR)"mcastrecv: IP_ADD_MEMBERSHIP failed, errno %ld\n",
               r_errno(sb));

    Printf((CONST_STRPTR)"mcastrecv: MCASTSHARE-READY bound 0.0.0.0:5353, "
                         "joined 224.0.0.251\n");
    r_serial("MCASTSHARE-READY\n");

    for (rounds = 0; rounds < R_ROUNDS; rounds++)
    {
        struct timeval tv;
        ULONG          readfds = (1UL << s);
        LONG           ready;

        tv.tv_secs  = 1;
        tv.tv_micro = 0;

        ready = r_waitselect(sb, s + 1, &readfds, &tv);
        if (ready <= 0)
            continue;

        {
            RecvAddr from;
            LONG     fromlen = (LONG)sizeof(from);
            LONG     got;

            from.sin_addr = 0;
            got = r_recvfrom(sb, s, r_rxbuf, (LONG)sizeof(r_rxbuf) - 1,
                             &from, &fromlen);
            if (got < 0)
            {
                Printf((CONST_STRPTR)"mcastrecv: recvfrom errno %ld\n",
                       r_errno(sb));
                continue;
            }

            seen++;
            Printf((CONST_STRPTR)"mcastrecv: recv %ld bytes from "
                                 "%ld.%ld.%ld.%ld:%ld\n",
                   got,
                   (LONG)((from.sin_addr >> 24) & 0xffUL),
                   (LONG)((from.sin_addr >> 16) & 0xffUL),
                   (LONG)((from.sin_addr >> 8) & 0xffUL),
                   (LONG)(from.sin_addr & 0xffUL),
                   (LONG)from.sin_port);
        }
    }

    Printf((CONST_STRPTR)"mcastrecv: done, %ld datagram(s) in %ld round(s)\n",
           seen, (LONG)R_ROUNDS);

    (VOID)r_close(sb, s);
    CloseLibrary(sb);

    return RETURN_OK;
}
