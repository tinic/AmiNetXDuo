/*
 * AmiNetXDuo, the bpf_* raw packet path: capture channels plus a Berkeley
 * packet filter interpreter.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BPF_H
#define AMINETXDUO_BPF_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AMI_STATIC_ASSERT
#  if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#    define AMI_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#  else
#    define AMI_SA_CAT2(a, b) a##b
#    define AMI_SA_CAT(a, b)  AMI_SA_CAT2(a, b)
#    define AMI_STATIC_ASSERT(cond, msg) \
        typedef char AMI_SA_CAT(ami_static_assert_, __LINE__)[(cond) ? 1 : -1]
#  endif
#endif

/* ---------------------------------------------------------------- the ABI */

#ifdef AMI_BPF_REPLICA

/*
 * Host-test replica of the Roadshow NDK <net/bpf.h>. On a 64-bit host `struct
 * bpf_hdr` gains tail padding the 68k struct lacks: never use its sizeof, use
 * AMI_BPF_HDR_BYTES.
 */

#define BPF_ALIGNMENT       ((ULONG)sizeof(LONG))
#define BPF_WORDALIGN(x)    ((((ULONG)(x)) + (BPF_ALIGNMENT - 1)) & ~(BPF_ALIGNMENT - 1))

#define BPF_MAXINSNS        512
#define BPF_MAXBUFSIZE      0x8000
#define BPF_MINBUFSIZE      32

struct bpf_insn;

struct bpf_program {
    ULONG            bf_len;
    struct bpf_insn *bf_insns;
};

struct bpf_stat {
    ULONG bs_recv;
    ULONG bs_drop;
};

struct bpf_version {
    UWORD bv_major;
    UWORD bv_minor;
};

#define BPF_MAJOR_VERSION   1
#define BPF_MINOR_VERSION   1

/*
 * <sys/ioccom.h>, verbatim in effect. The undef must cover the whole block:
 * a host <sys/ioctl.h> supplies Linux's encoding, and leaving any IOC_*
 * direction bit behind yields silently wrong command numbers.
 */
#undef IOCPARM_MASK
#undef IOCPARM_LEN
#undef IOCBASECMD
#undef IOCGROUP
#undef IOC_VOID
#undef IOC_OUT
#undef IOC_IN
#undef IOC_INOUT
#undef IOC_DIRMASK
#undef _IOC
#undef _IO
#undef _IOR
#undef _IOW
#undef _IOWR

#define IOCPARM_MASK        0x1fff
#define IOCPARM_LEN(x)      (((x) >> 16) & IOCPARM_MASK)
#define IOCBASECMD(x)       ((x) & ~(IOCPARM_MASK << 16))
#define IOCGROUP(x)         (((x) >> 8) & 0xff)
#define IOC_VOID            (0x20000000UL)
#define IOC_OUT             (0x40000000UL)
#define IOC_IN              (0x80000000UL)
#define IOC_INOUT           (IOC_IN | IOC_OUT)
#define IOC_DIRMASK         (0xe0000000UL)
#define _IOC(inout, group, num, len) \
    ((ULONG)(inout) | (((ULONG)(len) & IOCPARM_MASK) << 16) | \
     (((ULONG)(group)) << 8) | (ULONG)(num))
#define _IO(g, n)           _IOC(IOC_VOID,  (g), (n), 0)
#define _IOR(g, n, t)       _IOC(IOC_OUT,   (g), (n), sizeof(t))
#define _IOW(g, n, t)       _IOC(IOC_IN,    (g), (n), sizeof(t))
#define _IOWR(g, n, t)      _IOC(IOC_INOUT, (g), (n), sizeof(t))

/* struct ifreq is 32 bytes with a 16-byte name (net/if.h); only its size
   enters the ioctl encoding. */
#define IFNAMSIZ            16
#define AMI_BPF_IFREQ_SIZE  32

#define BIOCGBLEN           _IOC(IOC_OUT,   'B', 102, 4)
#define BIOCSBLEN           _IOC(IOC_INOUT, 'B', 102, 4)
#define BIOCSETF            _IOC(IOC_IN,    'B', 103, 8)
#define BIOCFLUSH           _IOC(IOC_VOID,  'B', 104, 0)
#define BIOCPROMISC         _IOC(IOC_VOID,  'B', 105, 0)
#define BIOCGDLT            _IOC(IOC_OUT,   'B', 106, 4)
#define BIOCGETIF           _IOC(IOC_OUT,   'B', 107, AMI_BPF_IFREQ_SIZE)
#define BIOCSETIF           _IOC(IOC_IN,    'B', 108, AMI_BPF_IFREQ_SIZE)
#define BIOCSRTIMEOUT       _IOC(IOC_IN,    'B', 109, 8)
#define BIOCGRTIMEOUT       _IOC(IOC_OUT,   'B', 110, 8)
#define BIOCGSTATS          _IOC(IOC_OUT,   'B', 111, 8)
#define BIOCIMMEDIATE       _IOC(IOC_IN,    'B', 112, 4)
#define BIOCVERSION         _IOC(IOC_OUT,   'B', 113, 4)


