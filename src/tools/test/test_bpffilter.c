/*
 * NetCapture's filter compiler, run through the real interpreter.
 *
 * The programs bpffilter.c emits are executed here by src/bpf/bpf_filter.c and
 * checked by src/bpf/bpf_validate.c -- the same two files that will run them
 * on the machine.  Nothing is reimplemented: a second interpreter written for
 * a test agrees with the compiler and with nothing else, and both of them
 * could be wrong about the same offset.
 *
 * WHAT THERE IS TO GET WRONG, which is the whole reason this file exists:
 *
 *   A filter that accepts everything reads exactly like one that works.  The
 *   user's capture is full of packets, the counters move, the file opens in
 *   Wireshark, and the answer to "what is this program saying to that machine"
 *   is the whole segment.  So every case below asserts BOTH directions: the
 *   frame that must be kept and the frame that must not.
 *
 *   The other silent failure is the opposite: a filter that matches nothing.
 *   An empty pcap looks like a quiet network.
 *
 *   And a program with a jump that runs off its own end is neither -- it is a
 *   BIOCSETF that fails, or, on a machine with no memory protection and a
 *   validator that let it through, something much worse.  Every program is
 *   handed to ami_bpf_validate() first.
 *
 * Frames are built here rather than captured, so an offset can be moved by one
 * byte and the test says which field stopped matching.
 *
 * Sabotage: tests/tools/bpffilter-verdict-selftest.sh breaks bpffilter.c in
 * six named ways and requires this to fail on each.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "aminetxduo/bpf.h"

#include "bpffilter.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

#define SNAP    96UL

/* -------------------------------------------------------------- the frames */

/*
 * A frame is built field by field.  60 bytes of Ethernet + IPv4 + TCP, or
 * whatever the case needs; the interpreter is told the wire length separately
 * so a truncated capture can be presented as one.
 */
#define FRAME_MAX   256

typedef struct Frame
{
    unsigned char bytes[FRAME_MAX];
    unsigned long len;
} Frame;

static void frame_init(Frame *f, unsigned long ethertype)
{
    memset(f->bytes, 0, sizeof(f->bytes));

    /* Two made-up MACs, so nothing here can match by accident. */
    f->bytes[0] = 0x00; f->bytes[1] = 0x80; f->bytes[2] = 0x10;
    f->bytes[3] = 0x11; f->bytes[4] = 0x22; f->bytes[5] = 0x33;
    f->bytes[6] = 0x02; f->bytes[7] = 0x41; f->bytes[8] = 0x4d;
    f->bytes[9] = 0x49; f->bytes[10] = 0x55; f->bytes[11] = 0x66;

    f->bytes[12] = (unsigned char)((ethertype >> 8) & 0xff);
    f->bytes[13] = (unsigned char)(ethertype & 0xff);

    f->len = 14;
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

static void put16(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 8) & 0xff);
    p[1] = (unsigned char)(v & 0xff);
}

/*
 * IPv4 with a header of `ihl` longwords, so the indexed port load is exercised
 * with something other than the usual 5.  `frag` goes straight into the flags
 * and fragment-offset halfword.
 */
static void frame_ip4(Frame *f, unsigned long proto, unsigned long src,
                      unsigned long dst, unsigned long ihl, unsigned long frag)
{
    unsigned char *ip = f->bytes + 14;

    frame_init(f, 0x0800);

    ip = f->bytes + 14;
    ip[0] = (unsigned char)(0x40 | (ihl & 0x0f));
    ip[1] = 0;
    put16(ip + 2, 20 + 20);
    put16(ip + 4, 0x1234);
    put16(ip + 6, frag);
    ip[8] = 64;
    ip[9] = (unsigned char)proto;
    put16(ip + 10, 0);
    put32(ip + 12, src);
    put32(ip + 16, dst);

    f->len = 14 + (ihl * 4);
}

