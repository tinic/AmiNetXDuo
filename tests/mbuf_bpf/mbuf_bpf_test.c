/*
 * AmiNetXDuo -- milestone 7 on-Amiga test: mbuf_* and bpf_*.
 *
 * The host tests in src/mbuf/test/ and src/bpf/test/ carry the exhaustive
 * batteries. This one exists for the four things a host cannot answer:
 *
 *   1. The real constants. On a 64-bit host the mbuf replica scales MSIZE to
 *      256 and MLEN/MHLEN to 224/208, so every boundary case is exercised at
 *      the wrong number. Here they are 128/108/100, and `struct bpf_hdr` is
 *      the real 18 bytes with bh_hdrlen 20.
 *   2. Msize alignment from A REAL AllocVec(). dtom() is published ABI and
 *      needs every mbuf on a 128-byte boundary; AllocVec() promises 8. The
 *      slab allocator's rounding is only really tested against the real
 *      allocator.
 *   3. THE NX_PACKET BRIDGE, which needs a live packet pool, which needs
 *      ThreadX running.
 *   4. Forbid()/Permit(), Signal() and GetSysTime() -- the four platform
 *      hooks that the host build replaces with stubs.
 *
 * Everything runs on main()'s own Process, adopted as a TX_THREAD, so
 * dos.library stays usable for output throughout.
 *
 *   cmake --build build/cm --parallel --target mbuf_bpf_test
 *   AMINETXDUO_RUN_TAG=mbufbpf ./tools/fsuae-run.sh -t 120 \
 *       build/cm/tests/mbuf_bpf/mbuf_bpf_test
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_amiga.h"
#include "nx_api.h"

#include "aminetxduo/mbuf.h"
#include "aminetxduo/bpf.h"
#include "aminetxduo/compat.h"
#include "aminetxduo/crashguard.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>
#include <stddef.h>


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

static char  t_line[256];
static ULONG t_used;

static VOID t_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{
    (VOID) unused;

    if (c != '\0' && t_used < (ULONG) (sizeof(t_line) - 1))
    {
        t_line[t_used++] = (char) c;
    }
}

/*
 * Every caller is main()'s Process -- no ThreadX thread ever logs here -- so
 * dos.library is safe and the output goes straight out rather than being
 * buffered for a replay at the end.
 */
static VOID t_log(const char *fmt, ...)
{

va_list args;
BPTR    out;
ULONG   i;


    t_used = 0;
    va_start(args, fmt);
    RawDoFmt((STRPTR) fmt, args, (void (*)()) t_put_char, NULL);
    va_end(args);

    t_line[t_used]     = '\n';
    t_line[t_used + 1] = '\0';

    for (i = 0; i <= t_used; i++)
    {
        RawPutChar((UBYTE) t_line[i]);
    }

    out =  Output();
    if (out != (BPTR) 0)
    {
        (VOID) Write(out, (APTR) t_line, (LONG) (t_used + 1));
    }
}


/* -------------------------------------------------------------- results -- */

static ULONG t_checks;
static ULONG t_failures;

static UINT t_check(UINT ok, const char *what)
{

    t_checks++;
    if (!ok)
    {
        t_failures++;
        t_log("  FAIL: %s", what);
    }

    return ok;
}

#define CHECK(cond, what)   (VOID) t_check((UINT) ((cond) ? 1 : 0), (what))


/* ----------------------------------------------------------- packet pool -- */

#define T_PACKET_PAYLOAD        1568        /* == AMI_POOL_PAYLOAD          */
#define T_PACKET_COUNT          8
#define T_PACKET_OVERHEAD       96

static NX_PACKET_POOL   t_pool;
static ULONG            t_pool_memory[(T_PACKET_COUNT *
                                       (T_PACKET_PAYLOAD + T_PACKET_OVERHEAD)) /
                                      sizeof(ULONG)];
static UINT             t_pool_ok;


/* -------------------------------------------------------------- helpers -- */

static VOID t_ramp(UBYTE *p, ULONG len, UBYTE start)
{

ULONG i;


    for (i = 0; i < len; i++)
    {
        p[i] = (UBYTE) (start + i);
    }
}

static UINT t_ramp_ok(const UBYTE *p, ULONG len, UBYTE start)
{

ULONG i;


    for (i = 0; i < len; i++)
    {
        if (p[i] != (UBYTE) (start + i))
        {
            return TX_FALSE;
        }
    }

    return TX_TRUE;
}

static struct mbuf *t_make_chain(ULONG n, ULONG each, UBYTE start)
{

struct mbuf  *head = NULL;
struct mbuf **np   = &head;
ULONG         i;


    for (i = 0; i < n; i++)
    {
        struct mbuf *m = ami_mbuf_get();

        if (m == NULL)
        {
            return head;
        }

        t_ramp((UBYTE *) m->m_data, each, (UBYTE) (start + i * each));
        m->m_len = (LONG) each;
        *np = m;
        np  = &m->m_next;
    }

    return head;
}


/* ---------------------------------------------------------- frame builder -- */

#define T_ETH_HDR   14

static const UBYTE t_mac_dst[6] = { 0x00, 0x80, 0x10, 0x11, 0x22, 0x33 };
static const UBYTE t_mac_src[6] = { 0x00, 0x60, 0x97, 0xAA, 0xBB, 0xCC };

static VOID t_put16(UBYTE *p, UWORD v)
{

    p[0] = (UBYTE) (v >> 8);
    p[1] = (UBYTE) v;
}