struct ami_bpf_timeval {
    ULONG tv_sec;
    ULONG tv_usec;
};

struct bpf_hdr {
    struct ami_bpf_timeval bh_tstamp;
    ULONG                  bh_caplen;
    ULONG                  bh_datalen;
    UWORD                  bh_hdrlen;
};

#define DLT_NULL        0
#define DLT_EN10MB      1
#define DLT_EN3MB       2
#define DLT_AX25        3
#define DLT_PRONET      4
#define DLT_CHAOS       5
#define DLT_IEEE802     6
#define DLT_ARCNET      7
#define DLT_SLIP        8
#define DLT_PPP         9
#define DLT_FDDI        10

#define BPF_CLASS(code) ((code) & 0x07)
#define     BPF_LD      0x00
#define     BPF_LDX     0x01
#define     BPF_ST      0x02
#define     BPF_STX     0x03
#define     BPF_ALU     0x04
#define     BPF_JMP     0x05
#define     BPF_RET     0x06
#define     BPF_MISC    0x07

#define BPF_SIZE(code)  ((code) & 0x18)
#define     BPF_W       0x00
#define     BPF_H       0x08
#define     BPF_B       0x10
#define BPF_MODE(code)  ((code) & 0xe0)
#define     BPF_IMM     0x00
#define     BPF_ABS     0x20
#define     BPF_IND     0x40
#define     BPF_MEM     0x60
#define     BPF_LEN     0x80
#define     BPF_MSH     0xa0

#define BPF_OP(code)    ((code) & 0xf0)
#define     BPF_ADD     0x00
#define     BPF_SUB     0x10
#define     BPF_MUL     0x20
#define     BPF_DIV     0x30
#define     BPF_OR      0x40
#define     BPF_AND     0x50
#define     BPF_LSH     0x60
#define     BPF_RSH     0x70
#define     BPF_NEG     0x80
#define     BPF_JA      0x00
#define     BPF_JEQ     0x10
#define     BPF_JGT     0x20
#define     BPF_JGE     0x30
#define     BPF_JSET    0x40
#define BPF_SRC(code)   ((code) & 0x08)
#define     BPF_K       0x00
#define     BPF_X       0x08

#define BPF_RVAL(code)  ((code) & 0x18)
#define     BPF_A       0x10

#define BPF_MISCOP(code) ((code) & 0xf8)
#define     BPF_TAX     0x00
#define     BPF_TXA     0x80

struct bpf_insn {
    UWORD code;
    UBYTE jt;
    UBYTE jf;
    LONG  k;
};

#define BPF_STMT(code, k)           { (UWORD)(code), 0, 0, (LONG)(k) }
#define BPF_JUMP(code, k, jt, jf)   { (UWORD)(code), (UBYTE)(jt), (UBYTE)(jf), (LONG)(k) }

#define BPF_MEMWORDS    16

#else /* !AMI_BPF_REPLICA, the real thing */

/*
 * Include order is load-bearing: <devices/timer.h> first so bh_tstamp resolves
 * to the Amiga timeval, <sys/types.h> before <net/if.h> for ssize_t, and
 * <net/if.h> because sizeof(struct ifreq) is baked into the BIOC*IF encodings.
 */
#include <devices/timer.h>
#include <sys/types.h>
#include <net/if.h>
#include <net/bpf.h>

#define AMI_BPF_IFREQ_SIZE  ((ULONG)sizeof(struct ifreq))

#endif /* AMI_BPF_REPLICA */

/* <sys/filio.h>'s FIONREAD: the bpf_ioctl() request reporting the buffered
   byte count. bpf_data_waiting() is a 0/1 flag, not a count. */
#define AMI_BPF_FIONREAD    _IOC(IOC_OUT, 'f', 127, 4)

/* Must stay 4.4BSD's _IOWR('i', 33, struct ifreq), which is what the real
   <sys/sockio.h> gives, so the two agree in the bits AMI_BPF_CMD() keeps. */
#define AMI_BPF_SIOCGIFADDR _IOC(IOC_INOUT, 'i', 33, AMI_BPF_IFREQ_SIZE)

