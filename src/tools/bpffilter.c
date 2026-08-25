/*
 * bpffilter, the compiler. See bpffilter.h for what the primitives mean.
 *
 * Each gate jumps to REJECT the moment it fails, so the gates are ANDed by
 * construction. Jumps name labels and are resolved once in tool_bpf_fixup(),
 * which is also the one place that can find an unplaced label or an offset
 * past 255.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bpffilter.h"

/* ------------------------------------------------------------- the frame */

#define ETH_TYPE_OFF        12
#define ETH_HDR_LEN         14

#define ETHERTYPE_IP        0x0800UL
#define ETHERTYPE_ARP       0x0806UL
#define ETHERTYPE_IP6       0x86ddUL

/* IPv4, from the start of the frame. */
#define IP4_FLAGS_OFF       (ETH_HDR_LEN + 6)    /* flags + fragment offset */
#define IP4_PROTO_OFF       (ETH_HDR_LEN + 9)
#define IP4_SRC_OFF         (ETH_HDR_LEN + 12)
#define IP4_DST_OFF         (ETH_HDR_LEN + 16)
#define IP4_FRAG_MASK       0x1fffUL

/* IPv6. */
#define IP6_NEXT_OFF        (ETH_HDR_LEN + 6)
#define IP6_SRC_OFF         (ETH_HDR_LEN + 8)
#define IP6_DST_OFF         (ETH_HDR_LEN + 24)
#define IP6_HDR_LEN         40

/* ARP, which is IPv4-over-Ethernet or this does not look at it. */
#define ARP_PTYPE_OFF       (ETH_HDR_LEN + 2)
#define ARP_SPA_OFF         (ETH_HDR_LEN + 14)
#define ARP_TPA_OFF         (ETH_HDR_LEN + 24)

/* IP protocol numbers. */
#define IPPROTO_ICMP_       1
#define IPPROTO_TCP_        6
#define IPPROTO_UDP_        17
#define IPPROTO_ICMPV6_     58

/* ------------------------------------------------------------ the builder */

#define LBL_MAX             16
#define LBL_NEXT            (-1)        /* fall through, no jump           */
#define LBL_UNPLACED        (-1L)

typedef struct ToolBpfBuild
{
    ToolBpfInsn    *out;
    unsigned long   max;
    unsigned long   n;

    long            pos[LBL_MAX];       /* instruction index, or UNPLACED  */
    int             nlabels;

    /* Per instruction, the label its jt and jf name.  LBL_NEXT for both on
       anything that is not a jump. */
    int             jt[TOOL_BPF_MAX_INSNS];
    int             jf[TOOL_BPF_MAX_INSNS];

    int             overflow;           /* ran out of room, or of labels   */
} ToolBpfBuild;

static int tool_bpf_label(ToolBpfBuild *b)
{
    if (b->nlabels >= LBL_MAX)
    {
        b->overflow = 1;
        return 0;                       /* a real index, so nothing indexes
                                           out of range while unwinding    */
    }

    b->pos[b->nlabels] = LBL_UNPLACED;

    return b->nlabels++;
}

static void tool_bpf_place(ToolBpfBuild *b, int label)
{
    if (label >= 0 && label < b->nlabels)
        b->pos[label] = (long)b->n;
}

static void tool_bpf_emit(ToolBpfBuild *b, unsigned short code, long k,
                          int jt, int jf)
{
    if (b->n >= b->max || b->n >= (unsigned long)TOOL_BPF_MAX_INSNS)
    {
        b->overflow = 1;
        return;
    }

    b->out[b->n].code = code;
    b->out[b->n].jt   = 0;
    b->out[b->n].jf   = 0;
    b->out[b->n].k    = k;

    b->jt[b->n] = jt;
    b->jf[b->n] = jf;

    b->n++;
}

/*
 * Labels into offsets. A jump is a count of instructions to skip from the one
 * after it, so the target must be ahead of the jump; the library's validator
 * rejects a program that tries otherwise.
 */