/* Ethernet + IPv4 + TCP, ihl 5, no fragment offset. 60 bytes. */
static ULONG t_make_tcp(UBYTE *buf, UWORD sport, UWORD dport)
{

ULONG ip  = T_ETH_HDR;
ULONG l4  = ip + 20;
ULONG end = l4 + 20 + 6;
ULONG i;


    for (i = 0; i < end; i++)
    {
        buf[i] = 0;
    }

    for (i = 0; i < 6; i++)
    {
        buf[i]     = t_mac_dst[i];
        buf[6 + i] = t_mac_src[i];
    }
    t_put16(&buf[12], 0x0800);

    buf[ip + 0] = 0x45;
    t_put16(&buf[ip + 2], (UWORD) (end - ip));
    buf[ip + 8] = 64;
    buf[ip + 9] = 6;
    buf[ip + 12] = 10; buf[ip + 13] = 0; buf[ip + 14] = 0; buf[ip + 15] = 1;
    buf[ip + 16] = 10; buf[ip + 17] = 0; buf[ip + 18] = 0; buf[ip + 19] = 2;

    t_put16(&buf[l4 + 0], sport);
    t_put16(&buf[l4 + 2], dport);
    buf[l4 + 12] = 0x50;

    for (i = 0; i < 6; i++)
    {
        buf[l4 + 20 + i] = 0xEE;
    }

    return end;
}

/* tcpdump "tcp port 80" for DLT_EN10MB, exactly as libpcap emits it. */
static const struct bpf_insn t_prog_tcp80[] =
{
    BPF_STMT(BPF_LD  | BPF_H | BPF_ABS, 12),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x0800, 0, 10),
    BPF_STMT(BPF_LD  | BPF_B | BPF_ABS, 23),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 6, 0, 8),
    BPF_STMT(BPF_LD  | BPF_H | BPF_ABS, 20),
    BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x1FFF, 6, 0),
    BPF_STMT(BPF_LDX | BPF_B | BPF_MSH, 14),
    BPF_STMT(BPF_LD  | BPF_H | BPF_IND, 14),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 80, 2, 0),
    BPF_STMT(BPF_LD  | BPF_H | BPF_IND, 16),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 80, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, 262144),
    BPF_STMT(BPF_RET | BPF_K, 0)
};

#define T_TCP80_LEN ((ULONG) (sizeof(t_prog_tcp80) / sizeof(t_prog_tcp80[0])))


/* ------------------------------------------------------------ mbuf tests -- */

static VOID t_test_layout(VOID)
{

    t_log("mbuf: the real 68k layout");

    t_log("  MSIZE %ld  MLEN %ld  MHLEN %ld  sizeof(struct mbuf) %ld",
          (ULONG) MSIZE, (ULONG) MLEN, (ULONG) MHLEN,
          (ULONG) sizeof(struct mbuf));

    CHECK(MSIZE == 128,                     "MSIZE is 128");
    CHECK(MLEN  == 108,                     "MLEN is 108");
    CHECK(MHLEN == 100,                     "MHLEN is 100");
    CHECK(sizeof(struct mbuf)   == 128,     "sizeof(struct mbuf) is 128");
    CHECK(sizeof(struct m_hdr)  == 20,      "sizeof(struct m_hdr) is 20");
    CHECK(sizeof(struct pkthdr) == 8,       "sizeof(struct pkthdr) is 8");
    CHECK(MCLBYTES == 2048,                 "MCLBYTES is 2048");

    CHECK(offsetof(struct mbuf, m_next)    == 0,  "mh_next at 0");
    CHECK(offsetof(struct mbuf, m_nextpkt) == 4,  "mh_nextpkt at 4");
    CHECK(offsetof(struct mbuf, m_data)    == 8,  "mh_data at 8");
    CHECK(offsetof(struct mbuf, m_len)     == 12, "mh_len at 12");
    CHECK(offsetof(struct mbuf, m_type)    == 16, "mh_type at 16");
    CHECK(offsetof(struct mbuf, m_flags)   == 18, "mh_flags at 18");
    CHECK(offsetof(struct mbuf, m_pkthdr)  == 20, "m_pkthdr at 20");
    CHECK(offsetof(struct mbuf, m_ext)     == 28, "m_ext at 28");
    CHECK(offsetof(struct mbuf, m_pktdat)  == 28, "m_pktdat at 28");
    CHECK(offsetof(struct mbuf, m_dat)     == 20, "m_dat at 20");
}

static VOID t_test_alignment(VOID)
{

struct mbuf *chain = NULL;
struct mbuf *m;
ULONG        i;
ULONG        got     = 0;
UINT         aligned = TX_TRUE;


    t_log("mbuf: 128-byte alignment out of a real AllocVec()");

    /* Enough to cross several slabs. AllocVec() gives 8-byte alignment, so
       every one of these depends on the slab rounding being right. */
    for (i = 0; i < 96; i++)
    {
        m = ami_mbuf_get();
        if (m == NULL)
        {
            break;
        }

        got++;

        if (((ULONG) m & (MSIZE - 1)) != 0)
        {
            aligned = TX_FALSE;
        }

        /* The published dtom(), from a pointer anywhere inside the data area. */
        if (dtom((UBYTE *) m->m_dat + MLEN - 1) != m)
        {
            aligned = TX_FALSE;
        }
        if (dtom((UBYTE *) m->m_data) != m)
        {
            aligned = TX_FALSE;
        }

        m->m_next = chain;
        chain     = m;
    }

    CHECK(got == 96,  "96 mbufs allocated");
    CHECK(aligned,    "every mbuf is MSIZE aligned and dtom() round-trips");

    ami_mbuf_freem(chain);
    CHECK(ami_mbuf_outstanding() == 0, "no mbufs leaked");
}

