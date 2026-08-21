/*
 * toolbpf, a capture channel from a command's side of the ABI.
 *
 * Nothing from src/.  Every call is a published bsdsocket.library LVO,
 * including the bpf_* ones, reached by an inline jsr through the library base
 * exactly as toolsock.c reaches the socket half.  A command that linked
 * src/bpf/ would get its OWN copy of the channel table and capture nothing at
 * all, which is why this is a client of the library and not a user of the
 * archive.
 *
 * Shared by NetTrace and NetCapture.  What is here is the part that is the
 * same in both: opening a channel, sizing its buffers, installing a filter,
 * turning captured records into pcap records, and the three diagnostics that
 * say a trace has holes in it.  What is NOT here is why a capture stops --
 * NetTrace stops when its workload finishes, NetCapture when the user's limit
 * is reached -- and what either of them prints.
 *
 * THE RECORD, AND WHERE IT COMES FROM
 *
 * bpf_read() returns whole `struct bpf_hdr` records, back to back, at the
 * offsets include/aminetxduo/bpf.h pins, with the next record starting at
 * BPF_WORDALIGN(hdrlen + caplen) from the start of this one.  The fields are
 * in the machine's own order, which on 68k is the order a pcap file wants.
 *
 * MEMORY, WHICH IS THE POINT OF EVERY SIZE HERE
 *
 * A capture on a 1 MB machine must not be the reason it runs out.  Three
 * allocations, all of them bounded before anything is captured:
 *
 *   2 x BLEN   inside the library, allocated by BIOCSETIF, never grown.  This
 *              is the only buffering there is: when the reader falls behind
 *              and both halves are full, the tap counts a drop and the frame
 *              is gone.  BIOCGSTATS reports that count and the caller is
 *              expected to put it in front of the user.
 *   BLEN       here, one AllocVec, the destination of bpf_read().
 *   16 KB      the pcap write buffer, inside ToolPcap, inside this struct.
 *
 * ToolBpfChan is therefore about BLEN + 16 KB and CANNOT be a local: an
 * AmigaDOS Shell command runs on a 4 KB stack, and nettrace.c's own history
 * records what a 16 KB automatic did to the machine (an F-line trap and a
 * reset, landing in unrelated code).  Every caller declares one file-scope
 * static.
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
 * A record is its bpf_hdr plus the snapshot, so a buffer has to hold both or
 * bpf_read() never returns a whole one.  Both capture commands require
 * BLEN >= SNAP + TOOL_BPF_SNAP_SLACK for that reason.
 *
 * Which fixes the ceiling on SNAP: the largest BLEN is TOOL_BPF_MAX_BLEN, so
 * a snapshot above TOOL_BPF_MAX_SNAP cannot be paired with any legal BLEN.
 * Offering 65535, the largest count a BPF filter can return, and then
 * rejecting every BLEN that could carry it sent the user to fix an argument
 * that has no working value.
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
     * Stop writing at this many records or this many bytes of file.  0 is no
     * limit.  Enforced inside the drain, because that is the only place a
     * record is counted, and a limit checked after a drain overshoots it by
     * however many records one read happened to carry.
     */
    ULONG           max_records;
    ULONG           max_filelen;
    BOOL            limit_hit;

    /*
     * Is the channel expected to be EMPTY when the capture ends?
     *
     * TRUE for a command whose traffic has stopped by then: anything still
     * buffered is a trace that stops short of the last few frames, and
     * tool_bpf_warn() says so.  FALSE for one that stops on a deadline while
     * the segment carries on, where whatever arrives in the last millisecond
     * is by definition outside what was asked for -- reported there, it is a
     * warning on every run and means nothing.  Set by tool_bpf_start() to
     * TRUE; NetCapture clears it.
     */
    BOOL            expect_drained;

    ULONG           short_reads;    /* bpf_read() said no record fits       */
    ULONG           recv;           /* BIOCGSTATS bs_recv, at the last look */
    ULONG           drop;           /* BIOCGSTATS bs_drop                   */
    ULONG           left;           /* FIONREAD at the end: must be 0       */
} ToolBpfChan;

/*
 * Open a channel on `iface`, size its buffers, install `prog`, and open `out`
 * for writing as a pcap file with `snaplen` in its header.
 *
 * `prog` may be NULL, in which case a one-instruction accept-everything
 * program truncating to `snaplen` is installed: the ABI has no BIOCSSNAPLEN,
 * and a filter's return value is what sets the snap length.
 *
 * Prints its own refusal and returns FALSE.  The caller must still call
 * tool_bpf_stop() to give back whatever was reached.
 */
BOOL tool_bpf_start(ToolBpfChan *c, struct Library *base, const char *iface,
                    const char *out, ULONG snaplen, ULONG blen,
                    const ToolBpfInsn *prog, ULONG proglen);

/*
 * How long bpf_read() waits for the first record, in microseconds.  0, the
 * default, is "do not wait", which is what a caller draining between its own
 * socket operations wants.  A caller whose only job is to read wants a wait,
 * or it spins.
 */
VOID tool_bpf_read_timeout(ToolBpfChan *c, ULONG micros);

/*
 * Move everything the channel has buffered into the pcap file, and answer how
 * many records that was.  Stops early, and sets limit_hit, when a limit above
 * is reached.
 */
ULONG tool_bpf_drain(ToolBpfChan *c);

/* Refresh recv and drop from BIOCGSTATS.  Cheap; safe to call in a loop. */
VOID tool_bpf_stats(ToolBpfChan *c);

/*
 * Final drain, final statistics, flush and close the file, free the buffer and
 * give the channel back.  Safe on a channel that never opened, and safe twice.
 *
 * The file is closed here and not left to the process exiting, so a capture
 * stopped with Ctrl-C is a complete, readable pcap of everything up to that
 * moment rather than a file missing its last buffer.
 */
VOID tool_bpf_stop(ToolBpfChan *c);

/*
 * The three ways a capture is not what it looks like: a write that failed, a
 * frame the channel dropped, and bytes still buffered at the end.  Printed by
 * both commands, so worded once.  Answers TRUE if it said anything.
 */
BOOL tool_bpf_warn(const ToolBpfChan *c);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TOOLBPF_H */
