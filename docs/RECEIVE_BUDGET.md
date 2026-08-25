# The receive step budget, and what it convicts

2026-08-24, on the physical A1200 (68EC020, 2 MB chip, 3c589), measured by the
budget instrument on branch `event-budget` (`netstat -s`, "receive budget",
library built `-DAMINETXDUO_RXPROBE=ON -DAMINETXDUO_LTO=OFF`).  Every number
below is from that machine during a ~1.1 Mbit/s TCP receive unless marked
emulated.

## The four legs

| leg | what it spans | mean | shape |
|---|---|---:|---|
| drain | reader dequeues reply -> packet handed to IP thread | 605 us | tight, 87% under 717 us |
| baton | bsd_nx_enter(), asking -> holding | 1,078 us | 88% under 359 us, ~12% at ~8 ms, max 66 ms |
| settle | handed to IP thread -> socket receive notify | 3,630 us | steady 2.9-5.7 ms |
| fetch | notify -> recv() dequeues | 8,211 us | mode 5.7-11.5 ms, max 36 ms |

## What was ruled out, in order, with the experiment that ruled it out

1. **Packet-pool starvation.**  Same binary, `ENV:ANXDPOOLDIV` 16 vs 8 on the
   physical machine: pool 45 -> 90 packets, throughput unchanged
   (~1.16 vs ~1.19 Mbit/s), demand never exceeded ~22 packets in either arm.
   The emulated FASTMEM=0 zero-window storm (217 -> 1) does not occur at
   hardware rates.  docs/PHYSICAL_RX_A1200.md's leading hypothesis is closed
   for this rate regime.
2. **Application task priority.**  `ChangeTaskPri 2` on the receiving app:
   fetch 8.2 -> ~6.1 ms.  Real but minor; the bulk survives.
3. **Baton waiter latency.**  The `baton-latency` branch (wait one Exec
   priority up): neutral on hardware -- baton 1,135 vs 1,078 us, fetch 8.4 vs
   8.5 ms, rate 1.07 vs 1.15 Mbit/s.

## What the numbers actually say

The fetch leg's ~8 ms equals the baton leg's slow class: the woken caller
waits, and the wait ends when the stack's ladder goes quiet.  That is not a
lock held too long in the bug sense -- it is `src/thread_priorities.h`
working exactly as designed.  Adopted callers hold ThreadX priority 16;
the IP thread holds 2; during a burst the IP thread has continuous work, so
a `recv()` runs only when the burst is finished.

And the machine is not idle while the caller waits: the physical profile
(docs/PHYSICAL_RX_A1200.md) has 5.7% true idle.  The CPU is busy the whole
time the caller stands in line.  **Therefore rescheduling cannot buy
throughput.  Only removing work can.**  Any "shorten the baton hold" surgery
that moves the same instructions to a different moment moves the wait, not
the rate.

## Where the work is, per ~4 KB chunk (about 3 frames)

    3 x ISR drain-and-sum (fused copy)        profiler: 12.8% of busy total
    3 x drain leg                              ~1.8 ms measured
    1 x settle leg (TCP processing, ACKs)      ~3.6 ms measured
    1 x recv copy-out                          profiler: 6.6% of busy total
    scheduler transitions threaded through all of the above

At 29 ms per chunk observed, the measured legs plus the profiled copies
account for roughly half; the rest is TX (ACK transmission is not
instrumented yet), the scheduler transitions themselves, and ambient load.

## The settle leg dissected (2026-08-25, physical, 7 runs)

The sub-leg instrument (superproject d79b9f9c, fork dd0d8e64) splits settle
into three chained spans: defer (deliver to the IP thread picking the packet
up, witnessed from nx_ip_packet_filter), demux (pickup to
_nx_tcp_socket_packet_process(), the one probe call the fork carries), and
state (socket entry to the receive notify).  Seven 10 s receives on the
physical machine, cmlazy configuration (RXPROBE, TS off, lazy TX, mDNS on,
fixed fork), rates 1.02-1.14 Mbit/s, lost=0 throughout:

| sub-leg | what it spans | mean | share of settle |
|---|---|---:|---:|
| defer | deliver -> IP thread holds the packet | 2,505 us | 74% |
| demux | IPv4 validation, trim, TCP checks, socket lookup | 446 us | 13% |
| state | TCP state machine, ACK checking, queueing, notify | 467 us | 14% |
| settle | the whole leg, measured independently | 3,385 us | sum agrees to 1% |