static VOID t_test_ops(VOID)
{

struct mbuf *m;
struct mbuf *c;
UBYTE        out[200];


    t_log("mbuf: chain operations at the real MLEN/MHLEN");

    /* adj, both directions. */
    m = t_make_chain(3, 10, 0);
    CHECK(ami_mbuf_length(m) == 30,            "chain is 30 bytes");
    CHECK(ami_mbuf_adj(m, 15) == 0,            "adj +15");
    CHECK(ami_mbuf_length(m) == 15,            "15 bytes left");
    CHECK(ami_mbuf_adj(m, -5) == 0,            "adj -5");
    CHECK(ami_mbuf_length(m) == 10,            "10 bytes left");
    ami_mbuf_freem(m);

    /* cat compaction happens at MLEN, which is 108 here and 224 on a
       64-bit host -- this is the case the host test cannot place. */
    m = ami_mbuf_get();
    m->m_len = 100;
    t_ramp((UBYTE *) m->m_data, 100, 0);
    c = t_make_chain(1, 8, 100);
    CHECK(ami_mbuf_cat(m, c) == 0,             "cat 100 + 8 into MLEN 108");
    CHECK(m->m_next == NULL,                   "compacted, not spliced");
    CHECK(m->m_len == 108,                     "exactly MLEN bytes");
    CHECK(t_ramp_ok((const UBYTE *) m->m_data, 108, 0), "cat data intact");
    ami_mbuf_freem(m);

    m = ami_mbuf_get();
    m->m_len = 101;
    c = t_make_chain(1, 8, 0);
    CHECK(ami_mbuf_cat(m, c) == 0,             "cat 101 + 8 overflows MLEN");
    CHECK(m->m_next == c,                      "spliced, not compacted");
    ami_mbuf_freem(m);

    /* copydata / copyback / copym. */
    m = t_make_chain(3, 10, 0);
    CHECK(ami_mbuf_copydata(m, 7, 16, out) == 0,      "copydata across mbufs");
    CHECK(t_ramp_ok(out, 16, 7),                      "copydata content");
    CHECK(ami_mbuf_copydata(m, 0, 31, out) == -1,     "copydata past the end");

    c = ami_mbuf_copym(m, 5, 20);
    CHECK(c != NULL,                                  "copym");
    if (c != NULL)
    {
        CHECK(ami_mbuf_length(c) == 20,               "copym length");
        CHECK(ami_mbuf_copydata(c, 0, 20, out) == 0,  "copym readback");
        CHECK(t_ramp_ok(out, 20, 5),                  "copym content");
        ami_mbuf_freem(c);
    }

    t_ramp(out, 20, 200);
    CHECK(ami_mbuf_copyback(m, 25, 20, out) == 0,     "copyback extends");
    CHECK(ami_mbuf_length(m) >= 45,                   "chain grew");
    CHECK(ami_mbuf_copydata(m, 25, 20, out) == 0,     "copyback readback");
    CHECK(t_ramp_ok(out, 20, 200),                    "copyback content");
    ami_mbuf_freem(m);

    /* prepend: MHLEN is 100, so 100 fits in a fresh header mbuf and 101
       does not. */
    m = ami_mbuf_gethdr();
    m->m_len        = 10;
    m->m_pkthdr.len = 10;
    m = ami_mbuf_prepend(m, 100);
    CHECK(m != NULL,                                  "prepend MHLEN bytes");
    if (m != NULL)
    {
        CHECK(m->m_len == 100,                        "prepend length");
        CHECK(m->m_pkthdr.len == 110,                 "pkthdr.len followed");
        CHECK((m->m_flags & M_PKTHDR) != 0,           "header moved to the new head");
        ami_mbuf_freem(m);
    }

    m = t_make_chain(1, 10, 0);
    CHECK(ami_mbuf_prepend(m, 109) == NULL,           "prepend past MLEN fails");
    CHECK(ami_mbuf_outstanding() == 0,                "...and frees the chain");

    /* pullup: 108 bytes fit in one plain mbuf, 109 do not. */
    m = t_make_chain(4, 30, 0);
    m = ami_mbuf_pullup(m, 108);
    CHECK(m != NULL,                                  "pullup MLEN bytes");
    if (m != NULL)
    {
        CHECK(m->m_len >= 108,                        "pullup gathered");
        CHECK(t_ramp_ok((const UBYTE *) m->m_data, 108, 0), "pullup content");
        ami_mbuf_freem(m);
    }

    m = t_make_chain(4, 30, 0);
    CHECK(ami_mbuf_pullup(m, 109) == NULL,            "pullup past MLEN fails");
    CHECK(ami_mbuf_outstanding() == 0,                "...and frees the chain");

    /* Clusters, and copym sharing them by reference count. */
    m = ami_mbuf_getcl();
    CHECK(m != NULL,                                  "cluster allocated");
    if (m != NULL)
    {
        CHECK((m->m_flags & M_EXT) != 0,              "M_EXT set");
        CHECK(m->m_ext.ext_size == MCLBYTES,          "ext_size is MCLBYTES");
        CHECK(ami_mbuf_clusters_outstanding() == 1,   "one cluster live");

        t_ramp((UBYTE *) m->m_data, 40, 0);
        m->m_len = 40;

        c = ami_mbuf_copym(m, 8, 16);
        CHECK(c != NULL,                              "copym over a cluster");
        if (c != NULL)
        {
            CHECK((c->m_flags & M_EXT) != 0,          "copy is also M_EXT");
            CHECK(c->m_ext.ext_buf == m->m_ext.ext_buf, "cluster shared");
            CHECK(ami_mbuf_clusters_outstanding() == 1, "still one cluster");
            ami_mbuf_freem(c);
            CHECK(ami_mbuf_clusters_outstanding() == 1, "reference dropped only");
        }

        ami_mbuf_freem(m);
        CHECK(ami_mbuf_clusters_outstanding() == 0,   "cluster released");
    }

    CHECK(ami_mbuf_outstanding() == 0, "no mbufs leaked");
}

