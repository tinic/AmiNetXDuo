/*
 * NetCapture, capture what is on the wire to a pcap file that Wireshark and
 * tcpdump read.
 *
 *     NetCapture OUT/K,IFACE/K,SNAP/N/K,BLEN/N/K,COUNT/N/K,SECONDS/N/K,
 *                SIZE/N/K,HOST/K,PORT/N/K,PROTO/K,NOT/S,QUIET/S
 *
 *   NetTrace already writes a pcap, and cannot do this.  Its arguments are
 *   LOOPBACK, WIRE, HOST, PORT and BYTES, because it generates the traffic it
 *   records: "capture the stack's own traffic while running a workload
 *   underneath it".  Somebody who wants to know what a browser, an ssh session
 *   or a game's netplay is really saying has the ABI and, until this, no
 *   command.  This one generates nothing.  It opens a channel, reads it, and
 *   writes the file.
 *
 *   It captures every frame the interface handles, whichever program caused
 *   it: the tap is inside bsdsocket.library, on the path every socket in the
 *   machine shares, so a capture is not confined to the process that opened
 *   it.  The stack has to be up first -- AddNetInterface, or User-Startup.
 *
 *   IT STOPS when the packet count, the elapsed time or the file size the user
 *   asked for is reached, and on Ctrl-C.  Every one of those closes the file
 *   properly, so a capture interrupted after ten seconds is a complete pcap of
 *   those ten seconds and not a file Wireshark refuses.  With no limit at all
 *   it runs until Ctrl-C, which is what tcpdump does and what a user watching
 *   for an intermittent fault needs.
 *
 *   THE FILTER is four keywords, not an expression language: HOST, PORT, PROTO
 *   and NOT, ANDed.  bpffilter.c says what each means and why there is no
 *   parser here.  The snap length is part of the same program -- the ABI has
 *   no BIOCSSNAPLEN, and a BPF program's return value is the number of bytes
 *   to keep.
 *
 *   WHAT IT COSTS THE MACHINE is fixed before the first frame: 2 x BLEN inside
 *   the library, one BLEN buffer here, and 16 KB of write buffer.  At the
 *   defaults that is 64 KB and it never grows.  When the reader cannot keep up
 *   -- a slow disk, a fast segment -- both library buffers fill, the tap counts
 *   the frame as dropped and does not block the stack for it.  That count is
 *   printed as it happens and again at the end: a capture with holes in it must
 *   never look like a quiet network.
 *
 *   Nothing from src/.  Every call is a published bsdsocket.library LVO; see
 *   toolbpf.c, which NetTrace shares.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"
#include "toolbpf.h"

#include "aminetxduo/version.h"

const char *const tool_name = "NetCapture";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetCapture");

#define TEMPLATE \
    "OUT/K,IFACE/K,SNAP/N/K,BLEN/N/K,COUNT/N/K,SECONDS/N/K," \
    "SIZE/N/K,HOST/K,PORT/N/K,PROTO/K,NOT/S,QUIET/S"

enum
{
    ARG_OUT = 0,
    ARG_IFACE,
    ARG_SNAP,
    ARG_BLEN,
    ARG_COUNT,
    ARG_SECONDS,
    ARG_SIZE,
    ARG_HOST,
    ARG_PORT,
    ARG_PROTO,
    ARG_NOT,
    ARG_QUIET,
    ARG_ARGCOUNT
};

/*
 * 96 bytes covers Ethernet + IP + TCP with 20 bytes of options, and was
 * tcpdump's own default for twenty years.  It is also what NetTrace defaults
 * to, and the two commands spell every size the same way on purpose.
 */
#define NC_SNAP_DEFAULT     96UL
#define NC_SNAP_MIN         14UL        /* below the Ethernet header       */
#define NC_SNAP_MAX         65535UL     /* a BPF return is a byte count    */

/*
 * Half of NetTrace's, and deliberately: NetTrace runs for as long as one
 * transfer takes and can afford the largest buffer the ABI allows, while this
 * runs unattended on whatever machine the user has.  16 KB a side is about 140
 * records at the default snap length, which is a second of a busy 10 Mbit
 * segment -- far more than the reader needs, and 32 KB of somebody's pool
 * rather than 64.
 */
#define NC_BLEN_DEFAULT     16384UL

