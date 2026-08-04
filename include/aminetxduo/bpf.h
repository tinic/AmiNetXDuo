/*
 * AmiNetXDuo, the bpf_* raw packet path: capture channels plus a Berkeley
 * packet filter interpreter.
 *
 * Two halves that only meet at one place (the tap):
 *
 *   1. Capture channels. Each open channel owns a pair of buffers holding
 *      captured frames in `struct bpf_hdr` form, with BPF_WORDALIGN padding
 *      between records, and is configured through bpf_ioctl() with the BIOC*
 *      commands. Readiness is signalled with an Exec signal mask rather than
 *      by blocking, which is what bpf_set_notify_mask()'s existence implies.
 *
 *   2. The filter VM. A classic BPF interpreter over `struct bpf_insn`, with a
 *      validator in front of it. Filter programs arrive from applications
 *      through BIOCSETF; on a machine with no memory protection a malformed
 *      one must be rejected, never merely survived.
 *
 * What a capture consumer sees is not the wire buffer.
 *
 * SANA-II is cooked (docs/RESEARCH.md 3.4): the device owns the link header.
 * The two directions are therefore not symmetric:
 *
 *   RX, src/sana2/sana2_rx.c has already synthesised a 14-byte Ethernet
 *         header from ios2_DstAddr / ios2_SrcAddr / ios2_PacketType by the
 *         time the frame reaches ami_sana2_rx_deliver(). Tapped there, the
 *         frame is already what a DLT_EN10MB consumer expects, in one
 *         contiguous run, and the filter reads it in place with no copy.
 *
 *   TX, in cooked mode no Ethernet header is ever built; the wire buffer
 *         starts at the IP or ARP header. ami_bpf_tap_tx() synthesises the 14
 *         bytes itself, from the same three facts SANA-II is about to be given
 *         (ios2_PacketType, ios2_DstAddr, and our own MAC as the source), and
 *         presents them to the filter as the first segment of a scatter view
 *         whose remaining segments are the NX_PACKET chain. The packet is never
 *         modified and never copied whole; only the captured prefix is copied,
 *         into the channel buffer.
 *
 * That is why the tap takes framing facts rather than just a pointer and a
 * length.
 *
 * Capture coverage. The tap sits on the stack's own frame path, so it sees
 * exactly what the stack sees and no more:
 *
 *   - Only EtherTypes the SANA-II shim posts a CMD_READ for: 0x0800, 0x0806,
 *     and 0x86DD in an IPv6 build. CMD_READ is per packet type and a SANA-II
 *     device drops anything with no matching read outstanding, so there is no
 *     "all types" read to post. Other protocols are invisible.
 *   - Only frames the device would have delivered to us anyway: our unicast,
 *     broadcast, and joined multicast. BIOCPROMISC is accepted and recorded,
 *     but SANA-II has no promiscuous-mode command, so it cannot be honoured.
 *     It returns success rather than failure because capture tools routinely
 *     abandon the interface when the call fails, and the partial capture is
 *     more useful than none.
 *   - Frames dropped by the device for want of an outstanding CMD_READ are
 *     invisible to us and are not counted in bs_drop, which counts only what
 *     the channel itself discarded for want of buffer space.
 *
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
 * Host-test replica of the Roadshow NDK <net/bpf.h> (Olaf Barthel 2001-2016,
 * over 4.4BSD's @(#)bpf.h 8.2 1/9/95). Structurally identical; on a 64-bit host
 * `struct bpf_hdr` gets two bytes of tail padding that the 68k struct does not
 * have, which is one reason nothing in the implementation uses
 * sizeof(struct bpf_hdr), see AMI_BPF_HDR_BYTES below.
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

/* <sys/ioccom.h>, verbatim in effect. */
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