static VOID t_test_packet_bridge(VOID)
{

NX_PACKET   *packet = NX_NULL;
NX_PACKET   *back   = NX_NULL;
struct mbuf *m;
UBYTE        data[900];
UBYTE        out[900];
UINT         status;


    t_log("mbuf: the NX_PACKET bridge");

    if (!t_pool_ok)
    {
        t_log("  SKIP: no packet pool");
        return;
    }

    t_ramp(data, (ULONG) sizeof(data), 17);

    status =  nx_packet_allocate(&t_pool, &packet, NX_IP_PACKET, NX_NO_WAIT);
    CHECK(status == NX_SUCCESS, "packet allocated");
    if (status != NX_SUCCESS)
    {
        return;
    }

    status =  nx_packet_data_append(packet, data, (ULONG) sizeof(data),
                                    &t_pool, NX_NO_WAIT);
    CHECK(status == NX_SUCCESS, "packet filled");

    /* NX_PACKET -> mbuf chain. */
    m = ami_mbuf_from_packet(packet, TX_TRUE);
    CHECK(m != NULL, "ami_mbuf_from_packet");
    if (m != NULL)
    {
        CHECK(ami_mbuf_length(m) == sizeof(data),   "mbuf chain length");
        CHECK((m->m_flags & M_PKTHDR) != 0,         "pkthdr requested");
        CHECK(m->m_pkthdr.len == (LONG) sizeof(data), "pkthdr.len");
        CHECK(ami_mbuf_copydata(m, 0, (LONG) sizeof(out), out) == 0,
              "readback from the chain");
        CHECK(t_ramp_ok(out, (ULONG) sizeof(out), 17), "chain content matches");

        /* ...and back again. The point of the round trip is that the mbuf
           chain does NOT reference the packet: releasing the packet first
           must leave the chain intact. */
        nx_packet_release(packet);
        packet = NX_NULL;

        CHECK(ami_mbuf_copydata(m, 0, (LONG) sizeof(out), out) == 0,
              "chain survives the packet being released");
        CHECK(t_ramp_ok(out, (ULONG) sizeof(out), 17), "chain content still matches");

        status =  ami_mbuf_to_packet(m, &t_pool, NX_NO_WAIT, &back);
        CHECK(status == NX_SUCCESS, "ami_mbuf_to_packet");
        if (status == NX_SUCCESS)
        {
            ULONG copied = 0;

            CHECK(back->nx_packet_length == sizeof(data), "packet length");
            status = nx_packet_data_retrieve(back, out, &copied);
            CHECK(status == NX_SUCCESS, "packet readback");
            CHECK(copied == sizeof(data), "packet readback length");
            CHECK(t_ramp_ok(out, (ULONG) sizeof(out), 17), "packet content matches");
            nx_packet_release(back);
        }

        ami_mbuf_freem(m);
    }

    if (packet != NX_NULL)
    {
        nx_packet_release(packet);
    }

    CHECK(ami_mbuf_outstanding() == 0,          "no mbufs leaked");
    CHECK(ami_mbuf_clusters_outstanding() == 0, "no clusters leaked");
}


/* ------------------------------------------------------------- bpf tests -- */

static APTR t_iface_cookie = (APTR) "iface";

static VOID t_test_bpf_abi(VOID)
{

    t_log("bpf: the real 68k record layout");

    t_log("  sizeof(struct bpf_hdr) %ld  bh_hdrlen %ld  sizeof(struct bpf_insn) %ld",
          (ULONG) sizeof(struct bpf_hdr), (ULONG) AMI_BPF_HDRLEN,
          (ULONG) sizeof(struct bpf_insn));

    CHECK(sizeof(struct bpf_hdr) == 18,     "sizeof(struct bpf_hdr) is 18");
    CHECK(AMI_BPF_HDRLEN == 20,             "bh_hdrlen is 20");
    CHECK(BPF_WORDALIGN(18) == 20,          "BPF_WORDALIGN(18) is 20");
    CHECK(sizeof(struct bpf_insn) == 8,     "sizeof(struct bpf_insn) is 8");
    CHECK(offsetof(struct bpf_hdr, bh_caplen)  == 8,  "bh_caplen at 8");
    CHECK(offsetof(struct bpf_hdr, bh_datalen) == 12, "bh_datalen at 12");
    CHECK(offsetof(struct bpf_hdr, bh_hdrlen)  == 16, "bh_hdrlen at 16");
}

