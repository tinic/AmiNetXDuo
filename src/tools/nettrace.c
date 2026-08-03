/*
 * NetTrace -- capture the stack's own traffic to a pcap file while running a
 * workload underneath it.
 *
 *     NetTrace [WIRE|LOOPBACK] [HOST=..] [PORT=n] [PATH=..] [BYTES=n]
 *              [OUT=file] [SNAP=n] [NOCAPTURE/S] [BLEN=n]
 *
 *   Capture and workload share one process so the throughput number and the
 *   trace come out of the same run.  Draining the capture between socket
 *   operations also bounds the buffer: the channel holds 2 x BLEN, and a
 *   megabyte at 1460 bytes a segment is well over a thousand records.
 *
 *   Nothing from src/.  Every call below is a published bsdsocket.library LVO,
 *   including the bpf_* ones.  Before this existed the capture path in
 *   src/bpf/ had 201 unit-test checks, no caller anywhere in the product, and
 *   all eight vectors pointing at bsd_enosys().
 *
 *   The bpf ABI has no BIOCSSNAPLEN: a filter program returns the number of
 *   bytes to keep, so `BPF_RET|BPF_K, n` accepts every packet and truncates
 *   it to n.  96 bytes covers Ethernet + IP + TCP with 20 bytes of options,
 *   and was tcpdump's own default for twenty years.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

#include <stdarg.h>
#include "aminetxduo/version.h"

const char *const tool_name = "NetTrace";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetTrace");

#define TEMPLATE \
    "LOOPBACK/S,WIRE/S,HOST/K,PORT/N/K,PATH/K,BYTES/N/K,OUT/K,SNAP/N/K," \
    "BLEN/N/K,NOCAPTURE/S,IFACE/K"

enum
{
    ARG_LOOPBACK = 0,
    ARG_WIRE,
    ARG_HOST,
    ARG_PORT,
    ARG_PATH,
    ARG_BYTES,
    ARG_OUT,
    ARG_SNAP,
    ARG_BLEN,
    ARG_NOCAPTURE,
    ARG_IFACE,
    ARG_COUNT
};

/* --------------------------------------------------------------- the ABI */

/*
 * The bpf half of <net/bpf.h>, open-coded like toolsock.h's sockaddr_in, so
 * this command builds against a stock NDK.
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
#define BIOCGSTATS_     IOC_(IOC_OUT_,   'B', 111, 8)
#define BIOCIMMEDIATE_  IOC_(IOC_IN_,    'B', 112, 4)

#define BPF_RET_K       0x06            /* BPF_RET | BPF_K */

typedef struct NtInsn
{
    UWORD code;
    UBYTE jt;
    UBYTE jf;
    LONG  k;
} NtInsn;

typedef struct NtProgram
{
    ULONG   bf_len;
    NtInsn *bf_insns;
} NtProgram;

typedef struct NtStat
{
    ULONG bs_recv;
    ULONG bs_drop;
} NtStat;

/* The capture record, at the offsets include/aminetxduo/bpf.h pins. */
#define NT_OFF_SEC      0
#define NT_OFF_USEC     4
#define NT_OFF_CAPLEN   8
#define NT_OFF_DATALEN  12
#define NT_OFF_HDRLEN   16
#define NT_WORDALIGN(x) ((((ULONG)(x)) + 3UL) & ~3UL)

/* ------------------------------------------------------------ the vectors */

/* LVO -0x16e */
static LONG nt_bpf_open(struct Library *base, LONG channel)
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
static LONG nt_bpf_close(struct Library *base, LONG channel)
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
static LONG nt_bpf_read(struct Library *base, LONG channel, APTR buf, LONG len)
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
static LONG nt_bpf_ioctl(struct Library *base, LONG channel, ULONG cmd,
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

/*
 * Every line is flushed as it is written: VPrintf() buffers through
 * dos.library, so the last lines are lost if the machine has to be killed.
 * It is also the only progress indicator -- a megabyte at 14 MHz takes
 * seconds, and a stalled run looks like a working one without it.
 */
static VOID nt_say(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);
    va_end(args);

    Flush(Output());
}

/* ------------------------------------------------------------ pcap output */