/*
 * struct ifreq is 32 bytes with a 16-byte name (net/if.h). Only its size
 * enters the ioctl encoding, and only the name is ever read, so the replica
 * carries the size as a constant rather than the whole structure.
 */
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
 * Include order matters here, in two ways.
 *
 * <devices/timer.h> first, for `struct timeval`. bh_tstamp is declared as
 * `struct __timeval`, which <net/bpf.h> resolves to `struct timeval` or
 * `struct TimeVal` depending on __NEW_TIMEVAL_DEFINITION_USED__, and `TimeVal`
 * is an AmigaOS type from <devices/timer.h>, which settles that Roadshow means
 * the Amiga timeval and not a Unix one. Verified 2026-07-24 on this toolchain:
 * <devices/timer.h> and <sys/time.h> both give an 8-byte, 2-byte-aligned struct
 * with tv_secs/tv_micro (newlib's here is Amiga-adapted, with tv_sec/tv_secs
 * and tv_usec/tv_micro as union aliases), so `struct bpf_hdr` comes out 18
 * bytes either way. The assert below is kept anyway: a toolchain that
 * substituted a stock Unix timeval would change the record layout silently,
 * which is how <pwd.h> went wrong (docs/RESEARCH.md 3.3).
 *
 * <sys/types.h> before <net/if.h>, because <net/if.h> reaches <sys/socket.h>,
 * which uses ssize_t without declaring it on this newlib-based toolchain and
 * fails to compile on its own.
 *
 * <net/if.h> at all, because sizeof(struct ifreq) is baked into the
 * BIOCGETIF/BIOCSETIF ioctl encodings.
 */
#include <devices/timer.h>
#include <sys/types.h>
#include <net/if.h>
#include <net/bpf.h>

#define AMI_BPF_IFREQ_SIZE  ((ULONG)sizeof(struct ifreq))

#endif /* AMI_BPF_REPLICA */

/*
 * <sys/filio.h>'s FIONREAD, spelled out here because the BIOC* encodings come
 * from <net/bpf.h> and that header does not reach the file ioctls. This is the
 * bpf_ioctl() request that reports the buffered byte count; bpf_data_waiting()
 * is a 0/1 flag and not a count.
 */
#define AMI_BPF_FIONREAD    _IOC(IOC_OUT, 'f', 127, 4)

/*
 * libpcap asks a bpf handle for the interface's address, not a socket. Out
 * here with FIONREAD rather than among the BIOC* codes: those are inside the
 * host-test replica, and this one is needed by both builds. The encoding is
 * 4.4BSD's _IOWR('i', 33, struct ifreq), which is what the real <sys/sockio.h>
 * gives too, so the two agree in the bits AMI_BPF_CMD() keeps.
 */
#define AMI_BPF_SIOCGIFADDR _IOC(IOC_INOUT, 'i', 33, AMI_BPF_IFREQ_SIZE)

/* --------------------------------------------------- pinned record layout */

/*
 * Measured 2026-07-24 with m68k-amigaos-gcc 15.2.0 -m68020 against the NDK
 * <net/bpf.h>. On 68k a struct is 2-byte aligned, so `struct bpf_hdr` is 18
 * bytes, 8 for the timeval, 4 + 4 for the two lengths, 2 for bh_hdrlen,
 * and BPF_WORDALIGN rounds the header to 20, leaving two pad bytes before the
 * captured data. Consumers step to the next record with
 * BPF_WORDALIGN(bh_hdrlen + bh_caplen) and read the data at record +
 * bh_hdrlen, so both numbers have to be exactly these.
 *
 * The implementation writes the six fields at these offsets explicitly and
 * never stores a `struct bpf_hdr` as a unit, so the emitted bytes are right
 * whatever a given compiler decides to pad.
 */
#define AMI_BPF_OFF_TSTAMP_SEC   0
#define AMI_BPF_OFF_TSTAMP_USEC  4
#define AMI_BPF_OFF_CAPLEN       8
#define AMI_BPF_OFF_DATALEN     12
#define AMI_BPF_OFF_HDRLEN      16
#define AMI_BPF_HDR_BYTES       18                          /* sizeof on 68k */
#define AMI_BPF_HDRLEN          20                          /* == BPF_WORDALIGN(18) */