static VOID t_test_bpf_vm(VOID)
{

UBYTE frame[128];
ULONG len;


    t_log("bpf: filter VM and validator on 68k");

    CHECK(ami_bpf_validate(t_prog_tcp80, T_TCP80_LEN) == 0,
          "libpcap 'tcp port 80' validates");

    len = t_make_tcp(frame, 1234, 80);
    CHECK(len == 60, "test frame is 60 bytes");
    CHECK(ami_bpf_filter(t_prog_tcp80, T_TCP80_LEN, frame, len, len) == 262144,
          "'tcp port 80' accepts a port-80 frame");

    len = t_make_tcp(frame, 4444, 22);
    CHECK(ami_bpf_filter(t_prog_tcp80, T_TCP80_LEN, frame, len, len) == 0,
          "'tcp port 80' rejects a port-22 frame");

    /* Big-endian packet assembly: the two-byte load at offset 12 must be
       0x0800, not 0x0008. */
    {
        static const struct bpf_insn p[] =
        {
            BPF_STMT(BPF_LD  | BPF_H | BPF_ABS, 12),
            BPF_STMT(BPF_RET | BPF_A, 0)
        };

        len = t_make_tcp(frame, 1234, 80);
        CHECK(ami_bpf_filter(p, 2, frame, len, len) == 0x0800,
              "packet bytes are assembled network order");
    }

    /* Out-of-range reads reject rather than trapping -- on a machine where an
       odd-address longword read is an address error, this matters. */
    {
        static const struct bpf_insn p[] =
        {
            BPF_STMT(BPF_LD  | BPF_W | BPF_ABS, 1000),
            BPF_STMT(BPF_RET | BPF_A, 0)
        };

        CHECK(ami_bpf_filter(p, 2, frame, len, len) == 0,
              "a read past the end rejects");
    }

    /* An unaligned longword read inside the frame must work: 68000 would take
       an address error, 68020 does not, and the interpreter assembles it from
       bytes either way. */
    {
        static const struct bpf_insn p[] =
        {
            BPF_STMT(BPF_LD  | BPF_W | BPF_ABS, 15),
            BPF_STMT(BPF_RET | BPF_A, 0)
        };
        ULONG expect = ((ULONG) frame[15] << 24) | ((ULONG) frame[16] << 16) |
                       ((ULONG) frame[17] << 8)  | (ULONG) frame[18];

        CHECK(ami_bpf_filter(p, 2, frame, len, len) == expect,
              "an odd-offset longword load reads correctly");
    }
}

/*
 * The validator's whole job is to stop a malformed filter program from
 * reaching the interpreter, and the interpreter's whole job is to survive one
 * anyway. On a machine with no memory protection there is no way to observe
 * the difference between "rejected the packet" and "corrupted something" from
 * inside the process -- so this runs under the crash guard, which turns a CPU
 * exception into a readable report instead of a dead emulator.
 *
 * Two batteries: hand-written programs aimed at each thing the interpreter
 * range-checks, then a few thousand pseudo-random instruction arrays. A random
 * `code` word hits undefined encodings in every class, and a random `k` hits
 * offsets far outside any packet.
 */