/* How long bpf_read() waits before coming back with nothing, in microseconds.
   It is also the worst case for noticing Ctrl-C. */
#define NC_READ_TIMEOUT     200000UL

/* Sanity ceilings, so a mistyped number is refused rather than acted on. */
#define NC_SECONDS_MAX      86400UL     /* a day                           */
#define NC_SIZE_MAX_KB      1048576UL   /* a gigabyte                      */

/* See toolbpf.h: this carries a 16 KB write buffer and a Shell command has a
   4 KB stack. */
static ToolBpfChan nc_cap;

static ToolBpfInsn nc_prog[TOOL_BPF_MAX_INSNS];

/* ------------------------------------------------------------- the filter */

/*
 * The line that says what is being captured, so a user who mistypes a filter
 * sees it rather than an empty file.  Built into the caller's buffer; there is
 * no allocation here and no format string that takes a string it did not
 * write.
 */
static VOID nc_describe(char *buf, ULONG buflen, const ToolBpfFilter *f,
                        const char *host_text)
{
    ULONG       n = 0;
    const char *p;
    UWORD       parts = 0;

    buf[0] = '\0';

    if (f->proto != TOOL_BPF_PROTO_ANY)
    {
        for (p = tool_bpf_proto_name(f->proto); *p != '\0' && n + 1 < buflen; )
            buf[n++] = *p++;
        parts++;
    }

    if (f->have_host)
    {
        if (parts != 0)
            for (p = " and "; *p != '\0' && n + 1 < buflen; )
                buf[n++] = *p++;
        for (p = "host "; *p != '\0' && n + 1 < buflen; )
            buf[n++] = *p++;
        for (p = host_text; *p != '\0' && n + 1 < buflen; )
            buf[n++] = *p++;
        parts++;
    }

    if (f->have_port)
    {
        char  digits[8];
        ULONG v = f->port;
        UWORD d = 0;

        if (parts != 0)
            for (p = " and "; *p != '\0' && n + 1 < buflen; )
                buf[n++] = *p++;
        for (p = "port "; *p != '\0' && n + 1 < buflen; )
            buf[n++] = *p++;

        do {
            digits[d++] = (char)('0' + (v % 10UL));
            v /= 10UL;
        } while (v != 0 && d < (UWORD)sizeof(digits));

        while (d > 0 && n + 1 < buflen)
            buf[n++] = digits[--d];

        parts++;
    }

    if (parts == 0)
        for (p = "everything"; *p != '\0' && n + 1 < buflen; )
            buf[n++] = *p++;
    else if (f->invert)
        for (p = ", inverted"; *p != '\0' && n + 1 < buflen; )
            buf[n++] = *p++;

    buf[n] = '\0';
}

/* --------------------------------------------------------------- the loop */