static void frame_ip4_ports(Frame *f, unsigned long proto, unsigned long src,
                            unsigned long dst, unsigned long ihl,
                            unsigned long sport, unsigned long dport)
{
    frame_ip4(f, proto, src, dst, ihl, 0);

    put16(f->bytes + 14 + (ihl * 4) + 0, sport);
    put16(f->bytes + 14 + (ihl * 4) + 2, dport);

    f->len = 14 + (ihl * 4) + 20;
}

static void frame_ip6(Frame *f, unsigned long next, const unsigned char *src,
                      const unsigned char *dst, unsigned long sport,
                      unsigned long dport)
{
    unsigned char *ip;

    frame_init(f, 0x86dd);

    ip = f->bytes + 14;
    ip[0] = 0x60;
    put16(ip + 4, 20);
    ip[6] = (unsigned char)next;
    ip[7] = 64;
    memcpy(ip + 8, src, 16);
    memcpy(ip + 24, dst, 16);

    put16(f->bytes + 14 + 40 + 0, sport);
    put16(f->bytes + 14 + 40 + 2, dport);

    f->len = 14 + 40 + 20;
}

static void frame_arp(Frame *f, unsigned long ptype, unsigned long spa,
                      unsigned long tpa)
{
    unsigned char *a;

    frame_init(f, 0x0806);

    a = f->bytes + 14;
    put16(a + 0, 1);                /* Ethernet                             */
    put16(a + 2, ptype);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, 1);                /* request                              */
    put32(a + 14, spa);
    put32(a + 24, tpa);

    f->len = 14 + 28;
}

/* ------------------------------------------------------------ the harness */

static struct bpf_insn prog[TOOL_BPF_MAX_INSNS];

/*
 * Compile, validate, and answer the length.  0 means the compiler refused, and
 * the caller says whether that was the point.
 *
 * ToolBpfInsn is copied into `struct bpf_insn` field by field rather than
 * cast: the two are the same eight bytes on the target and are not on a
 * 64-bit host, where a `long` is eight bytes on its own.
 */
static unsigned long build(const ToolBpfFilter *f, ToolBpfResult *why)
{
    ToolBpfInsn   mine[TOOL_BPF_MAX_INSNS];
    unsigned long n = 0;
    unsigned long i;

    *why = tool_bpf_compile(f, mine, (unsigned long)TOOL_BPF_MAX_INSNS, &n);
    if (*why != TOOL_BPF_OK)
        return 0;

    for (i = 0; i < n; i++)
    {
        prog[i].code = (UWORD)mine[i].code;
        prog[i].jt   = (UBYTE)mine[i].jt;
        prog[i].jf   = (UBYTE)mine[i].jf;
        prog[i].k    = (LONG)mine[i].k;
    }

    CHECK(ami_bpf_validate(prog, n) == 0,
          "the library's own validator refuses this program (%lu insns)", n);

    return n;
}

/* What the interpreter says about one frame. */
static unsigned long run(unsigned long n, const Frame *f)
{
    return ami_bpf_filter(prog, n, f->bytes, f->len, f->len);
}

static void filter_init(ToolBpfFilter *f)
{
    memset(f, 0, sizeof(*f));
    f->snaplen = SNAP;
    f->proto   = TOOL_BPF_PROTO_ANY;
}

/* Assert both directions at once: `keep` must be kept, `drop` must not. */
static void both(const char *what, const ToolBpfFilter *f, const Frame *keep,
                 const Frame *drop)
{
    ToolBpfResult why;
    unsigned long n = build(f, &why);

    if (n == 0)
    {
        CHECK(0, "%s: the compiler refused: %s", what, tool_bpf_error(why));
        return;
    }

    if (keep != 0)
        CHECK(run(n, keep) == SNAP, "%s: the frame that must be kept was "
              "dropped", what);

    if (drop != 0)
        CHECK(run(n, drop) == 0, "%s: the frame that must be dropped was "
              "kept", what);
}