static int tool_bpf_fixup(ToolBpfBuild *b)
{
    unsigned long i;

    for (i = 0; i < b->n; i++)
    {
        long target;
        long delta;

        if ((b->out[i].code & 0x07) != TOOL_BPF_JMP)
            continue;

        if ((b->out[i].code & 0xf0) == TOOL_BPF_JA)
        {
            if (b->jt[i] < 0 || b->pos[b->jt[i]] == LBL_UNPLACED)
                return -1;

            delta = b->pos[b->jt[i]] - (long)(i + 1);
            if (delta < 0)
                return -1;

            b->out[i].k = delta;
            continue;
        }

        if (b->jt[i] == LBL_NEXT)
        {
            b->out[i].jt = 0;
        }
        else
        {
            target = b->pos[b->jt[i]];
            if (target == LBL_UNPLACED)
                return -1;
            delta = target - (long)(i + 1);
            if (delta < 0 || delta > 255)
                return -1;
            b->out[i].jt = (unsigned char)delta;
        }

        if (b->jf[i] == LBL_NEXT)
        {
            b->out[i].jf = 0;
        }
        else
        {
            target = b->pos[b->jf[i]];
            if (target == LBL_UNPLACED)
                return -1;
            delta = target - (long)(i + 1);
            if (delta < 0 || delta > 255)
                return -1;
            b->out[i].jf = (unsigned char)delta;
        }
    }

    return 0;
}

/* --------------------------------------------------------------- shorthand */

#define LDH_ABS(off)    tool_bpf_emit(b, \
    (unsigned short)(TOOL_BPF_LD | TOOL_BPF_H | TOOL_BPF_ABS), (long)(off), \
    LBL_NEXT, LBL_NEXT)

#define LDW_ABS(off)    tool_bpf_emit(b, \
    (unsigned short)(TOOL_BPF_LD | TOOL_BPF_W | TOOL_BPF_ABS), (long)(off), \
    LBL_NEXT, LBL_NEXT)

#define LDB_ABS(off)    tool_bpf_emit(b, \
    (unsigned short)(TOOL_BPF_LD | TOOL_BPF_B | TOOL_BPF_ABS), (long)(off), \
    LBL_NEXT, LBL_NEXT)

#define LDH_IND(off)    tool_bpf_emit(b, \
    (unsigned short)(TOOL_BPF_LD | TOOL_BPF_H | TOOL_BPF_IND), (long)(off), \
    LBL_NEXT, LBL_NEXT)

/* X = (frame[off] & 0x0f) * 4, which is the IPv4 header length in bytes. */
#define LDX_MSH(off)    tool_bpf_emit(b, \
    (unsigned short)(TOOL_BPF_LDX | TOOL_BPF_B | TOOL_BPF_MSH), (long)(off), \
    LBL_NEXT, LBL_NEXT)

#define JEQ(k, t, f)    tool_bpf_emit(b, \
    (unsigned short)(TOOL_BPF_JMP | TOOL_BPF_JEQ | TOOL_BPF_K), (long)(k), \
    (t), (f))

#define JSET(k, t, f)   tool_bpf_emit(b, \
    (unsigned short)(TOOL_BPF_JMP | TOOL_BPF_JSET | TOOL_BPF_K), (long)(k), \
    (t), (f))

#define JA(t)           tool_bpf_emit(b, \
    (unsigned short)(TOOL_BPF_JMP | TOOL_BPF_JA), 0L, (t), LBL_NEXT)

#define RET(k)          tool_bpf_emit(b, (unsigned short)TOOL_BPF_RET_K, \
    (long)(k), LBL_NEXT, LBL_NEXT)

/* ------------------------------------------------------------- the gates */

static unsigned long tool_bpf_v6_word(const unsigned char *a, int word)
{
    const unsigned char *p = a + (word * 4);

    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  | (unsigned long)p[3];
}

/*
 * "This frame carries the transport the user asked for." `any_of_two` is the
 * bare-PORT case: TCP and UDP both qualify and nothing else does.
 */
static void tool_bpf_gate_proto(ToolBpfBuild *b, int reject, int off,
                                int want, int tcp, int udp, int any_of_two)
{
    if (want < 0 && !any_of_two)
        return;

    LDB_ABS(off);

    if (any_of_two)
    {
        int ok = tool_bpf_label(b);

        JEQ(tcp, ok, LBL_NEXT);
        JEQ(udp, LBL_NEXT, reject);
        tool_bpf_place(b, ok);
        return;
    }

    JEQ(want, LBL_NEXT, reject);
}