static VOID t_test_bpf_hostile(VOID)
{

static struct bpf_insn  prog[24];
UBYTE                   frame[64];
AmiBpfView              empty;
ULONG                   len;
ULONG                   seed = 0x13579BDFUL;
ULONG                   trial;
ULONG                   rejected = 0;
ULONG                   accepted = 0;
UWORD                   i;


    t_log("bpf: hostile filter programs under the crash guard");

    len = t_make_tcp(frame, 1234, 80);

    empty.wirelen = 0;
    empty.caplen  = 0;
    empty.nsegs   = 0;

    /* Each of these must be rejected by the validator AND survive the VM. */
    {
        static const struct bpf_insn bad_abs[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (LONG) 0x7FFFFFFFUL),
            BPF_STMT(BPF_RET | BPF_A, 0) };
        static const struct bpf_insn bad_neg[] = {
            BPF_STMT(BPF_LD | BPF_B | BPF_ABS, (LONG) -1),
            BPF_STMT(BPF_RET | BPF_A, 0) };
        static const struct bpf_insn bad_mem[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_MEM, 4000),
            BPF_STMT(BPF_RET | BPF_A, 0) };
        static const struct bpf_insn bad_st[] = {
            BPF_STMT(BPF_ST, (LONG) 0x40000000UL),
            BPF_STMT(BPF_RET | BPF_K, 0) };
        static const struct bpf_insn bad_ja[] = {
            BPF_STMT(BPF_JMP | BPF_JA, (LONG) 0xFFFFFFF0UL),
            BPF_STMT(BPF_RET | BPF_K, 0) };
        static const struct bpf_insn bad_jt[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_IMM, 0),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 250, 250),
            BPF_STMT(BPF_RET | BPF_K, 0) };
        static const struct bpf_insn bad_msh[] = {
            BPF_STMT(BPF_LDX | BPF_B | BPF_MSH, (LONG) 0xFFFFFFFFUL),
            BPF_STMT(BPF_LD  | BPF_W | BPF_IND, 0),
            BPF_STMT(BPF_RET | BPF_A, 0) };
        static const struct bpf_insn bad_code[] = {
            BPF_STMT(0xFFFF, (LONG) 0xFFFFFFFFUL),
            BPF_STMT(BPF_RET | BPF_K, 0) };
        static const struct bpf_insn no_ret[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_IMM, 1) };

        const struct bpf_insn *progs[9];
        ULONG                  lens[9];
        ULONG                  n;

        /*
         * The first three are LEGAL encodings carrying impossible packet
         * offsets. A packet offset is not something the validator can judge --
         * it depends on the frame, not the program -- so it must let them
         * through and the interpreter must be the thing that copes. Checking
         * that the validator does not over-reject matters as much as checking
         * that it rejects: a validator that refused these would make
         * "ldxb 4*([14]&0xf)" unloadable, and that is in every filter libpcap
         * emits for a TCP or UDP port.
         */
        progs[0] = bad_abs;  lens[0] = 2;
        progs[1] = bad_neg;  lens[1] = 2;
        progs[2] = bad_msh;  lens[2] = 3;

        /* These are malformed programs and must be refused at load time. */
        progs[3] = bad_mem;  lens[3] = 2;
        progs[4] = bad_st;   lens[4] = 2;
        progs[5] = bad_ja;   lens[5] = 2;
        progs[6] = bad_jt;   lens[6] = 3;
        progs[7] = bad_code; lens[7] = 2;
        progs[8] = no_ret;   lens[8] = 1;

        for (n = 0; n < 3; n++)
        {
            CHECK(ami_bpf_validate(progs[n], lens[n]) == 0,
                  "the validator accepts a legal program with a wild offset");
        }

        for (n = 3; n < 9; n++)
        {
            CHECK(ami_bpf_validate(progs[n], lens[n]) == -1,
                  "the validator refuses a malformed program");
        }

        if (!ami_crash_install())
        {
            const AmiCrashInfo *info = ami_crash_info();

            t_log("  FATAL: caught exception %ld (%s) at PC 0x%08lx",
                  info->number, ami_crash_name(info->number), info->pc);
            t_failures++;
            return;
        }

        for (n = 0; n < 9; n++)
        {
            (VOID) ami_bpf_filter(progs[n], lens[n], frame, len, len);
            (VOID) ami_bpf_filter(progs[n], lens[n], frame, len, 0);
            (VOID) ami_bpf_filter_view(progs[n], lens[n], &empty);
        }
        CHECK(TX_TRUE, "the interpreter survived every malformed program");
    }

    /* Pseudo-random programs. A 32-bit LCG is plenty: the point is coverage of
       undefined encodings, not statistical quality. */
    for (trial = 0; trial < 3000; trial++)
    {
        ULONG count;

        seed  = seed * 1103515245UL + 12345UL;
        count = 1UL + ((seed >> 16) % 16UL);

        for (i = 0; i < (UWORD) count; i++)
        {
            seed = seed * 1103515245UL + 12345UL;
            prog[i].code = (UWORD) (seed >> 13);
            seed = seed * 1103515245UL + 12345UL;
            prog[i].jt   = (UBYTE) (seed >> 20);
            prog[i].jf   = (UBYTE) (seed >> 12);
            seed = seed * 1103515245UL + 12345UL;
            prog[i].k    = (LONG) seed;
        }

        if (ami_bpf_validate(prog, count) == 0)
        {
            accepted++;
        }
        else
        {
            rejected++;
        }

        /* Run it whether or not the validator liked it. */
        (VOID) ami_bpf_filter(prog, count, frame, len, len);
        (VOID) ami_bpf_filter_view(prog, count, &empty);
    }

    ami_crash_remove();

    t_log("  3000 random programs: %ld rejected, %ld accepted, no exception",
          rejected, accepted);
    CHECK(rejected > 0, "the validator rejected some random programs");
    CHECK(TX_TRUE,      "the interpreter survived 3000 random programs");
}