/* ------------------------------------------------------------- no filter -- */

static void test_accept_everything(void)
{
    ToolBpfFilter f;
    ToolBpfResult why;
    unsigned long n;
    Frame         ip;
    Frame         arp;

    filter_init(&f);

    n = build(&f, &why);

    CHECK(n == 1, "the empty filter is %lu instructions, not 1", n);
    CHECK(prog[0].k == (LONG)SNAP,
          "the empty filter returns %ld, not the snap length", (long)prog[0].k);

    frame_ip4_ports(&ip, 6, 0x0a000001UL, 0x0a000002UL, 5, 1234, 80);
    frame_arp(&arp, 0x0800, 0x0a000001UL, 0x0a000002UL);

    CHECK(run(n, &ip) == SNAP, "the empty filter dropped an IPv4 frame");
    CHECK(run(n, &arp) == SNAP, "the empty filter dropped an ARP frame");
}

/*
 * The snap length is the filter's return value, so a wrong one is a capture
 * truncated to the wrong size rather than a capture that fails.
 */
static void test_snaplen_is_the_return(void)
{
    ToolBpfFilter f;
    ToolBpfResult why;
    unsigned long n;
    Frame         ip;

    filter_init(&f);
    f.snaplen = 1514;
    f.proto   = TOOL_BPF_PROTO_TCP;

    n = build(&f, &why);
    frame_ip4_ports(&ip, 6, 0x0a000001UL, 0x0a000002UL, 5, 1234, 80);

    CHECK(n > 0 && run(n, &ip) == 1514,
          "a snap length of 1514 came back as %lu", run(n, &ip));
}

/* --------------------------------------------------------------- protocol */

static void test_proto(void)
{
    ToolBpfFilter f;
    Frame         tcp4;
    Frame         udp4;
    Frame         icmp4;
    Frame         tcp6;
    Frame         icmp6;
    Frame         arp;

    frame_ip4_ports(&tcp4,  6, 0x0a000001UL, 0x0a000002UL, 5, 1234, 80);
    frame_ip4_ports(&udp4, 17, 0x0a000001UL, 0x0a000002UL, 5, 1234, 53);
    frame_ip4(&icmp4, 1, 0x0a000001UL, 0x0a000002UL, 5, 0);
    frame_arp(&arp, 0x0800, 0x0a000001UL, 0x0a000002UL);

    {
        static const unsigned char a[16] = {
            0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,0x01 };
        static const unsigned char b[16] = {
            0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,0x02 };

        frame_ip6(&tcp6, 6, a, b, 1234, 80);
        frame_ip6(&icmp6, 58, a, b, 0, 0);
    }

    filter_init(&f);
    f.proto = TOOL_BPF_PROTO_TCP;
    both("proto tcp, against udp", &f, &tcp4, &udp4);
    both("proto tcp, against arp", &f, &tcp6, &arp);

    filter_init(&f);
    f.proto = TOOL_BPF_PROTO_UDP;
    both("proto udp", &f, &udp4, &tcp4);

    /* ICMP means ICMPv6 in an IPv6 frame: one keyword, both families, which
       is what somebody asking to see the pings means. */
    filter_init(&f);
    f.proto = TOOL_BPF_PROTO_ICMP;
    both("proto icmp, v4", &f, &icmp4, &tcp4);
    both("proto icmp, v6", &f, &icmp6, &tcp6);

    filter_init(&f);
    f.proto = TOOL_BPF_PROTO_ARP;
    both("proto arp", &f, &arp, &tcp4);

    /* IP and IP6 are the families themselves, and exclude each other. */
    filter_init(&f);
    f.proto = TOOL_BPF_PROTO_IP;
    both("proto ip, against ip6", &f, &tcp4, &tcp6);
    both("proto ip, against arp", &f, &icmp4, &arp);

    filter_init(&f);
    f.proto = TOOL_BPF_PROTO_IP6;
    both("proto ip6", &f, &tcp6, &tcp4);
}

