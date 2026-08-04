/*
 * tcpdrill, the synthetic SANA-II device.
 *
 * packetdrill drives a real stack from below through a TUN device: the script
 * says what the application asks for and what must appear on the wire, and the
 * tool is itself the remote peer, so it can send a segment with any sequence
 * number, window, option, order and timing.
 *
 * None of that is reachable from the host here.  FS-UAE's A2065 has exactly
 * three backends, `slirp`, `slirp_inbound` and `none` (grep the binary; there
 * is no tap, no bridge and no libpcap on this build), and SLIRP is a
 * user-mode NAT that terminates the guest's TCP connection and re-originates
 * it as its own.  A host peer therefore never sees the guest's sequence
 * numbers, never sees its flags, and cannot put a byte of its own choosing into
 * the guest's receive path.  That is why tests/tools/netpeer.py and
 * tests/peer/httppeer.py are stream peers, and no work on them changes it.
 *
 * So the TUN device goes inside the guest.  This file is an AmigaOS Exec
 * device, created at run time with MakeLibrary()/AddDevice(), which implements
 * enough of SANA-II for src/sana2/ to open it, configure it, take it online and
 * run its reader threads against it.  DEVS:NetInterfaces/tap0 names it, so the
 * stack brings it up exactly as it brings up a2065.device, through the same
 * code, with no build-time switch and nothing in src/ aware that it is being
 * tested.  That gives:
 *
 *   * every frame the stack transmits arrives here complete, timestamped, in
 *     order, and is never lost, CMD_WRITE is a function call, not a wire;
 *   * every frame the stack receives is one this harness composed, byte for
 *     byte, including sequence numbers it has no business knowing;
 *   * the peer is not a TCP implementation, so nothing answers by accident.
 *     A segment sent to the fake peer stays unanswered until the script says
 *     otherwise, which is what makes an RTO measurable at all.
 *
 * It is not a network device.  It has no wire, it drops nothing on its own,
 * and its timing is the emulated machine's, not Ethernet's.  Anything measured
 * through it is the stack's behaviour with the link taken out of the question:
 * the right instrument for conformance and the wrong one for throughput.
 * Throughput lives in tests/trace/, over the real A2065.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TCPDRILL_TAPDEV_H
#define TCPDRILL_TAPDEV_H

#include <exec/types.h>

#define TAP_DEVICE_NAME     "tcpdrill.device"

/* One frame does not exceed 14 + 1500. Rounded up for the odd oversize. */
#define TAP_FRAME_MAX       1600

/* How many transmitted frames may sit uncollected.  Overruns are counted and
   reported rather than silently wrapping; the script engine drains after every
   step, so this only has to cover a burst the guest emits inside one poll
   interval. */
#define TAP_TX_SLOTS        96

typedef struct TapStats
{
    ULONG   tx_frames;          /* CMD_WRITEs completed                    */
    ULONG   tx_overrun;         /* frames the ring could not hold          */
    ULONG   rx_delivered;       /* frames handed to a queued CMD_READ      */
    ULONG   rx_no_reader;       /* frames dropped: no read of that type    */
    ULONG   rx_copy_failed;     /* the stack's CopyToBuff hook said no     */
    ULONG   reads_queued;       /* CMD_READs the device holds right now    */
    ULONG   online_count;       /* S2_ONLINEs seen                         */
    ULONG   offline_count;      /* S2_OFFLINEs seen                        */
} TapStats;

/*
 * Install the device into ExecBase's device list.  Must be called before
 * anything opens bsdsocket.library, because the stack opens its interfaces
 * when it starts and OpenDevice() only finds what is already there.
 */
LONG tap_install(const UBYTE *mac);
VOID tap_remove(VOID);

/* TRUE between S2_ONLINE and S2_OFFLINE. */
BOOL tap_is_online(VOID);

/*
 * Collect the oldest transmitted frame.  Returns its length, or 0 when the
 * ring is empty.  `stamp` receives the E-Clock reading taken inside BeginIO,
 * i.e. the moment the stack handed the frame over.
 */
ULONG tap_tx_get(UBYTE *buf, ULONG max, ULONG *stamp);

/*
 * Hand one complete Ethernet frame to the stack.  Returns 0 on delivery, -1
 * when no CMD_READ of that EtherType was outstanding, a real SANA-II
 * behaviour rather than an error, so the caller decides what it means.
 */
LONG tap_rx_put(const UBYTE *frame, ULONG len);

/* How many reads are outstanding for one EtherType (0 = any). */
ULONG tap_reads_for(UWORD ether_type);

VOID tap_get_stats(TapStats *out);

/* E-Clock ticks per second, and a reading, both from timer.device. */
ULONG tap_eclock_rate(VOID);
ULONG tap_eclock_now(VOID);

#endif /* TCPDRILL_TAPDEV_H */