/*
 * Classic libpcap, not pcapng: it opens in Wireshark and tcpdump with no
 * conversion step.
 *
 * Every field goes out big-endian, byte by byte, including the magic.  A pcap
 * file carries its endianness in that first longword and every tool in the
 * family has honoured it since 1993, so a 68k-written file opens on a
 * little-endian host as it stands.  Writing the bytes by hand rather than
 * storing a ULONG also avoids any alignment assumption about the buffer.
 */
#define PCAP_MAGIC          0xa1b2c3d4UL
#define PCAP_VERSION_MAJOR  2
#define PCAP_VERSION_MINOR  4
#define PCAP_DLT_EN10MB     1

#define NT_OUTBUF           16384UL

typedef struct NtOut
{
    BPTR  fh;
    UBYTE buf[NT_OUTBUF];
    ULONG used;
    ULONG records;
    ULONG bytes;
    BOOL  failed;
} NtOut;

static VOID nt_out_flush(NtOut *o)
{
    if (o->used == 0 || o->fh == 0)
        return;

    if (Write(o->fh, o->buf, (LONG)o->used) != (LONG)o->used)
        o->failed = TRUE;

    o->used = 0;
}

static VOID nt_out_raw(NtOut *o, const UBYTE *data, ULONG len)
{
    if (o->fh == 0)
        return;

    if (o->used + len > NT_OUTBUF)
        nt_out_flush(o);

    if (len > NT_OUTBUF)
    {
        if (Write(o->fh, (APTR)data, (LONG)len) != (LONG)len)
            o->failed = TRUE;
        return;
    }

    {
        ULONG i;

        for (i = 0; i < len; i++)
            o->buf[o->used + i] = data[i];
    }
    o->used += len;
}

static VOID nt_out_u32(NtOut *o, ULONG v)
{
    UBYTE b[4];

    b[0] = (UBYTE)(v >> 24);
    b[1] = (UBYTE)(v >> 16);
    b[2] = (UBYTE)(v >> 8);
    b[3] = (UBYTE)(v);
    nt_out_raw(o, b, 4);
}

static VOID nt_out_u16(NtOut *o, UWORD v)
{
    UBYTE b[2];

    b[0] = (UBYTE)(v >> 8);
    b[1] = (UBYTE)(v);
    nt_out_raw(o, b, 2);
}

static BOOL nt_out_open(NtOut *o, const char *name, ULONG snaplen)
{
    o->used    = 0;
    o->records = 0;
    o->bytes   = 0;
    o->failed  = FALSE;

    o->fh = Open((CONST_STRPTR)name, MODE_NEWFILE);
    if (o->fh == 0)
        return FALSE;

    nt_out_u32(o, PCAP_MAGIC);
    nt_out_u16(o, PCAP_VERSION_MAJOR);
    nt_out_u16(o, PCAP_VERSION_MINOR);
    nt_out_u32(o, 0);                   /* thiszone: UTC                   */
    nt_out_u32(o, 0);                   /* sigfigs                         */
    nt_out_u32(o, snaplen);
    nt_out_u32(o, PCAP_DLT_EN10MB);

    return TRUE;
}

static VOID nt_out_close(NtOut *o)
{
    if (o->fh == 0)
        return;

    nt_out_flush(o);
    Close(o->fh);
    o->fh = 0;
}

/* --------------------------------------------------------- record reading */

static ULONG nt_get32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static UWORD nt_get16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | (UWORD)p[1]);
}

/*
 * Static, and it has to be: NtCap embeds NtOut's 16 KB write buffer, while an
 * AmigaDOS Shell command runs on a 4 KB stack (a SystemTagList() child gets
 * whatever the parent had, and nothing here calls SetStackSize()).  As a local
 * in main() this overran the stack and the machine took an F-line trap
 * (#8000000B) and reset, with the crash landing in unrelated code.  One per
 * process, single-threaded, so static costs nothing.
 */
typedef struct NtCap
{
    struct Library *base;
    LONG            channel;
    BOOL            open;
    UBYTE          *buf;
    ULONG           buflen;
    NtOut           out;
    ULONG           short_reads;
} NtCap;