/* ------------------------------------------------------------------- host */

static void test_host_v4(void)
{
    ToolBpfFilter f;
    Frame         from;
    Frame         to;
    Frame         neither;
    Frame         arp_from;
    Frame         arp_to;
    Frame         arp_other;

    filter_init(&f);
    f.have_host = 1;
    f.host_v4   = 0xc0a80105UL;             /* 192.168.1.5 */

    frame_ip4_ports(&from,    6, 0xc0a80105UL, 0x08080808UL, 5, 1234, 80);
    frame_ip4_ports(&to,      6, 0x08080808UL, 0xc0a80105UL, 5, 80, 1234);
    frame_ip4_ports(&neither, 6, 0x08080808UL, 0x08080404UL, 5, 80, 1234);

    both("host, as source", &f, &from, &neither);
    both("host, as destination", &f, &to, &neither);

    /* An address that differs in the last octet only: a compare of three
       bytes, or of a halfword, passes every other case in this file. */
    {
        Frame nearly;

        frame_ip4_ports(&nearly, 6, 0xc0a80106UL, 0x08080808UL, 5, 1234, 80);
        both("host, one octet out", &f, &from, &nearly);
    }

    /* ARP for the same address, both ways round, and one for somebody else. */
    frame_arp(&arp_from,  0x0800, 0xc0a80105UL, 0x08080808UL);
    frame_arp(&arp_to,    0x0800, 0x08080808UL, 0xc0a80105UL);
    frame_arp(&arp_other, 0x0800, 0x08080808UL, 0x08080404UL);

    both("host, arp sender", &f, &arp_from, &arp_other);
    both("host, arp target", &f, &arp_to, &arp_other);

    /* ARP for a protocol that is not IPv4 has nothing at those offsets. */
    {
        Frame rarp;

        frame_arp(&rarp, 0x8035, 0xc0a80105UL, 0x08080808UL);
        both("host, arp for another protocol", &f, &arp_from, &rarp);
    }
}

static void test_host_v6(void)
{
    ToolBpfFilter f;
    Frame         from;
    Frame         to;
    Frame         neither;

    static const unsigned char want[16] = {
        0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,0x01 };
    static const unsigned char other[16] = {
        0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,0x02 };
    static const unsigned char third[16] = {
        0xfe,0x80,0,0,0,0,0,0,0x02,0x80,0x10,0xff,0xfe,0x32,0x33,0x34 };

    filter_init(&f);
    f.have_host  = 1;
    f.host_is_v6 = 1;
    memcpy(f.host_v6, want, 16);

    frame_ip6(&from, 6, want, other, 1234, 80);
    frame_ip6(&to, 6, other, want, 80, 1234);
    frame_ip6(&neither, 6, other, third, 80, 1234);

    both("host6, as source", &f, &from, &neither);
    both("host6, as destination", &f, &to, &neither);

    /*
     * Two addresses differing in the LAST word only.  The four-word compare
     * gives up on the source at the first mismatched word and tries the
     * destination; a compare that stops after the first word matches every
     * address in the same /32.
     */
    {
        Frame nearly;

        frame_ip6(&nearly, 6, other, third, 80, 1234);
        both("host6, last word only", &f, &from, &nearly);
    }
}

/* ------------------------------------------------------------------- port */

