/*
 * toolbpf, a capture channel from a command's side of the ABI.  See toolbpf.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolbpf.h"

/* --------------------------------------------------------------- the ABI */

/*
 * The bpf half of <net/bpf.h>, open-coded like toolsock.h's sockaddr_in, so
 * these commands build against a stock NDK.
 */
#define IOC_VOID_       0x20000000UL
#define IOC_OUT_        0x40000000UL
#define IOC_IN_         0x80000000UL
#define IOC_INOUT_      (IOC_IN_ | IOC_OUT_)
#define IOC_(io, g, n, l) \
    ((ULONG)(io) | ((((ULONG)(l)) & 0x1fffUL) << 16) | \
     (((ULONG)(g)) << 8) | (ULONG)(n))

#define FIONREAD_       IOC_(IOC_OUT_,   'f', 127, 4)
#define BIOCGBLEN_      IOC_(IOC_OUT_,   'B', 102, 4)
#define BIOCSBLEN_      IOC_(IOC_INOUT_, 'B', 102, 4)
#define BIOCSETF_       IOC_(IOC_IN_,    'B', 103, 8)
#define BIOCFLUSH_      IOC_(IOC_VOID_,  'B', 104, 0)
#define BIOCGDLT_       IOC_(IOC_OUT_,   'B', 106, 4)
#define BIOCSETIF_      IOC_(IOC_IN_,    'B', 108, 32)
#define BIOCSRTIMEOUT_  IOC_(IOC_IN_,    'B', 109, 8)
#define BIOCGSTATS_     IOC_(IOC_OUT_,   'B', 111, 8)
#define BIOCIMMEDIATE_  IOC_(IOC_IN_,    'B', 112, 4)

typedef struct ToolBpfProgram
{
    ULONG               bf_len;
    const ToolBpfInsn  *bf_insns;
} ToolBpfProgram;

/*
 * ToolBpfInsn is bpffilter.h's restatement of `struct bpf_insn`, written in
 * plain C types so that the compiler in bpffilter.c builds on a host.  What
 * BIOCSETF copies out of the array is the real thing, so the two layouts have
 * to agree, and on the target they do: LONG is 32 bits here.
 */
typedef char tool_bpf_insn_is_eight_bytes[
    (sizeof(ToolBpfInsn) == 8) ? 1 : -1];

typedef struct ToolBpfStat
{
    ULONG bs_recv;
    ULONG bs_drop;
} ToolBpfStat;

/* The capture record, at the offsets include/aminetxduo/bpf.h pins. */
#define TB_OFF_SEC          0
#define TB_OFF_USEC         4
#define TB_OFF_CAPLEN       8
#define TB_OFF_DATALEN      12
#define TB_OFF_HDRLEN       16
#define TB_HDR_MIN          18      /* the fields; the record is padded to 20 */
#define TB_WORDALIGN(x)     ((((ULONG)(x)) + 3UL) & ~3UL)

/* ------------------------------------------------------------ the vectors */

/* LVO -0x16e */
static LONG tool_bpf_open(struct Library *base, LONG channel)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = channel;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-366:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* LVO -0x174 */
static LONG tool_bpf_close(struct Library *base, LONG channel)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = channel;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-372:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* LVO -0x17a */
static LONG tool_bpf_read_lvo(struct Library *base, LONG channel, APTR buf,
                              LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = channel;
    register APTR            a0  __asm("a0") = buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-378:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

/* LVO -0x192 */
static LONG tool_bpf_ioctl(struct Library *base, LONG channel, ULONG cmd,
                           APTR buf)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = channel;
    register ULONG           d1  __asm("d1") = cmd;
    register APTR            a0  __asm("a0") = buf;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-402:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

/* --------------------------------------------------------- record reading */

static ULONG tool_bpf_get32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static UWORD tool_bpf_get16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | (UWORD)p[1]);
}

/* --------------------------------------------------------------- the sink */

/*
 * Where the pcap bytes go.  Not buffered again here: ToolPcap has already
 * gathered 16 KB, and dos.library's own buffering is what the Flush() in every
 * tool_printf() exists to defeat.
 */
static int tool_bpf_sink(void *cookie, const unsigned char *data,
                         unsigned long len)
{
    ToolBpfChan *c = (ToolBpfChan *)cookie;

    if (c->fh == 0)
        return -1;

    if (Write(c->fh, (APTR)data, (LONG)len) != (LONG)len)
        return -1;

    return 0;
}

/* ------------------------------------------------------------------ start */

