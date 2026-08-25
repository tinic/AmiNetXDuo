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

## The remaining levers, in expected-value order

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

## Reproducing

    cmake -B build/cmb -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
          -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_RXPROBE=ON -DAMINETXDUO_LTO=OFF
    tests/perf/run-poolshare.sh -b build/cmb -B <iface> -P <peer> -d "16" -s 15
    # then read "receive budget:" in the captured guest netstat -s

On the physical machine: install the probe library and the probe-aware
netstat, run one `iperf -s -4` receive, read `netstat -s`.  Counters
accumulate from boot; take deltas across runs on one boot.