static void test_port(void)
{
    ToolBpfFilter f;
    Frame         src;
    Frame         dst;
    Frame         neither;

    filter_init(&f);
    f.have_port = 1;
    f.port      = 22;

    frame_ip4_ports(&src,     6, 0x0a000001UL, 0x0a000002UL, 5, 22, 4000);
    frame_ip4_ports(&dst,     6, 0x0a000001UL, 0x0a000002UL, 5, 4000, 22);
    frame_ip4_ports(&neither, 6, 0x0a000001UL, 0x0a000002UL, 5, 4000, 80);

    both("port, as source", &f, &src, &neither);
    both("port, as destination", &f, &dst, &neither);

    /* A bare PORT takes UDP too, as tcpdump's does. */
    {
        Frame udp;
        Frame icmp;

        frame_ip4_ports(&udp, 17, 0x0a000001UL, 0x0a000002UL, 5, 22, 4000);
        frame_ip4(&icmp, 1, 0x0a000001UL, 0x0a000002UL, 5, 0);

        both("port, over udp", &f, &udp, &neither);
        both("port, against icmp", &f, &src, &icmp);
    }

    /*
     * A header with options in it.  The transport starts at 14 + ihl*4, so a
     * port read at a fixed 34 finds the options instead and this frame is the
     * one that catches it.
     */
    {
        Frame options;
        Frame options_other;

        frame_ip4_ports(&options, 6, 0x0a000001UL, 0x0a000002UL, 8, 22, 4000);
        frame_ip4_ports(&options_other, 6, 0x0a000001UL, 0x0a000002UL, 8,
                        4000, 80);

        both("port, with IP options", &f, &options, &options_other);
    }

    /*
     * A fragment other than the first has no transport header at all.  The
     * bytes at the port offsets are payload, and a filter that reads them
     * matches on data.
     */
    {
        Frame frag;

        frame_ip4(&frag, 6, 0x0a000001UL, 0x0a000002UL, 5, 0x00b9);
        put16(frag.bytes + 34, 22);         /* payload that looks like 22   */
        put16(frag.bytes + 36, 4000);
        frag.len = 14 + 20 + 20;

        both("port, later fragment", &f, &src, &frag);
    }

    /* IPv6, at its own fixed offsets. */
    {
        Frame keep;
        Frame drop;
        static const unsigned char a[16] = {
            0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,0x01 };
        static const unsigned char b[16] = {
            0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,0x02 };

        frame_ip6(&keep, 6, a, b, 4000, 22);
        frame_ip6(&drop, 6, a, b, 4000, 80);

        both("port, over IPv6", &f, &keep, &drop);
    }
}

/* --------------------------------------------------------- the conjunction */

static void test_and(void)
{
    ToolBpfFilter f;
    Frame         wanted;
    Frame         right_host_wrong_port;
    Frame         right_port_wrong_host;
    Frame         right_pair_wrong_proto;

    filter_init(&f);
    f.proto     = TOOL_BPF_PROTO_TCP;
    f.have_host = 1;
    f.host_v4   = 0xc0a80105UL;
    f.have_port = 1;
    f.port      = 22;

    frame_ip4_ports(&wanted, 6, 0xc0a80105UL, 0x08080808UL, 5, 4000, 22);
    frame_ip4_ports(&right_host_wrong_port, 6, 0xc0a80105UL, 0x08080808UL, 5,
                    4000, 80);
    frame_ip4_ports(&right_port_wrong_host, 6, 0x08080808UL, 0x08080404UL, 5,
                    4000, 22);
    frame_ip4_ports(&right_pair_wrong_proto, 17, 0xc0a80105UL, 0x08080808UL, 5,
                    4000, 22);

    both("tcp and host and port", &f, &wanted, &right_host_wrong_port);
    both("tcp and host and port, wrong host", &f, &wanted,
         &right_port_wrong_host);
    both("tcp and host and port, wrong proto", &f, &wanted,
         &right_pair_wrong_proto);
}

/* -------------------------------------------------------------------- NOT */