int main(int argc, char **argv)
{
    LONG            args[ARG_ARGCOUNT];
    struct RDArgs  *rda;
    struct Library *base;
    ToolBpfFilter   filter;
    ToolAddr        address;
    ULONG           proglen = 0;
    ToolBpfResult   why;
    const char     *out;
    const char     *iface;
    const char     *stop = "end";
    char            host_text[TOOL_ADDR_STRLEN];
    char            what[128];
    ULONG           snaplen;
    ULONG           blen;
    ULONG           want_count;
    ULONG           want_seconds;
    ULONG           want_kb;
    ULONG           started;
    ULONG           reported = 0;       /* ami_millis() of the last line    */
    ULONG           said_drop = 0;      /* the drop count already announced */
    BOOL            quiet;
    int             proto;
    int             rc = RETURN_OK;
    UWORD           i;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < (UWORD)ARG_ARGCOUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    /*
     * ReadArgs takes a leading minus, so "SNAP=-5" parses and arrives as a
     * negative LONG.  Cast to ULONG it becomes an enormous one, which the
     * range checks below catch for SNAP and BLEN and would not catch for
     * COUNT: COUNT=-1 is four thousand million packets, which is no limit at
     * all, and the command would then run until Ctrl-C having been told to
     * stop.  A limit of zero is the same mistake from the other end -- it
     * would read as "no limit" and the user meant "nothing".
     */
    if ((args[ARG_SNAP]  != 0 && *(LONG *)args[ARG_SNAP]  < 0) ||
        (args[ARG_BLEN]  != 0 && *(LONG *)args[ARG_BLEN]  < 0) ||
        (args[ARG_PORT]  != 0 && *(LONG *)args[ARG_PORT]  < 0))
    {
        tool_error("SNAP, BLEN and PORT are counts, and a negative one is a "
                   "mistake");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if ((args[ARG_COUNT]   != 0 && *(LONG *)args[ARG_COUNT]   < 1) ||
        (args[ARG_SECONDS] != 0 && *(LONG *)args[ARG_SECONDS] < 1) ||
        (args[ARG_SIZE]    != 0 && *(LONG *)args[ARG_SIZE]    < 1))
    {
        tool_error("COUNT, SECONDS and SIZE are limits: leave one out for no "
                   "limit, and give it at least 1 to set one");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    out   = (args[ARG_OUT] != 0) ? (const char *)args[ARG_OUT]
                                 : "RAM:capture.pcap";
    iface = (args[ARG_IFACE] != 0) ? (const char *)args[ARG_IFACE] : "eth0";
    quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;

    snaplen = (args[ARG_SNAP] != 0) ? (ULONG)(*(LONG *)args[ARG_SNAP])
                                    : NC_SNAP_DEFAULT;
    blen    = (args[ARG_BLEN] != 0) ? (ULONG)(*(LONG *)args[ARG_BLEN])
                                    : NC_BLEN_DEFAULT;

    want_count   = (args[ARG_COUNT] != 0)
                       ? (ULONG)(*(LONG *)args[ARG_COUNT]) : 0UL;
    want_seconds = (args[ARG_SECONDS] != 0)
                       ? (ULONG)(*(LONG *)args[ARG_SECONDS]) : 0UL;
    want_kb      = (args[ARG_SIZE] != 0)
                       ? (ULONG)(*(LONG *)args[ARG_SIZE]) : 0UL;

    if (snaplen < NC_SNAP_MIN || snaplen > NC_SNAP_MAX)
    {
        tool_error("SNAP must be between %lu and %lu bytes",
                   (LONG)NC_SNAP_MIN, (LONG)NC_SNAP_MAX);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (blen < TOOL_BPF_MIN_BLEN || blen > TOOL_BPF_MAX_BLEN)
    {
        tool_error("BLEN must be between %lu and %lu bytes",
                   (LONG)TOOL_BPF_MIN_BLEN, (LONG)TOOL_BPF_MAX_BLEN);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * A buffer that cannot hold one record captures nothing at all, and the
     * only sign of it is an empty file.  Refuse the combination instead.
     */
    if (blen < snaplen + 32UL)
    {
        tool_error("BLEN %lu is too small for a snap length of %lu",
                   (LONG)blen, (LONG)snaplen);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (want_seconds > NC_SECONDS_MAX)
    {
        tool_error("SECONDS must be %lu or fewer", (LONG)NC_SECONDS_MAX);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (want_kb > NC_SIZE_MAX_KB)
    {
        tool_error("SIZE must be %lu kilobytes or fewer", (LONG)NC_SIZE_MAX_KB);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    proto = TOOL_BPF_PROTO_ANY;
    if (args[ARG_PROTO] != 0)
    {
        proto = tool_bpf_proto_from_name((const char *)args[ARG_PROTO]);
        if (proto < 0)
        {
            tool_error("PROTO must be one of TCP, UDP, ICMP, ARP, IP, IP6");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    if (args[ARG_NOT] != 0 && args[ARG_HOST] == 0 && args[ARG_PORT] == 0 &&
        args[ARG_PROTO] == 0)
    {
        tool_error("NOT inverts a filter and there is none, so it would "
                   "capture nothing");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    base = tool_socket_open();
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /* ------------------------------------------------------- the filter -- */

    for (i = 0; i < (UWORD)sizeof(filter.host_v6); i++)
        filter.host_v6[i] = 0;

    filter.snaplen    = snaplen;
    filter.proto      = proto;
    filter.have_host  = 0;
    filter.host_is_v6 = 0;
    filter.host_v4    = 0;
    filter.have_port  = (args[ARG_PORT] != 0) ? 1 : 0;
    filter.port       = (args[ARG_PORT] != 0)
                            ? (ULONG)(*(LONG *)args[ARG_PORT]) : 0UL;
    filter.invert     = (args[ARG_NOT] != 0) ? 1 : 0;

    host_text[0] = '\0';

    if (args[ARG_HOST] != 0)
    {
        /* Through the library's own resolver, like every other command here,
           so a name works and both families do. */
        if (!tool_sock_resolve(base, (const char *)args[ARG_HOST], &address))
        {
            CloseLibrary(base);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        filter.have_host = 1;

        if (TOOL_ADDR_IS6(&address))
        {
            filter.host_is_v6 = 1;
            for (i = 0; i < 16; i++)
                filter.host_v6[i] = address.ta_V6[i];
        }
        else
        {
            filter.host_v4 = address.ta_V4;
        }

        tool_addr_text(base, &address, host_text, sizeof(host_text));
    }

    why = tool_bpf_compile(&filter, nc_prog,
                           (unsigned long)TOOL_BPF_MAX_INSNS, &proglen);
    if (why != TOOL_BPF_OK)
    {
        tool_error("%s", (LONG)tool_bpf_error(why));
        CloseLibrary(base);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    nc_describe(what, sizeof(what), &filter, host_text);

    /* ------------------------------------------------------- the channel -- */

    nc_cap.open = FALSE;

    if (!tool_bpf_start(&nc_cap, base, iface, out, snaplen, blen, nc_prog,
                        proglen))
    {
        tool_bpf_stop(&nc_cap);
        CloseLibrary(base);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    nc_cap.max_records = want_count;
    nc_cap.max_filelen = (want_kb != 0) ? (want_kb * 1024UL) : 0UL;

    /*
     * A capture of somebody else's traffic ends at a moment, and the segment
     * does not stop for it: whatever arrives between the last read and the
     * close is outside what was asked for.  NetTrace's traffic HAS stopped by
     * the time it closes, so there the same bytes mean a trace that is short.
     */
    nc_cap.expect_drained = FALSE;

    tool_bpf_read_timeout(&nc_cap, NC_READ_TIMEOUT);

    if (!quiet)
    {
        tool_say("capture: %s, snaplen %lu, buffers 2 x %lu, %lu instructions\n",
                 (LONG)iface, (LONG)snaplen, (LONG)nc_cap.buflen,
                 (LONG)proglen);
        tool_say("capture: %s -> %s\n", (LONG)what, (LONG)out);
        tool_say("capture: Ctrl-C stops it and closes the file\n");
    }

    started  = ami_millis();
    reported = started;

    /* ---------------------------------------------------------- the loop -- */

    for (;;)
    {
        ULONG now;

        (VOID)tool_bpf_drain(&nc_cap);

        if (tool_break())
        {
            stop = "break";
            break;
        }

        if (nc_cap.out.failed)
        {
            stop = "write";
            break;
        }

        if (nc_cap.limit_hit)
        {
            stop = (want_count != 0 && nc_cap.out.records >= want_count)
                       ? "count" : "size";
            break;
        }

        now = ami_millis();

        if (want_seconds != 0 && (now - started) >= want_seconds * 1000UL)
        {
            stop = "seconds";
            break;
        }

        tool_bpf_stats(&nc_cap);

        /*
         * A drop is announced the moment it happens and not only in the
         * summary: the user is watching this window, and a capture that
         * silently loses a third of the segment reads exactly like one that
         * did not.
         */
        if (!quiet && nc_cap.drop != said_drop)
        {
            tool_say("capture: %lu frames dropped, the reader is behind\n",
                     (LONG)nc_cap.drop);
            said_drop = nc_cap.drop;
        }

        if (!quiet && (now - reported) >= 5000UL)
        {
            tool_say("capture: %lu packets, %lu bytes\n",
                     (LONG)nc_cap.out.records, (LONG)nc_cap.out.filelen);
            reported = now;
        }
    }

    /* --------------------------------------------------------- the close -- */

    tool_bpf_stop(&nc_cap);

    tool_say("capture: written=%lu bytes=%lu file=%lu seen=%lu dropped=%lu "
             "short=%lu stop=%s\n",
             (LONG)nc_cap.out.records, (LONG)nc_cap.out.caplen_total,
             (LONG)nc_cap.out.filelen, (LONG)nc_cap.recv, (LONG)nc_cap.drop,
             (LONG)nc_cap.short_reads, (LONG)stop);

    if (tool_bpf_warn(&nc_cap))
        rc = RETURN_WARN;

    CloseLibrary(base);
    FreeArgs(rda);

    return rc;
}