static VOID t_test_bpf_capture(VOID)
{

UBYTE            frame[128];
UBYTE            out[512];
ULONG            len;
LONG             got;
const UBYTE     *rec;
ULONG            caplen;
ULONG            datalen;
ULONG            hdrlen;
ULONG            sec;
struct bpf_stat  st;
BYTE             sigbit;
ULONG            sigmask = 0;


    t_log("bpf: capture, record framing and Exec signal notification");

    CHECK(ami_bpf_init() == 0, "bpf init");
    CHECK(ami_bpf_attach_interface("eth0", t_iface_cookie, DLT_EN10MB, 1500,
                                   NULL) == 0, "interface attached");
    CHECK(ami_bpf_open(0) == 0,                             "channel open");
    CHECK(ami_bpf_ioctl(0, BIOCSETIF, (APTR) "eth0") == 0,  "BIOCSETIF");

    sigbit = AllocSignal(-1);
    if (sigbit != -1)
    {
        sigmask = 1UL << (ULONG) sigbit;
        (VOID) SetSignal(0UL, sigmask);
        CHECK(ami_bpf_set_notify_mask(0, sigmask) == 0, "notify mask set");
        {
            ULONG one = 1;

            CHECK(ami_bpf_ioctl(0, BIOCIMMEDIATE, &one) == 0, "BIOCIMMEDIATE");
        }
    }

    len = t_make_tcp(frame, 1234, 80);
    ami_bpf_tap_rx(t_iface_cookie, frame, len);
    ami_bpf_tap_rx(t_iface_cookie, frame, 61);

    if (sigbit != -1)
    {
        CHECK((SetSignal(0UL, 0UL) & sigmask) != 0,
              "the notify signal was delivered");
        (VOID) SetSignal(0UL, sigmask);
        FreeSignal(sigbit);
    }

    CHECK(ami_bpf_data_waiting(0) > 0, "data waiting");

    got = ami_bpf_read(0, out, (LONG) sizeof(out));

    rec     = out;
    caplen  = *(const ULONG *) (const void *) (rec + AMI_BPF_OFF_CAPLEN);
    datalen = *(const ULONG *) (const void *) (rec + AMI_BPF_OFF_DATALEN);
    hdrlen  = *(const UWORD *) (const void *) (rec + AMI_BPF_OFF_HDRLEN);
    sec     = *(const ULONG *) (const void *) (rec + AMI_BPF_OFF_TSTAMP_SEC);

    /* The fields are also readable through the NDK struct, which is the whole
       point of pinning the offsets. */
    {
        const struct bpf_hdr *bh = (const struct bpf_hdr *) (const void *) rec;

        CHECK(bh->bh_caplen  == caplen,  "bpf_hdr.bh_caplen agrees");
        CHECK(bh->bh_datalen == datalen, "bpf_hdr.bh_datalen agrees");
        CHECK(bh->bh_hdrlen  == hdrlen,  "bpf_hdr.bh_hdrlen agrees");
    }

    CHECK(hdrlen  == 20,        "record 0 bh_hdrlen is 20");
    CHECK(caplen  == 60,        "record 0 bh_caplen is 60");
    CHECK(datalen == 60,        "record 0 bh_datalen is 60");
    CHECK(sec >= AMI_BPF_AMIGA_EPOCH,
          "timestamp is past the Unix epoch, so GetSysTime() was adjusted");
    t_log("  bh_tstamp.tv_sec %ld", sec);

    {
        ULONG stride = BPF_WORDALIGN(hdrlen + caplen);
        ULONG i;
        UINT  same = TX_TRUE;

        CHECK(stride == 80, "record 0 stride is 80");

        for (i = 0; i < 60; i++)
        {
            if (out[hdrlen + i] != frame[i])
            {
                same = TX_FALSE;
            }
        }
        CHECK(same, "record 0 payload matches the frame");

        /* Record 1 sits exactly one stride on, and is the odd-length one, so
           its own stride carries the BPF_WORDALIGN pad. */
        rec    = out + stride;
        caplen = *(const ULONG *) (const void *) (rec + AMI_BPF_OFF_CAPLEN);
        hdrlen = *(const UWORD *) (const void *) (rec + AMI_BPF_OFF_HDRLEN);

        CHECK(caplen == 61,                          "record 1 bh_caplen is 61");
        CHECK(BPF_WORDALIGN(hdrlen + caplen) == 84,  "record 1 stride is 84");
        CHECK(got == (LONG) (stride + hdrlen + caplen),
              "read returned both records with no trailing pad");
    }

    CHECK(ami_bpf_read(0, out, (LONG) sizeof(out)) == 0, "nothing left to read");

    CHECK(ami_bpf_ioctl(0, BIOCGSTATS, &st) == 0, "BIOCGSTATS");
    CHECK(st.bs_recv == 2,  "two frames seen");
    CHECK(st.bs_drop == 0,  "none dropped");

    /* A real filter over the tap. */
    {
        struct bpf_program prog;

        prog.bf_len   = T_TCP80_LEN;
        prog.bf_insns = (struct bpf_insn *) t_prog_tcp80;
        CHECK(ami_bpf_ioctl(0, BIOCSETF, &prog) == 0, "BIOCSETF");

        len = t_make_tcp(frame, 4444, 22);
        ami_bpf_tap_rx(t_iface_cookie, frame, len);
        CHECK(ami_bpf_data_waiting(0) == 0, "filtered frame not captured");

        len = t_make_tcp(frame, 1234, 80);
        ami_bpf_tap_rx(t_iface_cookie, frame, len);
        CHECK(ami_bpf_data_waiting(0) > 0,  "matching frame captured");
        CHECK(ami_bpf_read(0, out, (LONG) sizeof(out)) == (LONG) (20 + 60),
              "one record read back");
    }

    CHECK(ami_bpf_close(0) == 0, "channel closed");
    ami_bpf_detach_interface(t_iface_cookie);
}