static void test_not(void)
{
    ToolBpfFilter f;
    Frame         matching;
    Frame         other;

    filter_init(&f);
    f.have_port = 1;
    f.port      = 22;
    f.invert    = 1;

    frame_ip4_ports(&matching, 6, 0x0a000001UL, 0x0a000002UL, 5, 4000, 22);
    frame_ip4_ports(&other,    6, 0x0a000001UL, 0x0a000002UL, 5, 4000, 80);

    /* Inverted, so the frame that would have been kept is the one dropped. */
    both("not port 22", &f, &other, &matching);

    /*
     * And the frames the filter could not have matched at all -- an ARP, with
     * no ports in it -- come back on the accepting side.  An inversion that
     * only swapped the matching branch would drop them.
     */
    {
        ToolBpfResult why;
        unsigned long n = build(&f, &why);
        Frame         arp;

        frame_arp(&arp, 0x0800, 0x0a000001UL, 0x0a000002UL);

        CHECK(n > 0 && run(n, &arp) == SNAP,
              "NOT port dropped an ARP frame, which has no port to be");
    }
}

/* -------------------------------------------------------------- refusals */

static void test_refusals(void)
{
    ToolBpfFilter f;
    ToolBpfInsn   insns[TOOL_BPF_MAX_INSNS];
    unsigned long n;

    filter_init(&f);
    f.snaplen = 0;
    CHECK(tool_bpf_compile(&f, insns, TOOL_BPF_MAX_INSNS, &n) ==
              TOOL_BPF_ERR_SNAP,
          "a snap length of 0 was accepted");

    filter_init(&f);
    f.snaplen = 70000;
    CHECK(tool_bpf_compile(&f, insns, TOOL_BPF_MAX_INSNS, &n) ==
              TOOL_BPF_ERR_SNAP,
          "a snap length past 65535 was accepted");

    filter_init(&f);
    f.have_port = 1;
    f.port      = 70000;
    CHECK(tool_bpf_compile(&f, insns, TOOL_BPF_MAX_INSNS, &n) ==
              TOOL_BPF_ERR_PORT,
          "a port past 65535 was accepted");

    /* A port on a protocol that has none would compile into a filter that
       matches nothing, and an empty capture looks like a quiet network. */
    filter_init(&f);
    f.have_port = 1;
    f.port      = 22;
    f.proto     = TOOL_BPF_PROTO_ARP;
    CHECK(tool_bpf_compile(&f, insns, TOOL_BPF_MAX_INSNS, &n) ==
              TOOL_BPF_ERR_IMPOSSIBLE,
          "arp with a port was accepted");

    filter_init(&f);
    f.have_port = 1;
    f.port      = 22;
    f.proto     = TOOL_BPF_PROTO_ICMP;
    CHECK(tool_bpf_compile(&f, insns, TOOL_BPF_MAX_INSNS, &n) ==
              TOOL_BPF_ERR_IMPOSSIBLE,
          "icmp with a port was accepted");

    /* An IPv6 address on the IPv4 family, and the other way round. */
    filter_init(&f);
    f.have_host  = 1;
    f.host_is_v6 = 1;
    f.proto      = TOOL_BPF_PROTO_IP;
    CHECK(tool_bpf_compile(&f, insns, TOOL_BPF_MAX_INSNS, &n) ==
              TOOL_BPF_ERR_IMPOSSIBLE,
          "an IPv6 host with PROTO IP was accepted");

    filter_init(&f);
    f.have_host = 1;
    f.host_v4   = 0x01020304UL;
    f.proto     = TOOL_BPF_PROTO_IP6;
    CHECK(tool_bpf_compile(&f, insns, TOOL_BPF_MAX_INSNS, &n) ==
              TOOL_BPF_ERR_IMPOSSIBLE,
          "an IPv4 host with PROTO IP6 was accepted");

    /* Nothing to invert. */
    filter_init(&f);
    f.invert = 1;
    CHECK(tool_bpf_compile(&f, insns, TOOL_BPF_MAX_INSNS, &n) ==
              TOOL_BPF_ERR_IMPOSSIBLE,
          "NOT with no filter was accepted");

    /* A buffer too small to hold the program, refused rather than overrun. */
    filter_init(&f);
    f.proto      = TOOL_BPF_PROTO_TCP;
    f.have_host  = 1;
    f.host_is_v6 = 1;
    f.have_port  = 1;
    f.port       = 22;
    CHECK(tool_bpf_compile(&f, insns, 4, &n) == TOOL_BPF_ERR_SPACE,
          "a four-instruction buffer took the whole program");
    CHECK(n == 0, "a refused compile still reported %lu instructions", n);
}