/*
 * Copy everything buffered in the channel into the pcap file.  Returns the
 * number of records taken.
 *
 * bpf_read() is non-blocking (include/aminetxduo/bpf.h), so this is called
 * from inside the workload's own loop rather than from a reader task; there is
 * nothing to synchronise.
 */
static ULONG nt_drain(NtCap *cap)
{
    ULONG took = 0;

    if (!cap->open)
        return 0;

    for (;;)
    {
        LONG  got;
        ULONG pos = 0;

        got = nt_bpf_read(cap->base, cap->channel, cap->buf, (LONG)cap->buflen);
        if (got <= 0)
        {
            /*
             * -1 means no record fits in the buffer, which cannot happen with
             * a buffer sized from BIOCGBLEN.  Counted rather than ignored: if
             * it ever does, the trace has a hole.
             */
            if (got < 0)
                cap->short_reads++;
            break;
        }

        while (pos + 20UL <= (ULONG)got)
        {
            ULONG sec    = nt_get32(cap->buf + pos + NT_OFF_SEC);
            ULONG usec   = nt_get32(cap->buf + pos + NT_OFF_USEC);
            ULONG caplen = nt_get32(cap->buf + pos + NT_OFF_CAPLEN);
            ULONG datlen = nt_get32(cap->buf + pos + NT_OFF_DATALEN);
            ULONG hdrlen = (ULONG)nt_get16(cap->buf + pos + NT_OFF_HDRLEN);

            if (hdrlen < 18UL || pos + hdrlen + caplen > (ULONG)got)
                break;

            nt_out_u32(&cap->out, sec);
            nt_out_u32(&cap->out, usec);
            nt_out_u32(&cap->out, caplen);
            nt_out_u32(&cap->out, datlen);
            nt_out_raw(&cap->out, cap->buf + pos + hdrlen, caplen);

            cap->out.records++;
            cap->out.bytes += caplen;
            took++;

            pos += NT_WORDALIGN(hdrlen + caplen);
        }

        if ((ULONG)got < cap->buflen / 2UL)
            break;
    }

    return took;
}

static BOOL nt_cap_start(NtCap *cap, struct Library *base, const char *iface,
                         const char *out, ULONG snaplen, ULONG blen)
{
    NtInsn    prog[1];
    NtProgram bp;
    ULONG     value;
    char      ifr[32];
    UWORD     i;

    cap->base    = base;
    cap->channel = 0;
    cap->open    = FALSE;
    cap->buf     = NULL;
    cap->out.fh  = 0;
    cap->short_reads = 0;

    /* -1 asks for any free channel and the answer names the one claimed, so
       two captures can run at once. */
    cap->channel = nt_bpf_open(base, -1);
    if (cap->channel < 0)
    {
        cap->channel = 0;
        tool_error("bpf_open failed -- is this bsdsocket.library ours, and "
                   "was it built with BPF on?");
        return FALSE;
    }
    cap->open = TRUE;

    /* Buffer size before the interface: real BPF refuses BIOCSBLEN once the
       buffers are allocated, and so does ours. */
    value = blen;
    if (nt_bpf_ioctl(base, cap->channel, BIOCSBLEN_, &value) != 0)
    {
        tool_error("BIOCSBLEN failed");
        return FALSE;
    }
    cap->buflen = value;

    for (i = 0; i < (UWORD)sizeof(ifr); i++)
        ifr[i] = '\0';
    for (i = 0; i + 1 < (UWORD)sizeof(ifr) && iface[i] != '\0'; i++)
        ifr[i] = iface[i];

    if (nt_bpf_ioctl(base, cap->channel, BIOCSETIF_, ifr) != 0)
    {
        tool_error("no interface '%s' to capture on", (LONG)iface);
        return FALSE;
    }

    /* One instruction: accept every packet, keep `snaplen` bytes of it. */
    prog[0].code = (UWORD)BPF_RET_K;
    prog[0].jt   = 0;
    prog[0].jf   = 0;
    prog[0].k    = (LONG)snaplen;
    bp.bf_len    = 1;
    bp.bf_insns  = prog;

    if (nt_bpf_ioctl(base, cap->channel, BIOCSETF_, &bp) != 0)
    {
        tool_error("BIOCSETF failed");
        return FALSE;
    }

    value = 1;
    (VOID)nt_bpf_ioctl(base, cap->channel, BIOCIMMEDIATE_, &value);

    value = 0;
    if (nt_bpf_ioctl(base, cap->channel, BIOCGDLT_, &value) != 0 ||
        value != PCAP_DLT_EN10MB)
    {
        tool_error("interface '%s' is not DLT_EN10MB (%lu)", (LONG)iface,
                   (LONG)value);
        return FALSE;
    }

    cap->buf = (UBYTE *)AllocVec(cap->buflen, MEMF_ANY);
    if (cap->buf == NULL)
    {
        tool_error("no memory for a %lu byte capture buffer", (LONG)cap->buflen);
        return FALSE;
    }

    if (!nt_out_open(&cap->out, out, snaplen))
    {
        tool_error("cannot write %s", (LONG)out);
        return FALSE;
    }

    (VOID)nt_bpf_ioctl(base, cap->channel, BIOCFLUSH_, NULL);

    nt_say("capture: channel 0 on %s, %lu byte buffers, snaplen %lu\n",
           (LONG)iface, (LONG)cap->buflen, (LONG)snaplen);

    return TRUE;
}