/*
 * bh_tstamp is seconds and microseconds since the UNIX epoch, not the Amiga
 * one, because that is what every capture consumer expects. GetSysTime()
 * reports seconds since 1978-01-01, so this is added on top: 8 years, of which
 * 1972 and 1976 were leap, so 2922 days.
 */
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
 * A packet as the interpreter sees it: up to AMI_BPF_MAX_SEGS runs of bytes
 * that are logically contiguous. One segment for a received frame (already
 * linear); two or more for a transmit, where segment 0 is the synthesised
 * link header and the rest is the NX_PACKET chain.
 *
 * `wirelen` is the length BPF_LEN reports and bh_datalen records: the whole
 * frame. `caplen` is how much of it the segments cover, which is the same
 * unless a truncated frame is being filtered.
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
 * Run a filter program. Returns the number of bytes of the packet to keep:
 * 0 rejects it, (ULONG)-1 accepts all of it. An empty program (count 0)
 * accepts everything, which is what BIOCSETF with bf_len 0 installs.
 *
 * The interpreter is total: every out-of-range packet read, every division by
 * zero and every scratch-memory index is checked at run time and rejects the
 * packet rather than trapping. It does not need a validated program to be safe,
 * but jump targets are not range-checked here, so a program that has not been
 * through ami_bpf_validate() can still run off the end of its own instruction
 * array. Validate first; BIOCSETF always does.
 */
ULONG ami_bpf_filter_view(const struct bpf_insn *insns, ULONG count,
                          const AmiBpfView *view);

/* Convenience wrapper over a contiguous buffer. */
ULONG ami_bpf_filter(const struct bpf_insn *insns, ULONG count,
                     const UBYTE *frame, ULONG wirelen, ULONG caplen);

/*
 * Reject anything the interpreter cannot execute safely: an empty or
 * oversized program, a backward or out-of-range jump, a scratch-memory index
 * past BPF_MEMWORDS, a constant division by zero, an unknown opcode, or a
 * program that does not end in a BPF_RET. Returns 0 to accept, -1 to reject.
 *
 * Stricter than 4.4BSD's bpf_validate, which lets several unknown encodings
 * through on the grounds that the interpreter will treat them as no-ops; here
 * anything the interpreter does not implement is refused at load time.
 */
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
 * What the channel calls return on failure. Negative throughout, so a caller
 * that only tests `< 0` is unaffected, and -1 stays the catch-all so an
 * unannotated failure still means EINVAL. src/bsdsocket/bpf.c turns these into
 * the errno the autodoc specifies for each call; nothing outside that mapping
 * should look at the value.
 *
 * AMI_BPF_EINTR comes from bpf_read() waiting out a BIOCSRTIMEOUT and finding
 * one of the caller's bpf_set_interrupt_mask() signals already set.
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
 * 1:1 with the bsdsocket.library LVOs. Register assignments, from the NDK
 * pragmas/bsdsocket_pragmas.h and confirmed against amitools'
 * bsdsocket_lib.fd and ndk-include/interfaces/bsdsocket.h:
 *
 *   0x16e bpf_open               (channel)             d0
 *   0x174 bpf_close              (channel)             d0
 *   0x17a bpf_read               (channel,buffer,len)  d0/a0/d1
 *   0x180 bpf_write              (channel,buffer,len)  d0/a0/d1
 *   0x186 bpf_set_notify_mask    (channel,signal_mask) d1/d0   <-- note
 *   0x18c bpf_set_interrupt_mask (channel,signal_mask) d0/d1
 *   0x192 bpf_ioctl              (channel,command,buf) d0/d1/a0
 *   0x198 bpf_data_waiting       (channel)             d0
 *
 * bpf_set_notify_mask takes its channel in d1 and its mask in d0, the reverse
 * of every other call in the group, including bpf_set_interrupt_mask. Both the
 * pragma and the .fd agree, so it is real and not a typo in one source. The
 * generated vector must not tidy it.
 *
 * Return values are 0 for success and a negative status for failure
 * throughout, except bpf_open (the channel claimed), bpf_read (bytes copied)
 * and bpf_write (bytes accepted). The LVOs report -1 for all of them.
 *
 * Every call takes an `owner`: an opaque token identifying the caller, which
 * for the LVOs is the per-opener SocketBase. The autodoc says the channel "will
 * be associated with the library base ... It will be automatically closed when
 * the library is closed", and specifies EPERM for every call made on someone
 * else's channel, so the token is compared on each one and
 * ami_bpf_close_owner() runs when a base goes away. A token of NULL is an owner
 * like any other, which is what the host tests use.
 */