/* --------------------------------------------------- pinned record layout */

/*
 * Consumers step with BPF_WORDALIGN(bh_hdrlen + bh_caplen) and read data at
 * record + bh_hdrlen, so these must be exact. The implementation writes the
 * six fields at these offsets and never stores a `struct bpf_hdr` as a unit.
 */
#define AMI_BPF_OFF_TSTAMP_SEC   0
#define AMI_BPF_OFF_TSTAMP_USEC  4
#define AMI_BPF_OFF_CAPLEN       8
#define AMI_BPF_OFF_DATALEN     12
#define AMI_BPF_OFF_HDRLEN      16
#define AMI_BPF_HDR_BYTES       18                          /* sizeof on 68k */
#define AMI_BPF_HDRLEN          20                          /* == BPF_WORDALIGN(18) */

/* bh_tstamp is UNIX-epoch, not Amiga: add this to GetSysTime()'s seconds. */
#define AMI_BPF_AMIGA_EPOCH     252460800UL     /* 2922 * 86400 */

AMI_STATIC_ASSERT(AMI_BPF_HDRLEN == BPF_WORDALIGN(AMI_BPF_HDR_BYTES),
                  "bh_hdrlen must be the word-aligned header size");
AMI_STATIC_ASSERT(BPF_MEMWORDS == 16, "BPF_MEMWORDS");
AMI_STATIC_ASSERT(sizeof(struct bpf_insn) == 8, "bpf_insn size");

#ifndef AMI_BPF_REPLICA
AMI_STATIC_ASSERT(sizeof(struct bpf_hdr) == AMI_BPF_HDR_BYTES, "bpf_hdr size");
AMI_STATIC_ASSERT(BPF_ALIGNMENT == 4, "BPF_ALIGNMENT");
#endif

/* ------------------------------------------------------------- the filter */

/*
 * A packet as the interpreter sees it: up to AMI_BPF_MAX_SEGS logically
 * contiguous runs. `wirelen` is the whole frame (what BPF_LEN and bh_datalen
 * report); `caplen` is how much of it the segments actually cover.
 */
#define AMI_BPF_MAX_SEGS    8

typedef struct AmiBpfSeg
{
    const UBYTE *base;
    ULONG        len;
} AmiBpfSeg;

typedef struct AmiBpfView
{
    ULONG     wirelen;
    ULONG     caplen;
    UWORD     nsegs;
    AmiBpfSeg seg[AMI_BPF_MAX_SEGS];
} AmiBpfView;

/* Build a one-segment view over a contiguous frame. */
VOID ami_bpf_view_linear(AmiBpfView *view, const UBYTE *frame, ULONG len);

/* Append a segment. Returns -1 if the view is full. */
LONG ami_bpf_view_add(AmiBpfView *view, const UBYTE *base, ULONG len);

/*
 * Returns bytes of the packet to keep: 0 rejects, (ULONG)-1 accepts all, an
 * empty program accepts everything. Jump targets are NOT range-checked here:
 * the program must have passed ami_bpf_validate() first.
 */
ULONG ami_bpf_filter_view(const struct bpf_insn *insns, ULONG count,
                          const AmiBpfView *view);

/* Convenience wrapper over a contiguous buffer. */
ULONG ami_bpf_filter(const struct bpf_insn *insns, ULONG count,
                     const UBYTE *frame, ULONG wirelen, ULONG caplen);

/* Returns 0 to accept, -1 to reject. Stricter than 4.4BSD's bpf_validate:
   any encoding the interpreter does not implement is refused at load time. */
LONG ami_bpf_validate(const struct bpf_insn *insns, ULONG count);

/* ------------------------------------------------------------- lifecycle */

#ifndef AMI_BPF_MAX_CHANNELS
#define AMI_BPF_MAX_CHANNELS    4
#endif

/* Default channel buffer size, per buffer, and there are two of them. */
#ifndef AMI_BPF_DEFAULT_BLEN
#define AMI_BPF_DEFAULT_BLEN    4096
#endif

LONG ami_bpf_init(VOID);
VOID ami_bpf_cleanup(VOID);

/* ------------------------------------------------------------ status codes */

/*
 * Failure returns, negative throughout, with -1 the EINVAL catch-all. Only
 * src/bsdsocket/bpf.c maps these to errno; nothing else reads the value.
 */
#define AMI_BPF_EINVAL      (-1)
#define AMI_BPF_ENXIO       (-2)
#define AMI_BPF_EPERM       (-3)
#define AMI_BPF_EBUSY       (-4)
#define AMI_BPF_EINTR       (-5)
#define AMI_BPF_EIO         (-6)
#define AMI_BPF_ENOBUFS     (-7)
#define AMI_BPF_EMSGSIZE    (-8)