Per-run means moved less than 5% around those figures; the partition is not
noisy.

**What this convicts: settle is not NetX arithmetic.**  All of NetX Duo's
per-segment processing -- header validation, trim, checksum bookkeeping,
socket lookup, the entire TCP state machine to the notify -- is
demux + state = 0.9 ms/segment, not the ~1.2-2.9 the leg's wall time
suggested.  The profiler's tail (socket processing 2.4%, packet processing
1.7%, trim 1.5%, ACK checking 1.5%) was already the whole story: many small
functions, none dominant, ~0.9 ms of honest diffuse work on a 14 MHz CPU.
There is no single NetX code path inside settle whose removal buys a
milligram more than that, and the configuration cuts (timestamps off) are
already taken.

The 2.5 ms defer span is the serial pipeline showing its shape: the sampled
chains are the serial regime (a deliver whose notify closes before the next
deliver), and for those frames the span is the reader finishing its
delivery pass, the ISR taking its preemptions, and one Exec
Signal/Wait/dispatch round trip before the IP thread holds the packet.
Most of that wall time overlaps work that must happen anyway (the ISR copy,
the drain of following frames); the removable part is the transition glue
itself, which is exactly the inventory docs/THREADING-OPTIONS.md prices for
option 4.  Settle is therefore not a lever of its own: cutting it means
cutting scheduler transitions, and that is the green-thread port's case,
not a config or a code-path fix.

## The single-copy claim, landed and measured (2026-08-25, physical)

Branch `single-copy-landing` (green-realm + the `single-copy-rx2` merge)
finally lands task #6: the device drains a PIO card's FIFO straight into the
posted CMD_READ's packet with the Internet checksum fused into the drain
(`ANXD_S2_RX_DIRECT`/`RX_FILLED`, netdev_direct.c), removing the ISR-level
staging copy the old profile priced inside its 12.8% fused-copy share.  The
three-byte-tail sum defect that killed IPv4 in the first attempt (fixed in
b3c1b124, guarded by IoSumDrill, 264 checks in-guest) did not recur: v4 DHCP
bound on both deploy boots and rx_err_verify stayed 0 across every frame.

Engagement on the real 3c589 is total and visible: netstat's new
"direct fills" line (the shim counts its own completions; the netstatus
record now carries it) reads equal to "copy/direct fill" and to "summed
while filling" -- every received frame took the claimed, fused-sum lane.
On the emulated dp8390 the same line reads 100% with summed 0, because the
remote-DMA core completes unsummed and the verifier walks at task level, by
design.  The LANCE stages and fuses in the hook, also by design.