/*
 * Claim a channel: 0..AMI_BPF_MAX_CHANNELS-1 for a particular one, negative
 * for any free one. Returns the channel claimed, AMI_BPF_ENXIO if the number
 * is out of range, or AMI_BPF_EBUSY if that channel, or, for the negative
 * form, every channel, is already open. A channel captures nothing until
 * BIOCSETIF.
 *
 * Roadshow's libpcap is the reference client for the negative form: it calls
 * bpf_open(-1) and then uses the return value as the channel for every call
 * that follows (docs/RESEARCH.md 55). That is also why channel 0 is a valid
 * handle here although the autodoc says "A channel handle > 0".
 */
LONG ami_bpf_open(APTR owner, LONG channel);

/* Release the channel, its buffers and its filter. */
LONG ami_bpf_close(APTR owner, LONG channel);

/*
 * Close every channel this owner still holds. Called when a library base
 * closes, so a program that exits without bpf_close() does not strand a
 * channel for the life of the stack. Frees memory and takes the channel lock;
 * it does not block.
 */
VOID ami_bpf_close_owner(APTR owner);

/*
 * Copy out whole capture records. Non-blocking: returns 0 when nothing is
 * buffered rather than waiting, because the Amiga idiom for waiting is the
 * signal mask set by bpf_set_notify_mask(), and blocking here would strand the
 * caller's other Exec signals. Returns the number of bytes copied.
 *
 * Only complete records are ever returned, so a consumer can walk the buffer
 * with BPF_WORDALIGN(bh_hdrlen + bh_caplen) without bounds surprises. If the
 * caller's buffer is too small for even the first pending record,
 * AMI_BPF_EINVAL is returned and nothing is consumed: ask BIOCGBLEN and size
 * the buffer to match, as capture libraries do.
 */
LONG ami_bpf_read(APTR owner, LONG channel, APTR buffer, LONG len);

/*
 * Inject one frame. For DLT_EN10MB the buffer must start with a 14-byte
 * Ethernet header; it is parsed for the destination address and EtherType and
 * the remainder goes out as the SANA-II payload, because in cooked mode the
 * device builds the header itself and would otherwise duplicate it.
 *
 * Returns the number of bytes accepted, AMI_BPF_EMSGSIZE past the MTU,
 * AMI_BPF_ENOBUFS if the interface refused it, or AMI_BPF_ENXIO if the channel
 * has no interface with an injector; see ami_bpf_attach_interface().
 */
LONG ami_bpf_write(APTR owner, LONG channel, APTR buffer, LONG len);

/* Signals sent to the calling task when a record lands. 0 disables. */
LONG ami_bpf_set_notify_mask(APTR owner, LONG channel, ULONG signal_mask);

/*
 * The interrupt-time counterpart. The tap runs at task level in this
 * implementation (the SANA-II readers are Tasks and the copy hooks do not call
 * it), so this mask is delivered by the same Signal() from the same context as
 * the notify mask. It is honoured, not ignored, but means nothing extra here.
 */
LONG ami_bpf_set_interrupt_mask(APTR owner, LONG channel, ULONG signal_mask);

/*
 * AMI_BPF_FIONREAD, BIOCGBLEN, BIOCSBLEN, BIOCSETF, BIOCFLUSH, BIOCPROMISC,
 * BIOCGDLT, BIOCGETIF, BIOCSETIF, BIOCSRTIMEOUT, BIOCGRTIMEOUT, BIOCGSTATS,
 * BIOCIMMEDIATE, BIOCVERSION. Anything else is AMI_BPF_EINVAL.
 *
 * Dispatch ignores the length field of the ioctl encoding and keys on
 * direction + group + number, so a caller built against a differently sized
 * struct ifreq still reaches the right handler.
 */
