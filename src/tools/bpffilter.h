/*
 * bpffilter, a very small filter compiler: host, port, protocol, and NOT.
 *
 * The bpf ABI has no BIOCSSNAPLEN.  A filter program returns the number of
 * bytes of the frame to keep, so the snap length and the filter are one thing:
 * `BPF_RET|BPF_K, n` accepts every packet and truncates it to n, and anything
 * more selective is the same program with tests in front of it.  That is why
 * this exists at all -- a capture command cannot avoid emitting BPF.
 *
 * DELIBERATELY NOT a tcpdump expression compiler.  There is no grammar here,
 * no `and`, no `or`, no parentheses and no negation of a single term: the four
 * primitives below are keywords on the command line and they are ANDed, with
 * NOT inverting the whole verdict.  That covers "what is this program saying
 * to that machine", which is the question a capture is opened for, in
 * something a user types once and gets right.  A real expression compiler is a
 * parser, a register allocator and an optimiser, and every one of those is a
 * place to be wrong on a machine with no memory protection.
 *
 * WHAT THE PRIMITIVES MEAN
 *
 *   HOST    an IPv4 or IPv6 address.  Matches a frame whose IP source or
 *           destination is it, in that address's own family.  For an IPv4
 *           address it also matches ARP, on the sender or target protocol
 *           address, because a capture of a host that drops the ARP for that
 *           host is missing the half that explains the silence.
 *
 *   PORT    a TCP or UDP port, source or destination.  With no PROTO it
 *           accepts either protocol, as tcpdump's bare `port` does.
 *
 *   PROTO   TCP, UDP, ICMP, ARP, IP or IP6.  TCP, UDP and ICMP are matched in
 *           both families -- ICMP means ICMPv6 in an IPv6 frame -- and IP and
 *           IP6 are the families themselves.
 *
 *   NOT     inverts the verdict, and nothing else: it swaps the two return
 *           values and costs no instruction.
 *
 * WHAT IT DOES NOT SEE
 *
 *   An IPv4 fragment other than the first has no ports in it, so a PORT filter
 *   rejects it.  An IPv6 frame with extension headers in front of the
 *   transport header has its ports somewhere this does not look, and a PORT
 *   filter rejects that too.  Both are what tcpdump's own `port` primitive
 *   does, for the same reason: walking the chain is a loop, and the BPF VM has
 *   no backward jump.
 *
 * No Amiga headers, so the host test can compile it: a filter that accepts
 * everything reads exactly like one that works, and every mistake in here is
 * a capture that is quietly missing frames.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BPFFILTER_H
#define AMINETXDUO_BPFFILTER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * struct bpf_insn, as include/aminetxduo/bpf.h lays it out.  Restated here
 * rather than included for the reason toolsock.h restates sockaddr_in: this
 * file must compile on a host that has no such header.  `long` is the field's
 * type on the target, where it is 32 bits, and the only thing the target ever
 * does with this array is hand it to BIOCSETF.
 */
typedef struct ToolBpfInsn
{
    unsigned short  code;
    unsigned char   jt;
    unsigned char   jf;
    long            k;
} ToolBpfInsn;

/*
 * The classic BPF encoding, spelled out because <net/bpf.h> is not on this
 * machine.  Class in the low three bits, then size or op, then mode or source.
 */
#define TOOL_BPF_LD         0x00
#define TOOL_BPF_LDX        0x01
#define TOOL_BPF_ALU        0x04
#define TOOL_BPF_JMP        0x05
#define TOOL_BPF_RET        0x06

#define TOOL_BPF_W          0x00
#define TOOL_BPF_H          0x08
#define TOOL_BPF_B          0x10

#define TOOL_BPF_ABS        0x20
#define TOOL_BPF_IND        0x40
#define TOOL_BPF_MSH        0xa0

#define TOOL_BPF_JA         0x00
#define TOOL_BPF_JEQ        0x10
#define TOOL_BPF_JSET       0x40

#define TOOL_BPF_K          0x00

/* The one-instruction program: accept everything, keep `n` bytes. */
#define TOOL_BPF_RET_K      (TOOL_BPF_RET | TOOL_BPF_K)

/* Longest program this can produce, with room to spare.  BPF_MAXINSNS is 512. */
#define TOOL_BPF_MAX_INSNS  64

enum ToolBpfProto
{
    TOOL_BPF_PROTO_ANY = 0,
    TOOL_BPF_PROTO_TCP,
    TOOL_BPF_PROTO_UDP,
    TOOL_BPF_PROTO_ICMP,
    TOOL_BPF_PROTO_ARP,
    TOOL_BPF_PROTO_IP,          /* IPv4 and nothing else                    */
    TOOL_BPF_PROTO_IP6
};

typedef struct ToolBpfFilter
{
    unsigned long   snaplen;        /* bytes of each frame to keep          */

    int             proto;          /* one of ToolBpfProto                  */

    int             have_host;
    int             host_is_v6;
    unsigned long   host_v4;        /* host byte order                      */
    unsigned char   host_v6[16];    /* the sixteen wire bytes               */

    int             have_port;
    unsigned long   port;

    int             invert;         /* NOT                                  */
} ToolBpfFilter;

typedef enum ToolBpfResult
{
    TOOL_BPF_OK = 0,
    TOOL_BPF_ERR_SNAP,          /* a snap length of 0, or past 65535        */
    TOOL_BPF_ERR_PORT,          /* a port past 65535                        */
    TOOL_BPF_ERR_IMPOSSIBLE,    /* the combination can never match a frame  */
    TOOL_BPF_ERR_SPACE          /* the program did not fit                  */
} ToolBpfResult;

/*
 * Compile `f` into `out`, which holds `max` instructions, and write the length
 * to `*count`.  The program only ever jumps forward and always ends in a
 * return, which is what the library's own validator insists on.
 */
ToolBpfResult tool_bpf_compile(const ToolBpfFilter *f, ToolBpfInsn *out,
                               unsigned long max, unsigned long *count);

/* A sentence for the user.  Never NULL. */
const char *tool_bpf_error(ToolBpfResult why);

/* "tcp" -> TOOL_BPF_PROTO_TCP, case-insensitively.  -1 for anything else. */
int tool_bpf_proto_from_name(const char *name);

/* The other direction, for the line the command prints.  Never NULL. */
const char *tool_bpf_proto_name(int proto);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_BPFFILTER_H */