/* Source or destination, one 32-bit compare each. */
static void tool_bpf_gate_host4(ToolBpfBuild *b, int reject,
                                unsigned long host, int src_off, int dst_off)
{
    int ok = tool_bpf_label(b);

    LDW_ABS(src_off);
    JEQ(host, ok, LBL_NEXT);
    LDW_ABS(dst_off);
    JEQ(host, LBL_NEXT, reject);
    tool_bpf_place(b, ok);
}

/* Sixteen bytes as four words. Any mismatch in the source tries the
   destination; any mismatch in the destination rejects. */
static void tool_bpf_gate_host6(ToolBpfBuild *b, int reject,
                                const unsigned char *host)
{
    int  dst = tool_bpf_label(b);
    int  ok  = tool_bpf_label(b);
    int  i;

    for (i = 0; i < 4; i++)
    {
        LDW_ABS(IP6_SRC_OFF + (i * 4));
        JEQ(tool_bpf_v6_word(host, i), (i == 3) ? ok : LBL_NEXT, dst);
    }

    tool_bpf_place(b, dst);

    for (i = 0; i < 4; i++)
    {
        LDW_ABS(IP6_DST_OFF + (i * 4));
        JEQ(tool_bpf_v6_word(host, i), LBL_NEXT, reject);
    }

    tool_bpf_place(b, ok);
}

/*
 * IPv4 ports. The header is a variable length, so X is loaded from the IHL
 * nibble and the two loads are indexed. A fragment other than the first carries
 * no transport header and is rejected before either load.
 */
static void tool_bpf_gate_port4(ToolBpfBuild *b, int reject, unsigned long port)
{
    int ok = tool_bpf_label(b);

    LDH_ABS(IP4_FLAGS_OFF);
    JSET(IP4_FRAG_MASK, reject, LBL_NEXT);

    LDX_MSH(ETH_HDR_LEN);
    LDH_IND(ETH_HDR_LEN + 0);
    JEQ(port, ok, LBL_NEXT);
    LDH_IND(ETH_HDR_LEN + 2);
    JEQ(port, LBL_NEXT, reject);

    tool_bpf_place(b, ok);
}

/* IPv6 ports, at a fixed offset because this does not walk extension headers. */
static void tool_bpf_gate_port6(ToolBpfBuild *b, int reject, unsigned long port)
{
    int ok = tool_bpf_label(b);

    LDH_ABS(ETH_HDR_LEN + IP6_HDR_LEN + 0);
    JEQ(port, ok, LBL_NEXT);
    LDH_ABS(ETH_HDR_LEN + IP6_HDR_LEN + 2);
    JEQ(port, LBL_NEXT, reject);

    tool_bpf_place(b, ok);
}

/* ------------------------------------------------------------- compilation */

