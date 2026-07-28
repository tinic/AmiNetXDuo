/*
 * ttlprobe -- measures whether setsockopt(IP_TTL) reaches the wire, and on
 * which socket type.
 *
 * This is the measurement `traceroute` is designed around: IP_TTL is honoured
 * on a raw socket and ignored on a UDP one, so a UDP-probe traceroute cannot
 * work on this stack.
 *
 * Four datagrams, each preceded by setsockopt(IPPROTO_IP, IP_TTL) and a
 * getsockopt() to show what the library thinks it stored:
 *
 *   1. raw ICMP, TTL 1      2. raw ICMP, TTL 5
 *   3. UDP,      TTL 1      4. UDP,      TTL 5
 *
 * Its own output proves nothing: getsockopt reads back whatever setsockopt
 * stored, on both.  The answer is in the frame dump the emulated A2065 writes:
 *
 *   tests/trace/a2065pcap.py build/fsuae-base-<tag>/Cache/Logs/fs-uae.log.txt \
 *       -o ttl.pcap
 *   tcpdump -nr ttl.pcap -v        # or open it in Wireshark
 *
 * A one-off probe rather than a command or a test, so it has no CMake entry.
 * Compile it by hand and stage it like any other executable:
 *
 *   . tools/amiga-toolchain.sh
 *   "$AMIGA_GCC" -O2 -m68020 -I"$AMIGA_NDK" -o TtlProbe tests/tools/ttlprobe.c
 *
 * The vectors are called by hand at the LVOs docs/RESEARCH.md 3.2 lists, for
 * the same reason src/tools/toolsock.c does it: the NDK inlines assume a
 * global SocketBase.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* struct sockaddr_in, open-coded -- four fields and a pad, unchanged since
   4.2BSD. */
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
#define P_SOCK_RAW      3
#define P_IPPROTO_IP    0
#define P_IPPROTO_ICMP  1
#define P_IP_TTL        4

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
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (d3)
                      : "cc", "memory");
    return res;
}

static LONG p_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
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

static LONG p_getsockopt(struct Library *base, LONG s, LONG level, LONG name,
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

/* --------------------------------------------------------------- probes --- */

static UWORD p_checksum(const UBYTE *data, ULONG len)
{
    ULONG sum = 0;
    ULONG i;

    for (i = 0; i + 1 < len; i += 2)
        sum += ((ULONG)data[i] << 8) | (ULONG)data[i + 1];

    if ((len & 1) != 0)
        sum += (ULONG)data[len - 1] << 8;

    while ((sum >> 16) != 0)
        sum = (sum & 0xffffUL) + (sum >> 16);

    return (UWORD)(~sum & 0xffffUL);
}

static UBYTE p_packet[16];

static VOID p_one(struct Library *sb, BOOL raw, LONG ttl, ULONG dest, UWORD id)
{
    ProbeAddr sa;
    LONG      s;
    LONG      value = ttl;
    LONG      back = -1;
    LONG      backlen = (LONG)sizeof(back);
    LONG      sent;
    ULONG     i;

    for (i = 0; i < (ULONG)sizeof(sa.sin_zero); i++)
        sa.sin_zero[i] = 0;

    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = P_AF_INET;
    sa.sin_port   = raw ? 0 : 33434;    /* the classic traceroute port */
    sa.sin_addr   = dest;

    s = p_socket(sb, P_AF_INET, raw ? P_SOCK_RAW : P_SOCK_DGRAM,
                 raw ? P_IPPROTO_ICMP : 0);
    if (s < 0)
    {
        Printf((CONST_STRPTR)"%s socket failed, errno %ld\n",
               raw ? (LONG)"raw" : (LONG)"udp", p_errno(sb));
        return;
    }

    if (p_setsockopt(sb, s, P_IPPROTO_IP, P_IP_TTL, &value,
                     (LONG)sizeof(value)) != 0)
    {
        Printf((CONST_STRPTR)"  setsockopt(IP_TTL, %ld) FAILED, errno %ld\n",
               ttl, p_errno(sb));
    }

    if (p_getsockopt(sb, s, P_IPPROTO_IP, P_IP_TTL, &back, &backlen) != 0)
        back = -1;

    if (raw)
    {
        /* An ICMP echo request, so the packet is a legal one either way. */
        p_packet[0] = 8;                /* type: echo                     */
        p_packet[1] = 0;
        p_packet[2] = 0;
        p_packet[3] = 0;
        p_packet[4] = (UBYTE)(id >> 8);
        p_packet[5] = (UBYTE)(id & 0xff);
        p_packet[6] = 0;
        p_packet[7] = (UBYTE)ttl;       /* sequence: which probe this is  */

        for (i = 8; i < (ULONG)sizeof(p_packet); i++)
            p_packet[i] = (UBYTE)i;

        {
            UWORD c = p_checksum(p_packet, sizeof(p_packet));

            p_packet[2] = (UBYTE)(c >> 8);
            p_packet[3] = (UBYTE)(c & 0xff);
        }
    }
    else
    {
        for (i = 0; i < (ULONG)sizeof(p_packet); i++)
            p_packet[i] = (UBYTE)('A' + ttl);
    }

    sent = p_sendto(sb, s, p_packet, (LONG)sizeof(p_packet), &sa);

    Printf((CONST_STRPTR)"%s: asked for TTL %ld, getsockopt says %ld, "
                         "sendto returned %ld (errno %ld)\n",
           raw ? (LONG)"raw ICMP" : (LONG)"UDP     ", ttl, back, sent,
           (LONG)((sent < 0) ? p_errno(sb) : 0));

    (VOID)p_close(sb, s);
}

int main(void)
{
    struct Library *sb;
    ULONG           dest = (8UL << 24) | (8UL << 16) | (8UL << 8) | 8UL;

    sb = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (sb == NULL)
    {
        Printf((CONST_STRPTR)"no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    /* Spaced out, so the four are easy to tell apart in the frame dump. */
    p_one(sb, TRUE,  1, dest, 0x4141);
    Delay(50);
    p_one(sb, TRUE,  5, dest, 0x4142);
    Delay(50);
    p_one(sb, FALSE, 1, dest, 0);
    Delay(50);
    p_one(sb, FALSE, 5, dest, 0);
    Delay(50);

    CloseLibrary(sb);
    return RETURN_OK;
}