BOOL tool_bpf_start(ToolBpfChan *c, struct Library *base, const char *iface,
                    const char *out, ULONG snaplen, ULONG blen,
                    const ToolBpfInsn *prog, ULONG proglen)
{
    ToolBpfInsn     accept_all[1];
    ToolBpfProgram  bp;
    ULONG           value;
    char            ifr[32];
    UWORD           i;

    c->base        = base;
    c->channel     = 0;
    c->open        = FALSE;
    c->buf         = NULL;
    c->buflen      = 0;
    c->fh          = 0;
    c->max_records = 0;
    c->max_filelen = 0;
    c->limit_hit   = FALSE;
    c->expect_drained = TRUE;
    c->short_reads = 0;
    c->recv        = 0;
    c->drop        = 0;
    c->left        = 0;
    c->out.sink    = 0;
    c->out.open    = 0;

    /* -1 asks for any free channel and the answer names the one claimed, so
       two captures can run at once. */
    c->channel = tool_bpf_open(base, -1);
    if (c->channel < 0)
    {
        c->channel = 0;
        tool_error("bpf_open failed: either this bsdsocket.library is not "
                   "ours, or it was built without BPF");
        return FALSE;
    }
    c->open = TRUE;

    /* Buffer size before the interface: real BPF refuses BIOCSBLEN once the
       buffers are allocated, and so does ours. */
    value = blen;
    if (tool_bpf_ioctl(base, c->channel, BIOCSBLEN_, &value) != 0)
    {
        tool_error("BIOCSBLEN failed");
        return FALSE;
    }
    c->buflen = value;

    for (i = 0; i < (UWORD)sizeof(ifr); i++)
        ifr[i] = '\0';
    for (i = 0; i + 1 < (UWORD)sizeof(ifr) && iface[i] != '\0'; i++)
        ifr[i] = iface[i];

    if (tool_bpf_ioctl(base, c->channel, BIOCSETIF_, ifr) != 0)
    {
        tool_error("no interface '%s' to capture on", (LONG)iface);
        return FALSE;
    }

    if (prog == NULL || proglen == 0)
    {
        /* One instruction: accept every packet, keep `snaplen` bytes of it. */
        accept_all[0].code = (unsigned short)TOOL_BPF_RET_K;
        accept_all[0].jt   = 0;
        accept_all[0].jf   = 0;
        accept_all[0].k    = (long)snaplen;
        prog    = accept_all;
        proglen = 1;
    }

    bp.bf_len   = proglen;
    bp.bf_insns = prog;

    if (tool_bpf_ioctl(base, c->channel, BIOCSETF_, &bp) != 0)
    {
        tool_error("BIOCSETF failed");
        return FALSE;
    }

    value = 1;
    (VOID)tool_bpf_ioctl(base, c->channel, BIOCIMMEDIATE_, &value);

    value = 0;
    if (tool_bpf_ioctl(base, c->channel, BIOCGDLT_, &value) != 0 ||
        value != TOOL_PCAP_DLT_EN10MB)
    {
        tool_error("interface '%s' is not DLT_EN10MB (%lu)", (LONG)iface,
                   (LONG)value);
        return FALSE;
    }

    c->buf = (UBYTE *)AllocVec(c->buflen, MEMF_ANY);
    if (c->buf == NULL)
    {
        tool_error("no memory for a %lu byte capture buffer", (LONG)c->buflen);
        return FALSE;
    }

    c->fh = Open((CONST_STRPTR)out, MODE_NEWFILE);
    if (c->fh == 0)
    {
        tool_error("cannot write %s", (LONG)out);
        return FALSE;
    }

    tool_pcap_begin(&c->out, tool_bpf_sink, c, snaplen);

    (VOID)tool_bpf_ioctl(base, c->channel, BIOCFLUSH_, NULL);

    return TRUE;
}

VOID tool_bpf_read_timeout(ToolBpfChan *c, ULONG micros)
{
    ULONG tv[2];

    if (!c->open)
        return;

    tv[0] = micros / 1000000UL;
    tv[1] = micros % 1000000UL;

    (VOID)tool_bpf_ioctl(c->base, c->channel, BIOCSRTIMEOUT_, tv);
}

/* ------------------------------------------------------------------ drain */

/*
 * At most this many bpf_read() calls in one drain.  The tap keeps filling
 * while the reader empties, so "read until it comes back empty" is a loop that
 * a busy segment can keep alive indefinitely -- and the caller's Ctrl-C test
 * is outside it.  Eight reads is eight bufferfuls, which is four times
 * everything the channel can be holding.
 */
#define TB_READS_PER_DRAIN  8