ToolBpfResult tool_bpf_compile(const ToolBpfFilter *f, ToolBpfInsn *out,
                               unsigned long max, unsigned long *count)
{
    ToolBpfBuild  build;
    ToolBpfBuild *b = &build;
    int           accept;
    int           reject;
    int           l_v4;
    int           l_v6;
    int           l_arp;
    int           use_v4;
    int           use_v6;
    int           use_arp;
    unsigned long i;

    if (f == 0 || out == 0 || count == 0 || max == 0)
        return TOOL_BPF_ERR_SPACE;

    *count = 0;

    if (f->snaplen == 0 || f->snaplen > 65535UL)
        return TOOL_BPF_ERR_SNAP;

    if (f->have_port && f->port > 65535UL)
        return TOOL_BPF_ERR_PORT;

    /* A port on a protocol that has none is a typing mistake, not a filter that
       matches nothing quietly. */
    if (f->have_port &&
        (f->proto == TOOL_BPF_PROTO_ICMP || f->proto == TOOL_BPF_PROTO_ARP))
        return TOOL_BPF_ERR_IMPOSSIBLE;

    for (i = 0; i < (unsigned long)TOOL_BPF_MAX_INSNS; i++)
    {
        build.jt[i] = LBL_NEXT;
        build.jf[i] = LBL_NEXT;
    }
    for (i = 0; i < (unsigned long)LBL_MAX; i++)
        build.pos[i] = LBL_UNPLACED;

    build.out      = out;
    build.max      = max;
    build.n        = 0;
    build.nlabels  = 0;
    build.overflow = 0;

    /*
     * Nothing to test: one instruction. A NOT with nothing to invert would be
     * "capture nothing", which the caller refuses before this.
     */
    if (f->proto == TOOL_BPF_PROTO_ANY && !f->have_host && !f->have_port)
    {
        if (f->invert)
            return TOOL_BPF_ERR_IMPOSSIBLE;

        out[0].code = (unsigned short)TOOL_BPF_RET_K;
        out[0].jt   = 0;
        out[0].jf   = 0;
        out[0].k    = (long)f->snaplen;
        *count      = 1;

        return TOOL_BPF_OK;
    }

    /* Which branches can carry a frame the filter would accept. */
    use_v4 = !(f->proto == TOOL_BPF_PROTO_ARP ||
               f->proto == TOOL_BPF_PROTO_IP6 ||
               (f->have_host && f->host_is_v6));

    use_v6 = !(f->proto == TOOL_BPF_PROTO_ARP ||
               f->proto == TOOL_BPF_PROTO_IP ||
               (f->have_host && !f->host_is_v6));

    use_arp = !(f->proto == TOOL_BPF_PROTO_TCP ||
                f->proto == TOOL_BPF_PROTO_UDP ||
                f->proto == TOOL_BPF_PROTO_ICMP ||
                f->proto == TOOL_BPF_PROTO_IP ||
                f->proto == TOOL_BPF_PROTO_IP6 ||
                f->have_port ||
                (f->have_host && f->host_is_v6));

    if (!use_v4 && !use_v6 && !use_arp)
        return TOOL_BPF_ERR_IMPOSSIBLE;

    accept = tool_bpf_label(b);
    reject = tool_bpf_label(b);
    l_v4   = tool_bpf_label(b);
    l_v6   = tool_bpf_label(b);
    l_arp  = tool_bpf_label(b);

    /* The dispatch.  A family with no branch goes straight to the refusal. */
    LDH_ABS(ETH_TYPE_OFF);
    JEQ(ETHERTYPE_IP,  use_v4  ? l_v4  : reject, LBL_NEXT);
    JEQ(ETHERTYPE_IP6, use_v6  ? l_v6  : reject, LBL_NEXT);
    JEQ(ETHERTYPE_ARP, use_arp ? l_arp : reject, reject);

    if (use_v4)
    {
        tool_bpf_place(b, l_v4);

        switch (f->proto)
        {
        case TOOL_BPF_PROTO_TCP:
            tool_bpf_gate_proto(b, reject, IP4_PROTO_OFF, IPPROTO_TCP_,
                                0, 0, 0);
            break;
        case TOOL_BPF_PROTO_UDP:
            tool_bpf_gate_proto(b, reject, IP4_PROTO_OFF, IPPROTO_UDP_,
                                0, 0, 0);
            break;
        case TOOL_BPF_PROTO_ICMP:
            tool_bpf_gate_proto(b, reject, IP4_PROTO_OFF, IPPROTO_ICMP_,
                                0, 0, 0);
            break;
        default:
            if (f->have_port)
                tool_bpf_gate_proto(b, reject, IP4_PROTO_OFF, -1,
                                    IPPROTO_TCP_, IPPROTO_UDP_, 1);
            break;
        }

        if (f->have_host)
            tool_bpf_gate_host4(b, reject, f->host_v4, IP4_SRC_OFF,
                                IP4_DST_OFF);

        if (f->have_port)
            tool_bpf_gate_port4(b, reject, f->port);

        JA(accept);
    }

    if (use_v6)
    {
        tool_bpf_place(b, l_v6);

        switch (f->proto)
        {
        case TOOL_BPF_PROTO_TCP:
            tool_bpf_gate_proto(b, reject, IP6_NEXT_OFF, IPPROTO_TCP_,
                                0, 0, 0);
            break;
        case TOOL_BPF_PROTO_UDP:
            tool_bpf_gate_proto(b, reject, IP6_NEXT_OFF, IPPROTO_UDP_,
                                0, 0, 0);
            break;
        case TOOL_BPF_PROTO_ICMP:
            /* ICMPv6 is a different protocol number and, for a user asking to
               see the pings, the same thing. */
            tool_bpf_gate_proto(b, reject, IP6_NEXT_OFF, IPPROTO_ICMPV6_,
                                0, 0, 0);
            break;
        default:
            if (f->have_port)
                tool_bpf_gate_proto(b, reject, IP6_NEXT_OFF, -1,
                                    IPPROTO_TCP_, IPPROTO_UDP_, 1);
            break;
        }

        if (f->have_host)
            tool_bpf_gate_host6(b, reject, f->host_v6);

        if (f->have_port)
            tool_bpf_gate_port6(b, reject, f->port);

        JA(accept);
    }

    if (use_arp)
    {
        tool_bpf_place(b, l_arp);

        if (f->have_host)
        {
            /* An ARP whose protocol type is not IPv4 has no address here to
               compare and is not what a HOST filter is asking about. */
            LDH_ABS(ARP_PTYPE_OFF);
            JEQ(ETHERTYPE_IP, LBL_NEXT, reject);

            tool_bpf_gate_host4(b, reject, f->host_v4, ARP_SPA_OFF,
                                ARP_TPA_OFF);
        }

        JA(accept);
    }

    tool_bpf_place(b, accept);
    RET(f->invert ? 0UL : f->snaplen);

    tool_bpf_place(b, reject);
    RET(f->invert ? f->snaplen : 0UL);

    if (build.overflow)
        return TOOL_BPF_ERR_SPACE;

    if (tool_bpf_fixup(b) != 0)
        return TOOL_BPF_ERR_SPACE;

    *count = build.n;

    return TOOL_BPF_OK;
}