static VOID nt_cap_stop(NtCap *cap)
{
    NtStat st;
    ULONG  left = 0UL;

    if (!cap->open)
        return;

    (VOID)nt_drain(cap);

    st.bs_recv = 0;
    st.bs_drop = 0;
    (VOID)nt_bpf_ioctl(cap->base, cap->channel, BIOCGSTATS_, &st);

    /* Nothing should still be buffered after the drain above; if it is, the
       trace stops short of the last few frames. FIONREAD rather than
       bpf_data_waiting(), which the autodoc defines as a 0/1 flag. */
    (VOID)nt_bpf_ioctl(cap->base, cap->channel, FIONREAD_, &left);

    nt_out_close(&cap->out);

    nt_say("capture: %lu records written, %lu bytes, "
                "%lu seen, %lu dropped by the channel, %lu short reads\n",
                (LONG)cap->out.records, (LONG)cap->out.bytes,
                (LONG)st.bs_recv, (LONG)st.bs_drop, (LONG)cap->short_reads);

    if (cap->out.failed)
        tool_error("the trace file was truncated -- disk full?");

    if (st.bs_drop != 0)
        tool_error("%lu frames were seen and NOT written: the trace has holes",
                   (LONG)st.bs_drop);

    if (left != 0UL)
        tool_error("%lu bytes were still buffered at the end of the trace",
                   (LONG)left);

    if (cap->buf != NULL)
    {
        FreeVec(cap->buf);
        cap->buf = NULL;
    }

    (VOID)nt_bpf_close(cap->base, cap->channel);
    cap->open = FALSE;
}

/* ---------------------------------------------------------- the workloads */

#define NT_CHUNK    4096

static UBYTE nt_buf[NT_CHUNK];

/* See the note on NtCap: 16 KB of write buffer cannot live on a 4 KB stack. */
static NtCap nt_cap;

typedef struct NtResult
{
    ULONG bytes;
    ULONG ticks;
    BOOL  ok;
} NtResult;

/*
 * A bulk transfer between two sockets in this one process, over 127.0.0.1.
 *
 * Single-threaded and therefore entirely non-blocking: listener, client and
 * accepted socket all go through one WaitSelect().  The capture is drained on
 * every pass, so the channel never holds more than a few hundred microseconds
 * of traffic.
 */