The rate did not move: six warm runs 1199/1207/1190/805/1085/1228 kbit/s
(client-side, same tool as cycle 4), mean 1119 vs the cycle-4 green2 mean
1132 -- minus 1.1%, inside the pre-registered plus/minus 2% neutral band.
The 805 is the known mDNS-CPU dip class (holds ring stayed bounded).  A
storm run held 957 kbit/s with zero new holds over 50 ms.  Drain eased 605
to 591 us; the other legs sat flat.  **Honest reading: the staging copy's
CPU was real but its removal hides under wall overlap on a 94%-busy
machine -- the ISR still drains the same bytes off the same slow port, and
the freed cycles are absorbed by the waits the budget already names.  The
claim ships anyway: one interrupt-level copy fewer, the checksum for free,
zero errors, and the fused drain is the foundation any faster port I/O
(task #4's movem path) multiplies against.**

1. **Fewer, larger recv() bites.**  Fetch is paid per recv() call.  An
   application reading 32 KB instead of 4 KB pays it an eighth as often.
   Zero stack changes; measure iperf with a larger read size first -- if the
   rate moves the way the arithmetic says, application guidance (and the
   dist iperf default) is the cheapest real win on the table.
2. **Cheaper settle.**  CLOSED 2026-08-25 by the dissection above: the
   arithmetic inside settle is 0.9 ms/segment and diffuse, the
   configuration cuts are taken, and the rest of the leg is scheduling
   overlap.  The lever folds into option 4's transition-glue case.
3. **An ACK/TX budget leg.**  The transmit half of every received segment is
   uninstrumented.  Extend the budget before cutting anything there.
4. **Scheduler transition cost.**  ~2 baton passes per frame, each an Exec
   Signal/Wait pair with a context save.  The event-log branch counted 6,700
   transitions in a session; at even 200 us each that is a second and a half.
   Worth one measurement (a transitions-per-second figure during transfer)
   before any design work.

## Calibration: other stacks over the same device (2026-08-25, EMULATED)

The ~1.19-1.23 Mbit/s physical figure has never had a reference point on the
same machine: the oft-quoted AmiTCP 906 KB/s was an X-Surf-100 on somebody
else's hardware and driver, and comparing across that boundary is exactly the
conflation docs/PHYSICAL_RX_A1200.md warns about.  The calibration question
is whether a mature foreign stack, driving OUR anxnet.device on THIS class of
machine, is faster (stack-wide gap remains) or not (we are at the
hardware/PIO ceiling).

The emulated leg is done.  One guest recipe (the run-smbmount.sh
foreign-stack pattern): full Workbench 3.1 SYS:, ne2000_pcmcia bridged,
anxnet.device from this branch's cmgp build in DEVS:Networks, static IPv4,
the same C:iperf binary in every arm, the same third-machine sender
(iperfpeer.py, 10 s tcp sends), four receives per arm, byte counts matched
peer-side, lost=0 throughout:

| stack | four runs, kbit/s | mean |
|---|---|---:|
| AmiTCP_NG 4.1.5, 68020 | 2145 / 2138 / 2141 / 2144 | 2142 |
| ours (this branch) | 2771 / 2777 / 2768 / 2795 | 2778 |
| Roadshow demo 1.15 | 2755 / 2756 / 2761 / 2756 | 2757 |

Emulated rates are a property of the host as much as the guest and do NOT
transfer to hardware; the ranking on one rig in one sitting is the signal.
What it says: **our stack is not behind either foreign stack over the same
driver in the same machine -- it leads Roadshow by a hair and AmiTCP_NG by
~30%.**  If the physical arm agrees, the remaining campaign gap is
hardware/PIO ceiling, not stack-wide inefficiency, and the levers left are
the ones the budget already names (option 4 glue, task #4 movem port I/O).
The physical arm (self-restoring boot, stack swapped under the same rig) is
prepared and pending its machine slot; until it runs, this section claims
the emulated ranking only.

AmiTCP_NG operational facts, learned the hard way in the rehearsal and
needed by whoever runs the physical arm:

- Source: github.com/MW0MWZ/AmiTCP_NG v4.1.5 (GPL fork of AmiTCP 3.0b2,
  Roadshow-compatible ABI 4.1).  The 68020 archive runs on an emulated
  68EC020 A1200; no FPU dependency observed.
- **rexxsyslib.library is a hard dependency, and its absence is a SILENT
  WEDGE**: AddNetInterface never returns, nothing is logged, the boot looks
  dead.  Stock Workbench 3.1 ships the library and the physical machine has
  it (33392 bytes); RexxMast does NOT need to be running (the rehearsal
  never started it).
- No usergroup.library dependency (unlike AmiTCP classic).  ROM libraries
  otherwise: dos, intuition, utility, timer.device.
- It wants AmiTCP: assigned (db/AmiTCP.config, db/netdb) and
  DEVS:NetInterfaces files in Roadshow's key=value format; a generic
  SANA-II device= name is looked up in DEVS:Networks.  Its C: tools shadow
  ours by name -- stage them in their own drawer, never over C:.
- A bare-drive guest (no Workbench) wedges even with rexxsyslib staged;
  the full WB3.1 SYS: is part of the recipe.  Judge foreign-stack boots
  only by guest-written marker files, never by our rig's probes.

## Reproducing

    cmake -B build/cmb -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
          -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_RXPROBE=ON -DAMINETXDUO_LTO=OFF
    tests/perf/run-poolshare.sh -b build/cmb -B <iface> -P <peer> -d "16" -s 15
    # then read "receive budget:" in the captured guest netstat -s

On the physical machine: install the probe library and the probe-aware
netstat, run one `iperf -s -4` receive, read `netstat -s`.  Counters
accumulate from boot; take deltas across runs on one boot.
