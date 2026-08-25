/*
 * toolbpf, a capture channel from a command's side of the ABI: published
 * bsdsocket.library LVOs only, shared by NetTrace and NetCapture. ToolBpfChan
 * is BLEN + 16 KB and must be a file-scope static -- a Shell command runs on a
 * 4 KB stack.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TOOLBPF_H
#define AMINETXDUO_TOOLBPF_H

#include "tools.h"

#include "bpffilter.h"
#include "toolpcap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What BIOCSBLEN will accept, from include/aminetxduo/bpf.h. */
#define TOOL_BPF_MIN_BLEN       32UL
#define TOOL_BPF_MAX_BLEN       0x8000UL

/*
 * A record is its bpf_hdr plus the snapshot, so BLEN >= SNAP +
 * TOOL_BPF_SNAP_SLACK or bpf_read() never returns a whole one. That fixes the
 * SNAP ceiling: above TOOL_BPF_MAX_SNAP no legal BLEN could carry it.
 */
#define TOOL_BPF_SNAP_SLACK     32UL
#define TOOL_BPF_MIN_SNAP       14UL    /* below the Ethernet header */
#define TOOL_BPF_MAX_SNAP       (TOOL_BPF_MAX_BLEN - TOOL_BPF_SNAP_SLACK)

typedef struct ToolBpfChan
{
    struct Library *base;
    LONG            channel;
    BOOL            open;

    UBYTE          *buf;            /* the bpf_read() destination           */
    ULONG           buflen;         /* what BIOCSBLEN actually granted      */

    BPTR            fh;
    ToolPcap        out;

    /*
     * Stop writing at this many records or this many bytes of file. 0 is no
     * limit. Enforced inside the drain, the only place a record is counted.
     */
    ULONG           max_records;
    ULONG           max_filelen;
    BOOL            limit_hit;

    /*
     * Is the channel expected to be EMPTY when the capture ends? TRUE for a
     * command whose traffic has stopped by then; FALSE for one that stops on a
     * deadline while the segment carries on. tool_bpf_start() sets TRUE.
     */
    BOOL            expect_drained;

    ULONG           short_reads;    /* bpf_read() said no record fits       */
    ULONG           recv;           /* BIOCGSTATS bs_recv, at the last look */
    ULONG           drop;           /* BIOCGSTATS bs_drop                   */
    ULONG           left;           /* FIONREAD at the end: must be 0       */
} ToolBpfChan;

/*
 * Open a channel on `iface`, size its buffers, install `prog`, and open `out`
 * for writing as a pcap file with `snaplen` in its header. NULL `prog` installs
 * accept-everything truncated to `snaplen`. Prints its own refusal, returns
 * FALSE; the caller must still call tool_bpf_stop().
 */
BOOL tool_bpf_start(ToolBpfChan *c, struct Library *base, const char *iface,
                    const char *out, ULONG snaplen, ULONG blen,
                    const ToolBpfInsn *prog, ULONG proglen);

/* How long bpf_read() waits for the first record, in microseconds. 0, the
   default, is "do not wait". A caller whose only job is to read wants a wait,
   or it spins. */
VOID tool_bpf_read_timeout(ToolBpfChan *c, ULONG micros);

/* Move everything the channel has buffered into the pcap file, and answer how
   many records that was. Stops early, and sets limit_hit, at a limit above. */
ULONG tool_bpf_drain(ToolBpfChan *c);

/* Refresh recv and drop from BIOCGSTATS.  Cheap; safe to call in a loop. */
VOID tool_bpf_stats(ToolBpfChan *c);

/* Final drain, final statistics, flush and close the file, free the buffer.
   Safe on a channel that never opened, and safe twice. Closed here rather than
   at exit, so a capture stopped with Ctrl-C is a complete pcap. */
VOID tool_bpf_stop(ToolBpfChan *c);

/* The three ways a capture is not what it looks like: a write that failed, a
   frame the channel dropped, bytes still buffered at the end. TRUE if it said
   anything. */
BOOL tool_bpf_warn(const ToolBpfChan *c);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TOOLBPF_H */