const char *tool_bpf_error(ToolBpfResult why)
{
    switch (why)
    {
    case TOOL_BPF_OK:
        return "no error";
    case TOOL_BPF_ERR_SNAP:
        return "the snap length must be between 1 and 65535 bytes";
    case TOOL_BPF_ERR_PORT:
        return "the port must be between 0 and 65535";
    case TOOL_BPF_ERR_IMPOSSIBLE:
        return "no frame can satisfy that combination, so it would capture "
               "nothing";
    case TOOL_BPF_ERR_SPACE:
        return "the filter did not fit in the program buffer";
    }

    return "the filter was refused";
}

static int tool_bpf_same(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = *a++;
        char cb = *b++;

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');

        if (ca != cb)
            return 0;
    }

    return *a == '\0' && *b == '\0';
}

int tool_bpf_proto_from_name(const char *name)
{
    if (name == 0)
        return -1;

    if (tool_bpf_same(name, "any"))     return TOOL_BPF_PROTO_ANY;
    if (tool_bpf_same(name, "tcp"))     return TOOL_BPF_PROTO_TCP;
    if (tool_bpf_same(name, "udp"))     return TOOL_BPF_PROTO_UDP;
    if (tool_bpf_same(name, "icmp"))    return TOOL_BPF_PROTO_ICMP;
    if (tool_bpf_same(name, "arp"))     return TOOL_BPF_PROTO_ARP;
    if (tool_bpf_same(name, "ip"))      return TOOL_BPF_PROTO_IP;
    if (tool_bpf_same(name, "ip6"))     return TOOL_BPF_PROTO_IP6;

    return -1;
}

const char *tool_bpf_proto_name(int proto)
{
    switch (proto)
    {
    case TOOL_BPF_PROTO_TCP:    return "tcp";
    case TOOL_BPF_PROTO_UDP:    return "udp";
    case TOOL_BPF_PROTO_ICMP:   return "icmp";
    case TOOL_BPF_PROTO_ARP:    return "arp";
    case TOOL_BPF_PROTO_IP:     return "ip";
    case TOOL_BPF_PROTO_IP6:    return "ip6";
    default:                    return "any";
    }
}