static VOID nt_loopback(struct Library *base, NtCap *cap, ULONG want,
                        NtResult *res)
{
    ToolSockAddrAny sa;
    ToolFdSet    rd;
    ToolFdSet    wr;
    ToolTimeval  tv;
    LONG         lst = -1;
    LONG         cli = -1;
    LONG         srv = -1;
    LONG         one = 1;
    ULONG        sent = 0;
    ULONG        recvd = 0;
    ULONG        start;
    ULONG        i;
    BOOL         connected = FALSE;

    res->bytes = 0;
    res->ticks = 0;
    res->ok    = FALSE;

    for (i = 0; i < NT_CHUNK; i++)
        nt_buf[i] = (UBYTE)(i & 0xFF);

    lst = tool_sock_socket(base, TOOL_AF_INET, TOOL_SOCK_STREAM, 0);
    cli = tool_sock_socket(base, TOOL_AF_INET, TOOL_SOCK_STREAM, 0);
    if (lst < 0 || cli < 0)
    {
        tool_error("socket() failed: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(base)));
        goto done;
    }

    (VOID)tool_sock_setsockopt(base, lst, TOOL_SOL_SOCKET, TOOL_SO_REUSEADDR,
                               &one, (LONG)sizeof(one));

    (VOID)tool_sock_addr_v4(&sa, 0x7F000001UL, 0);
    if (tool_sock_bind(base, lst, &sa) < 0 ||
        tool_sock_listen(base, lst, 1) < 0 ||
        tool_sock_getsockname(base, lst, &sa) < 0)
    {
        tool_error("cannot set up the loopback listener: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(base)));
        goto done;
    }

    (VOID)tool_sock_ioctl(base, lst, TOOL_FIONBIO, &one);
    (VOID)tool_sock_ioctl(base, cli, TOOL_FIONBIO, &one);

    if (tool_sock_connect(base, cli, &sa) < 0)
    {
        LONG err = tool_sock_errno(base);

        if (err != TOOL_EINPROGRESS && err != TOOL_EWOULDBLOCK)
        {
            tool_error("loopback connect failed: %s",
                       (LONG)tool_sock_errstr(err));
            goto done;
        }
    }

    start = ami_millis();

    while (recvd < want)
    {
        LONG n;
        LONG nfds;

        if (tool_break())
            break;

        tool_fd_zero(&rd);
        tool_fd_zero(&wr);

        if (srv < 0)
            tool_fd_add(&rd, lst);
        else
            tool_fd_add(&rd, srv);

        if (sent < want)
            tool_fd_add(&wr, cli);

        nfds = lst;
        if (cli > nfds) nfds = cli;
        if (srv > nfds) nfds = srv;

        tv.tv_secs  = 5;
        tv.tv_micro = 0;

        n = tool_sock_select(base, nfds + 1, &rd, &wr, &tv);
        if (n < 0)
        {
            tool_error("WaitSelect failed: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(base)));
            break;
        }
        if (n == 0)
        {
            tool_error("loopback transfer stalled with %lu of %lu bytes",
                       (LONG)recvd, (LONG)want);
            break;
        }

        if (srv < 0 && tool_fd_isset(&rd, lst))
        {
            srv = tool_sock_accept(base, lst, NULL);
            if (srv >= 0)
                (VOID)tool_sock_ioctl(base, srv, TOOL_FIONBIO, &one);
        }

        if (!connected && tool_fd_isset(&wr, cli))
            connected = TRUE;

        if (connected && sent < want && tool_fd_isset(&wr, cli))
        {
            ULONG left = want - sent;
            LONG  ask  = (left > NT_CHUNK) ? (LONG)NT_CHUNK : (LONG)left;

            n = tool_sock_send(base, cli, nt_buf, ask);
            if (n > 0)
                sent += (ULONG)n;
            else if (n < 0 && tool_sock_errno(base) != TOOL_EWOULDBLOCK)
            {
                tool_error("send failed: %s",
                           (LONG)tool_sock_errstr(tool_sock_errno(base)));
                break;
            }
        }

        if (srv >= 0 && tool_fd_isset(&rd, srv))
        {
            n = tool_sock_recv(base, srv, nt_buf, (LONG)NT_CHUNK);
            if (n > 0)
                recvd += (ULONG)n;
            else if (n == 0)
                break;
            else if (tool_sock_errno(base) != TOOL_EWOULDBLOCK)
                break;
        }

        (VOID)nt_drain(cap);
    }

    res->ticks = ami_millis() - start;
    res->bytes = recvd;
    res->ok    = (recvd >= want);

done:
    if (srv >= 0) (VOID)tool_sock_close(base, srv);
    if (cli >= 0) (VOID)tool_sock_close(base, cli);
    if (lst >= 0) (VOID)tool_sock_close(base, lst);

    (VOID)nt_drain(cap);
}

/*
 * One HTTP/1.0 GET over the wire, read to completion.
 *
 * HTTP/1.0 with no keep-alive, so the body ends when the peer closes: the
 * trace covers the shutdown as well, and there is no chunk parser here.  The
 * bytes are counted, not kept; tests/curl checks payloads byte for byte.
 */
static VOID nt_wire(struct Library *base, NtCap *cap, const ToolAddr *address,
                    UWORD port, const char *path, NtResult *res)
{
    ToolSockAddrAny sa;
    ToolFdSet    rd;
    ToolTimeval  tv;
    LONG         s;
    LONG         n;
    ULONG        start;
    ULONG        body = 0;
    ULONG        total = 0;
    BOOL         in_body = FALSE;
    ULONG        match = 0;
    char         req[512];
    ULONG        len = 0;
    const char  *p;

    res->bytes = 0;
    res->ticks = 0;
    res->ok    = FALSE;

    s = tool_sock_socket(base, (LONG)address->ta_Family, TOOL_SOCK_STREAM,
                         0);
    if (s < 0)
    {
        tool_error("socket() failed: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(base)));
        return;
    }

    (VOID)tool_sock_addr(&sa, address, port);

    start = ami_millis();

    if (tool_sock_connect(base, s, &sa) < 0)
    {
        tool_sock_fail(base, "connect", address, port);
        (VOID)tool_sock_close(base, s);
        return;
    }

    /*
     * PATH comes off the command line and `req` is on the caller's stack,
     * which a Shell command gets four kilobytes of. Copying it unbounded let
     * any PATH longer than 462 characters overwrite this frame's return
     * address, silently -- there is no MMU here. The trailer is appended only
     * if it still fits, so a truncated request is refused by the server rather
     * than sent as something else.
     */
    {
        static const char nt_head[]  = "GET ";
        static const char nt_tail[]  =
            " HTTP/1.0\r\nHost: amiga\r\nConnection: close\r\n\r\n";
        const ULONG       room = (ULONG)sizeof(req) - (ULONG)sizeof(nt_tail);

        for (p = nt_head; *p != '\0'; p++)
            req[len++] = *p;

        for (p = path; *p != '\0' && len < room; p++)
            req[len++] = *p;

        if (*p != '\0')
        {
            tool_error("the path is too long for a request buffer of %ld bytes",
                       (LONG)sizeof(req));
            (VOID)tool_sock_close(base, s);
            return;
        }

        for (p = nt_tail; *p != '\0'; p++)
            req[len++] = *p;
    }

    if (tool_sock_send(base, s, req, (LONG)len) != (LONG)len)
    {
        tool_error("cannot send the request: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(base)));
        (VOID)tool_sock_close(base, s);
        return;
    }

    for (;;)
    {
        LONG i;

        if (tool_break())
            break;

        tool_fd_zero(&rd);
        tool_fd_add(&rd, s);
        tv.tv_secs  = 20;
        tv.tv_micro = 0;

        n = tool_sock_select(base, s + 1, &rd, NULL, &tv);
        if (n <= 0)
        {
            if (n == 0)
                tool_error("the peer went quiet after %lu bytes", (LONG)total);
            break;
        }

        n = tool_sock_recv(base, s, nt_buf, (LONG)NT_CHUNK);
        if (n <= 0)
        {
            if (n == 0)
                res->ok = TRUE;         /* clean close: the body is complete */
            break;
        }

        total += (ULONG)n;

        /* Find the blank line once, then count body bytes. */
        if (!in_body)
        {
            for (i = 0; i < n; i++)
            {
                UBYTE c = nt_buf[i];

                if ((match == 0 && c == '\r') || (match == 2 && c == '\r'))
                    match++;
                else if ((match == 1 && c == '\n') || (match == 3 && c == '\n'))
                    match++;
                else
                    match = (c == '\r') ? 1 : 0;

                if (match == 4)
                {
                    in_body = TRUE;
                    body += (ULONG)(n - i - 1);
                    break;
                }
            }
        }
        else
        {
            body += (ULONG)n;
        }

        (VOID)nt_drain(cap);
    }

    res->ticks = ami_millis() - start;
    res->bytes = body;

    (VOID)tool_sock_close(base, s);
    (VOID)nt_drain(cap);
}

/* ------------------------------------------------------------------- main */

static VOID nt_report(const char *what, const NtResult *r)
{
    ULONG rate;

    if (r->ticks == 0)
    {
        nt_say("%s: %lu bytes in under a millisecond\n", (LONG)what,
               (LONG)r->bytes);
        return;
    }

    /* Bytes per second without a 64-bit divide: bytes/ms * 1000 loses too much
       on a short run, so scale by 1000 in the safe order. */
    rate = (r->bytes / r->ticks) * 1000UL +
           ((r->bytes % r->ticks) * 1000UL) / r->ticks;

    nt_say("%s: %lu bytes in %lu ms = %lu bytes/s (%lu KB/s)%s\n",
                (LONG)what, (LONG)r->bytes, (LONG)r->ticks, (LONG)rate,
                (LONG)(rate / 1024UL), (LONG)(r->ok ? "" : "  INCOMPLETE"));
}

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *base;
    NtResult        result;
    const char     *out;
    const char     *iface;
    const char     *path;
    ULONG           snaplen;
    ULONG           blen;
    ULONG           bytes;
    ToolAddr        address;
    UWORD           port    = 80;
    BOOL            wire;
    BOOL            capture;
    int             rc = RETURN_OK;
    UWORD           i;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < (UWORD)ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    wire    = (args[ARG_WIRE] != 0) || (args[ARG_HOST] != 0);
    capture = (args[ARG_NOCAPTURE] == 0);

    path  = (args[ARG_PATH] != 0) ? (const char *)args[ARG_PATH] : "/";
    out   = (args[ARG_OUT] != 0) ? (const char *)args[ARG_OUT]
                                 : (wire ? "DH0:wire.pcap" : "DH0:loop.pcap");
    iface = (args[ARG_IFACE] != 0) ? (const char *)args[ARG_IFACE]
                                   : (wire ? "eth0" : "lo0");

    snaplen = (args[ARG_SNAP] != 0) ? (ULONG)(*(LONG *)args[ARG_SNAP]) : 96UL;
    blen    = (args[ARG_BLEN] != 0) ? (ULONG)(*(LONG *)args[ARG_BLEN]) : 32768UL;
    bytes   = (args[ARG_BYTES] != 0) ? (ULONG)(*(LONG *)args[ARG_BYTES])
                                     : 1048576UL;

    if (args[ARG_PORT] != 0)
        port = (UWORD)(*(LONG *)args[ARG_PORT]);

    nt_say("NetTrace: opening bsdsocket.library\n");

    base = tool_socket_open();
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (wire)
    {
        const char *host = (args[ARG_HOST] != 0)
                               ? (const char *)args[ARG_HOST] : "10.0.2.2";

        if (!tool_sock_resolve(base, host, &address))
        {
            CloseLibrary(base);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    nt_cap.open = FALSE;
    nt_cap.out.fh = 0;

    if (capture && !nt_cap_start(&nt_cap, base, iface, out, snaplen, blen))
    {
        nt_cap_stop(&nt_cap);
        CloseLibrary(base);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (wire)
    {
        char text[TOOL_ADDR_STRLEN];

        tool_addr_text(base, &address, text, sizeof(text));
        nt_say("NetTrace: GET %s from %s port %lu, capturing %s\n",
                    (LONG)path, (LONG)text, (LONG)port,
                    (LONG)(capture ? iface : "nothing"));
        nt_wire(base, &nt_cap, &address, port, path, &result);
        nt_report("wire", &result);
    }
    else
    {
        nt_say("NetTrace: %lu bytes over 127.0.0.1, capturing %s\n",
               (LONG)bytes, (LONG)(capture ? iface : "nothing"));
        nt_loopback(base, &nt_cap, bytes, &result);
        nt_report("loopback", &result);
    }

    if (!result.ok)
        rc = RETURN_WARN;

    if (capture)
        nt_cap_stop(&nt_cap);

    CloseLibrary(base);
    FreeArgs(rda);

    return rc;
}