LONG ami_bpf_ioctl(APTR owner, LONG channel, ULONG command, APTR buffer);

/*
 * 1 if anything is buffered, 0 if not, negative on a bad channel. The autodoc
 * is explicit that this is a flag; for the byte count use AMI_BPF_FIONREAD.
 */
LONG ami_bpf_data_waiting(APTR owner, LONG channel);

/*
 * How many channels are currently bound to an interface, i.e. whether tapping
 * is worth any preparation. The two taps below check this themselves; this is
 * for a caller that has to build something, a scatter view over a packet
 * chain, before it can call one.
 */
UWORD ami_bpf_capturing(VOID);

/* ------------------------------------------------- interface registration */

/*
 * Called by whoever creates a SANA-II interface; the netstack bring-up code is
 * the natural place, since it holds both the configured name and the
 * AmiSana2If pointer. `cookie` is an opaque identity: whatever value is passed
 * here is the value the taps below must be given.
 *
 * `inject` may be NULL, in which case bpf_write() on a channel bound to this
 * interface fails. See the note in src/bpf/bpf_tap.c for the shape a SANA-II
 * side implementation takes.
 */
typedef LONG (*AmiBpfInjectFn)(APTR cookie, UWORD ether_type,
                               const UBYTE *dst, const UBYTE *payload,
                               ULONG len);

LONG ami_bpf_attach_interface(const char *name, APTR cookie, ULONG dlt,
                              ULONG mtu, AmiBpfInjectFn inject);

/*
 * What address the interface behind `cookie` has right now, host order, or 0
 * if it has none. Asked rather than stored: a DHCP lease changes it, and a
 * copy taken at attach would be stale the moment it did, and every future
 * path that moves an address would have to remember to update it.
 */
typedef ULONG (*AmiBpfAddrFn)(APTR cookie);

VOID ami_bpf_set_address_hook(AmiBpfAddrFn fn);

/* Unbinds any channel still pointing at it; the channels stay open. */
VOID ami_bpf_detach_interface(APTR cookie);

/* --------------------------------------------------------------- the taps */

/*
 * The two calls src/sana2/ has to add. Both are no-ops when no channel is
 * capturing on that interface, a load, a compare and a return.
 *
 * 1. Receive. In src/sana2/sana2_rx.c, at the top of ami_sana2_rx_deliver(),
 *    before the length check and before the link header is stripped:
 *
 *        ami_bpf_tap_rx(iface, packet->nx_packet_prepend_ptr,
 *                       packet->nx_packet_length);
 *
 *    That point is the convergence of the cooked and raw paths, so both are
 *    covered by the one call, and the frame there is a complete link-layer
 *    frame in one contiguous run.
 *
 * 2. Transmit. In src/sana2/sana2_tx.c, in ami_sana2_tx_send(), after the
 *    `if (iface->raw_mode)` block that may prepend a header and before
 *    `length = packet->nx_packet_length;`:
 *
 *        ami_bpf_tap_tx(iface, packet, iface->raw_mode, ether_type,
 *                       dst_msw, dst_lsw, iface->mac);
 *
 *    `has_link_header` tells the tap whether the 14 bytes are already in the
 *    packet (raw mode) or have to be synthesised (cooked mode, the default).
 *    Placing the call after the raw block means one call site serves both.
 *
 * Both take the same `iface` pointer that was passed to
 * ami_bpf_attach_interface() as `cookie`.
 */

VOID ami_bpf_tap_rx(APTR cookie, const UBYTE *frame, ULONG len);

#ifdef NX_API_H
VOID ami_bpf_tap_tx(APTR cookie, NX_PACKET *packet, BOOL has_link_header,
                    UWORD ether_type, ULONG dst_msw, ULONG dst_lsw,
                    const UBYTE *src_mac);
#endif

/*
 * The generic form, for a caller that already has a scatter view. Both taps
 * above funnel into this.
 */
VOID ami_bpf_tap_view(APTR cookie, const AmiBpfView *view);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_BPF_H */
