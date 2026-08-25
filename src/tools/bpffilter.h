/*
 * bpffilter, a very small filter compiler: HOST, PORT, PROTO and NOT, ANDed.
 * The bpf ABI has no BIOCSSNAPLEN -- a filter program's return value IS the
 * snap length. No Amiga headers, so the host test can compile it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BPFFILTER_H
#define AMINETXDUO_BPFFILTER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * struct bpf_insn, as include/aminetxduo/bpf.h lays it out. Restated rather
 * than included: this file must compile on a host that has no such header.
 * `long` is the field's type on the target, where it is 32 bits.
 */
typedef struct ToolBpfInsn
{
    unsigned short  code;
    unsigned char   jt;
    unsigned char   jf;
    long            k;
} ToolBpfInsn;

/* The classic BPF encoding, spelled out because <net/bpf.h> is not on this
   machine. Class in the low three bits, then size or op, then mode or source. */
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