/* ------------------------------------------------------- the eight vectors */

/*
 * 1:1 with the bsdsocket.library LVOs. bpf_set_notify_mask takes its channel
 * in d1 and its mask in d0, the reverse of every other call in the group; the
 * generated vector must not tidy that away. `owner` is compared on every call.
 */

/*
 * Claim a channel: 0..AMI_BPF_MAX_CHANNELS-1 for a particular one, negative
 * for any free one. Returns the channel claimed, AMI_BPF_ENXIO out of range,
 * AMI_BPF_EBUSY if already open. Channel 0 is a valid handle; nothing is
 * captured until BIOCSETIF.
 */
LONG ami_bpf_open(APTR owner, LONG channel);

/* Release the channel, its buffers and its filter. */
LONG ami_bpf_close(APTR owner, LONG channel);

/* Close every channel this owner still holds. Frees memory and takes the
   channel lock; it does not block. */
VOID ami_bpf_close_owner(APTR owner);

/*
 * Copy out whole capture records; returns bytes copied. Non-blocking, 0 when
 * nothing is buffered. Only complete records are returned; a buffer too small
 * for the first pending record returns AMI_BPF_EINVAL and consumes nothing.
 */
LONG ami_bpf_read(APTR owner, LONG channel, APTR buffer, LONG len);

/*
 * Inject one frame. For DLT_EN10MB the buffer must start with a 14-byte
 * Ethernet header; it is stripped and the remainder goes out as the SANA-II
 * payload. Returns bytes accepted, or EMSGSIZE / ENOBUFS / ENXIO.
 */
LONG ami_bpf_write(APTR owner, LONG channel, APTR buffer, LONG len);

/* Signals sent to the calling task when a record lands. 0 disables. */
LONG ami_bpf_set_notify_mask(APTR owner, LONG channel, ULONG signal_mask);

/* The interrupt-time counterpart; the tap runs at task level here, so this is
   delivered by the same Signal() from the same context as the notify mask. */
LONG ami_bpf_set_interrupt_mask(APTR owner, LONG channel, ULONG signal_mask);

/*
 * Dispatch ignores the length field of the ioctl encoding and keys on
 * direction + group + number, so a caller built against a differently sized
 * struct ifreq still reaches the right handler.
 */
LONG ami_bpf_ioctl(APTR owner, LONG channel, ULONG command, APTR buffer);

/* 1 if anything is buffered, 0 if not, negative on a bad channel. A flag, not
   a count; for the byte count use AMI_BPF_FIONREAD. */
LONG ami_bpf_data_waiting(APTR owner, LONG channel);

/* How many channels are currently bound to an interface. */
UWORD ami_bpf_capturing(VOID);

/* ------------------------------------------------- interface registration */

/*
 * `cookie` is an opaque identity: whatever is passed here is what the taps
 * below must be given. `inject` may be NULL, in which case bpf_write() on a
 * channel bound to this interface fails.
 */
typedef LONG (*AmiBpfInjectFn)(APTR cookie, UWORD ether_type,
                               const UBYTE *dst, const UBYTE *payload,
                               ULONG len);

LONG ami_bpf_attach_interface(const char *name, APTR cookie, ULONG dlt,
                              ULONG mtu, AmiBpfInjectFn inject);

/* What address the interface behind `cookie` has right now, host order, or 0
   if it has none. Asked on every use, never cached: DHCP moves it. */
typedef ULONG (*AmiBpfAddrFn)(APTR cookie);

VOID ami_bpf_set_address_hook(AmiBpfAddrFn fn);

/* Unbinds any channel still pointing at it; the channels stay open. */
VOID ami_bpf_detach_interface(APTR cookie);

/* --------------------------------------------------------------- the taps */

/*
 * `cookie` must be the same pointer given to ami_bpf_attach_interface().
 * `has_link_header` says whether the 14 link-header bytes are already in the
 * packet (raw mode) or must be synthesised (cooked mode, the default).
 */

VOID ami_bpf_tap_rx(APTR cookie, const UBYTE *frame, ULONG len);

#ifdef NX_API_H
VOID ami_bpf_tap_tx(APTR cookie, NX_PACKET *packet, BOOL has_link_header,
                    UWORD ether_type, ULONG dst_msw, ULONG dst_lsw,
                    const UBYTE *src_mac);
#endif

/* The generic form, for a caller that already has a scatter view. */
VOID ami_bpf_tap_view(APTR cookie, const AmiBpfView *view);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_BPF_H */