/* Names, which is what the user types. */
static void test_names(void)
{
    CHECK(tool_bpf_proto_from_name("tcp") == TOOL_BPF_PROTO_TCP, "tcp");
    CHECK(tool_bpf_proto_from_name("TCP") == TOOL_BPF_PROTO_TCP, "TCP");
    CHECK(tool_bpf_proto_from_name("Udp") == TOOL_BPF_PROTO_UDP, "Udp");
    CHECK(tool_bpf_proto_from_name("icmp") == TOOL_BPF_PROTO_ICMP, "icmp");
    CHECK(tool_bpf_proto_from_name("arp") == TOOL_BPF_PROTO_ARP, "arp");
    CHECK(tool_bpf_proto_from_name("ip") == TOOL_BPF_PROTO_IP, "ip");
    CHECK(tool_bpf_proto_from_name("ip6") == TOOL_BPF_PROTO_IP6, "ip6");
    CHECK(tool_bpf_proto_from_name("any") == TOOL_BPF_PROTO_ANY, "any");

    /* "ipv6" is what a user types and is not a spelling this accepts; it must
       be refused rather than read as "ip". */
    CHECK(tool_bpf_proto_from_name("ipv6") == -1, "ipv6 was accepted");
    CHECK(tool_bpf_proto_from_name("") == -1, "the empty name was accepted");
    CHECK(tool_bpf_proto_from_name("tcpx") == -1, "tcpx was accepted");
}

/*
 * A short frame must not make the interpreter read past it, and the point at
 * which a filter starts accepting is a fact about the program worth pinning.
 *
 * The deepest byte a `tcp and host and port` program over IPv4 reads is the
 * destination port, at offsets 36 and 37: 14 of Ethernet, a 20-byte header,
 * and the port pair.  So 38 bytes is exactly enough and 37 is not.  A filter
 * whose loads moved -- an off-by-one in an offset, a port read at a fixed 34
 * rather than through the index register -- moves this threshold, and every
 * whole-frame case in this file would still pass.
 */
#define RUNT_ENOUGH     38

static void test_runt_frames(void)
{
    ToolBpfFilter f;
    ToolBpfResult why;
    unsigned long n;
    Frame         whole;
    unsigned long len;

    filter_init(&f);
    f.proto     = TOOL_BPF_PROTO_TCP;
    f.have_host = 1;
    f.host_v4   = 0x0a000001UL;
    f.have_port = 1;
    f.port      = 22;

    n = build(&f, &why);
    CHECK(n > 0, "the filter did not compile");
    if (n == 0)
        return;

    frame_ip4_ports(&whole, 6, 0x0a000001UL, 0x0a000002UL, 5, 4000, 22);

    for (len = 0; len <= whole.len; len++)
    {
        unsigned long got = ami_bpf_filter(prog, n, whole.bytes, len, len);

        if (len < RUNT_ENOUGH)
            CHECK(got == 0,
                  "a %lu-byte frame was accepted, and the program reads to "
                  "offset %d", len, RUNT_ENOUGH - 1);
        else
            CHECK(got == SNAP,
                  "a %lu-byte frame was rejected, and %d bytes is every byte "
                  "the program reads", len, RUNT_ENOUGH);
    }
}

int main(void)
{
    test_accept_everything();
    test_snaplen_is_the_return();
    test_proto();
    test_host_v4();
    test_host_v6();
    test_port();
    test_and();
    test_not();
    test_refusals();
    test_names();
    test_runt_frames();

    printf("bpffilter: %d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