static VOID t_test_bpf_tap_tx(VOID)
{

NX_PACKET *packet = NX_NULL;
UBYTE      payload[100];
UBYTE      out[256];
UINT       status;
LONG       got;
ULONG      hdrlen;
ULONG      caplen;
const UBYTE *data;
UINT       same = TX_TRUE;
ULONG      i;
static const UBYTE our_mac[6] = { 0x00, 0x80, 0x10, 0x44, 0x55, 0x66 };


    t_log("bpf: the transmit tap synthesises the link header");

    if (!t_pool_ok)
    {
        t_log("  SKIP: no packet pool");
        return;
    }

    CHECK(ami_bpf_init() == 0, "bpf init");
    CHECK(ami_bpf_attach_interface("eth0", t_iface_cookie, DLT_EN10MB, 1500,
                                   NULL) == 0, "interface attached");
    CHECK(ami_bpf_open(0) == 0,                            "channel open");
    CHECK(ami_bpf_ioctl(0, BIOCSETIF, (APTR) "eth0") == 0, "BIOCSETIF");

    t_ramp(payload, (ULONG) sizeof(payload), 90);

    status =  nx_packet_allocate(&t_pool, &packet, NX_IP_PACKET, NX_NO_WAIT);
    CHECK(status == NX_SUCCESS, "packet allocated");
    if (status != NX_SUCCESS)
    {
        (VOID) ami_bpf_close(0);
        ami_bpf_detach_interface(t_iface_cookie);
        return;
    }

    status =  nx_packet_data_append(packet, payload, (ULONG) sizeof(payload),
                                    &t_pool, NX_NO_WAIT);
    CHECK(status == NX_SUCCESS, "packet filled");

    /*
     * Exactly the call src/sana2/sana2_tx.c must make, with the arguments it
     * has in hand: cooked mode (has_link_header FALSE), the EtherType from the
     * driver command, the destination from
     * nx_ip_driver_physical_address_msw/lsw, and our own MAC.
     */
    ami_bpf_tap_tx(t_iface_cookie, packet, TX_FALSE, 0x0800,
                   0x00000080UL,            /* 00:80 */
                   0x10112233UL,            /* 10:11:22:33 */
                   our_mac);

    got = ami_bpf_read(0, out, (LONG) sizeof(out));

    hdrlen = *(const UWORD *) (const void *) (out + AMI_BPF_OFF_HDRLEN);
    caplen = *(const ULONG *) (const void *) (out + AMI_BPF_OFF_CAPLEN);

    CHECK(hdrlen == 20, "bh_hdrlen is 20");
    CHECK(caplen == sizeof(payload) + 14,
          "captured frame is the payload plus a synthesised link header");
    CHECK(got == (LONG) (hdrlen + caplen), "read returned the whole record");

    data = out + hdrlen;

    /* The header the tap invented, byte for byte. */
    CHECK(data[0] == 0x00 && data[1] == 0x80 && data[2] == 0x10 &&
          data[3] == 0x11 && data[4] == 0x22 && data[5] == 0x33,
          "destination address from msw/lsw");
    for (i = 0; i < 6; i++)
    {
        if (data[6 + i] != our_mac[i])
        {
            same = TX_FALSE;
        }
    }
    CHECK(same, "source address is our MAC");
    CHECK(data[12] == 0x08 && data[13] == 0x00, "EtherType is 0x0800");

    same = TX_TRUE;
    for (i = 0; i < sizeof(payload); i++)
    {
        if (data[14 + i] != payload[i])
        {
            same = TX_FALSE;
        }
    }
    CHECK(same, "payload follows the header unmodified");

    /* The packet itself must be untouched -- TCP hands the same NX_PACKET
       back for retransmission. */
    CHECK(packet->nx_packet_length == sizeof(payload),
          "the NX_PACKET was not modified");

    nx_packet_release(packet);

    CHECK(ami_bpf_close(0) == 0, "channel closed");
    ami_bpf_detach_interface(t_iface_cookie);
}


/* ------------------------------------------------------ ThreadX startup --- */

VOID tx_application_define(VOID *first_unused_memory)
{

UINT status;


    (VOID) first_unused_memory;

    nx_system_initialize();

    status =  nx_packet_pool_create(&t_pool, "mbuf/bpf test pool",
                                    T_PACKET_PAYLOAD,
                                    (VOID *) t_pool_memory,
                                    (ULONG) sizeof(t_pool_memory));
    t_pool_ok = (status == NX_SUCCESS) ? TX_TRUE : TX_FALSE;
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{

UINT        status;
TX_THREAD   main_thread;


    t_log("AmiNetXDuo -- milestone 7: mbuf_* and bpf_* on real 68k");

    ami_crash_set_reference((APTR) main, "mbuf_bpf_test");

    /*
     * The mbuf and BPF code needs nothing from ThreadX; only the NX_PACKET
     * bridge and the transmit tap do. Run the rest first so a kernel problem
     * cannot hide a layout problem.
     */
    CHECK(ami_mbuf_init(256, 8) == 0, "mbuf pool configured");

    t_test_layout();
    t_test_alignment();
    t_test_ops();
    t_test_bpf_abi();
    t_test_bpf_vm();
    t_test_bpf_hostile();
    t_test_bpf_capture();

    status =  tx_amiga_kernel_start();
    if (status != TX_SUCCESS)
    {
        t_log("FATAL: tx_amiga_kernel_start() = %ld", (ULONG) status);
        t_log("");
        t_log("%ld checks, %ld failures -- FAIL", t_checks, t_failures + 1);
        return (20);
    }

    status =  tx_amiga_adopt_thread(&main_thread, "mbuf/bpf test", 16);
    if (status != TX_SUCCESS)
    {
        t_log("FATAL: tx_amiga_adopt_thread() = %ld", (ULONG) status);
        t_log("");
        t_log("%ld checks, %ld failures -- FAIL", t_checks, t_failures + 1);
        return (20);
    }

    CHECK(t_pool_ok, "packet pool created");

    t_test_packet_bridge();
    t_test_bpf_tap_tx();

    (VOID) tx_amiga_orphan_thread(&main_thread);

    ami_bpf_cleanup();
    ami_mbuf_cleanup();
    CHECK(ami_alloc_count() == 0, "no allocations leaked");

    t_log("");
    t_log("%ld checks, %ld failures -- %s", t_checks, t_failures,
          (t_failures == 0UL) ? "PASS" : "FAIL");

    return ((t_failures == 0UL) ? 0 : 20);
}