ULONG tool_bpf_drain(ToolBpfChan *c)
{
    ULONG took  = 0;
    UWORD reads = 0;

    if (!c->open || c->buf == NULL || c->limit_hit)
        return 0;

    while (reads++ < (UWORD)TB_READS_PER_DRAIN)
    {
        LONG  got;
        ULONG pos = 0;

        got = tool_bpf_read_lvo(c->base, c->channel, c->buf, (LONG)c->buflen);
        if (got <= 0)
        {
            /*
             * -1 means no record fits in the buffer, which cannot happen with
             * a buffer sized from BIOCSBLEN.  Counted rather than ignored: if
             * it ever does, the trace has a hole.
             */
            if (got < 0)
                c->short_reads++;
            break;
        }

        while (pos + 20UL <= (ULONG)got)
        {
            ULONG sec    = tool_bpf_get32(c->buf + pos + TB_OFF_SEC);
            ULONG usec   = tool_bpf_get32(c->buf + pos + TB_OFF_USEC);
            ULONG caplen = tool_bpf_get32(c->buf + pos + TB_OFF_CAPLEN);
            ULONG datlen = tool_bpf_get32(c->buf + pos + TB_OFF_DATALEN);
            ULONG hdrlen = (ULONG)tool_bpf_get16(c->buf + pos + TB_OFF_HDRLEN);

            if (hdrlen < (ULONG)TB_HDR_MIN ||
                pos + hdrlen + caplen > (ULONG)got)
                break;

            /*
             * The limit is tested before the record and not after it, so the
             * file never carries one more frame than the user asked for and
             * never grows past the size they set.
             */
            if (c->max_records != 0 && c->out.records >= c->max_records)
            {
                c->limit_hit = TRUE;
                return took;
            }

            if (c->max_filelen != 0 &&
                c->out.filelen + (unsigned long)TOOL_PCAP_REC_HDR +
                    (unsigned long)caplen > (unsigned long)c->max_filelen)
            {
                c->limit_hit = TRUE;
                return took;
            }

            tool_pcap_record(&c->out, sec, usec, caplen, datlen,
                             c->buf + pos + hdrlen);
            took++;

            pos += TB_WORDALIGN(hdrlen + caplen);
        }

        if ((ULONG)got < c->buflen / 2UL)
            break;
    }

    return took;
}

VOID tool_bpf_stats(ToolBpfChan *c)
{
    ToolBpfStat st;

    if (!c->open)
        return;

    st.bs_recv = 0;
    st.bs_drop = 0;

    if (tool_bpf_ioctl(c->base, c->channel, BIOCGSTATS_, &st) == 0)
    {
        c->recv = st.bs_recv;
        c->drop = st.bs_drop;
    }
}

/* ------------------------------------------------------------------- stop */

VOID tool_bpf_stop(ToolBpfChan *c)
{
    UWORD pass;

    if (!c->open)
        return;

    /*
     * No waiting from here on: the drains below are emptying what is already
     * buffered, and a read that waits its full BIOCSRTIMEOUT for a record that
     * is not coming adds that wait to every Ctrl-C.
     */
    tool_bpf_read_timeout(c, 0);

    /* Until it comes back empty, which TB_READS_PER_DRAIN stops one drain from
       doing on its own.  Bounded, for the reason the drain is. */
    for (pass = 0; pass < 64; pass++)
    {
        if (tool_bpf_drain(c) == 0)
            break;
    }

    tool_bpf_stats(c);

    /*
     * Nothing must still be buffered after the drains above.  Anything left
     * means the trace stops short of the last few frames -- except when the
     * capture stopped on purpose, which is not a defect and is not reported
     * as one.  See expect_drained.  FIONREAD rather than bpf_data_waiting(),
     * which the autodoc defines as a 0/1 flag.
     */
    c->left = 0;
    if (c->expect_drained && !c->limit_hit)
        (VOID)tool_bpf_ioctl(c->base, c->channel, FIONREAD_, &c->left);

    tool_pcap_end(&c->out);

    if (c->fh != 0)
    {
        Close(c->fh);
        c->fh = 0;
    }

    if (c->buf != NULL)
    {
        FreeVec(c->buf);
        c->buf = NULL;
    }

    (VOID)tool_bpf_close(c->base, c->channel);
    c->open = FALSE;
}

BOOL tool_bpf_warn(const ToolBpfChan *c)
{
    BOOL said = FALSE;

    if (c->out.failed)
    {
        tool_error("the trace file was truncated, perhaps because the disk "
                   "is full");
        said = TRUE;
    }

    if (c->drop != 0)
    {
        tool_error("%lu frames were seen and not written: the trace has holes",
                   (LONG)c->drop);
        said = TRUE;
    }

    if (c->left != 0UL)
    {
        tool_error("%lu bytes were still buffered at the end of the trace",
                   (LONG)c->left);
        said = TRUE;
    }

    return said;
}
