# The green realm: option 4's prototype, and where it stands

2026-08-25 (cycles 2-4), branch `green-realm` (off `event-budget`), repo
/home/turo/anxd-pollfix.  The design brief is docs/THREADING-OPTIONS.md
section 5; the glue inventory it removes is docs/RECEIVE_BUDGET.md's
(~0.9-1.2 ms/frame of Exec Signal/Wait/dispatch round trips at ~1.15 Mbit/s
on the physical A1200).  Cycles 3 and 4 took it to the physical
A1200 under the hard rules; the cycle-4 rematch below is the standing
verdict, and the machine now runs the green2 build.

## What is built and real

`-DAMINETXDUO_GREEN_REALM=ON` (default OFF; OFF builds the baton port
behavior-for-behavior -- all four local configurations compile, green and
baton, probe and plain).

- **Real m68k stack switching.**  `_tx_thread_stack_build()` builds a
  genuine initial frame -- eleven zeroed callee-saves under
  `_tx_green_thread_begin` -- on the stack `tx_thread_create()` supplied;
  no Exec Task is created.  `_tx_green_switch()`
  (port/threadx-amiga/src/tx_green_switch.S) is the whole context switch:
  movem.l of d2-d7/a2-a6, SP swap, movem.l back, rts.  The switch protocol
  is one Forbid() held across the switch, one Permit() at the resumed
  side, which makes the `_tx_thread_current_ptr` surgery and the switch a
  single atom with no state of its own.
- **The realm scheduler.**  `_tx_thread_schedule()` (green build) runs on
  the master Task -- the realm -- and dispatches a GREEN thread by
  switching into its saved context; `_tx_thread_system_return()` switches
  back.  A green-to-green handoff is therefore a function call plus ~40
  bytes of movem, no Exec involvement at all.  The old scheduler loop is
  intact under the other #ifdef arm.
- **One idle Wait() for the whole stack.**  A green thread that must sleep
  on Exec signals calls `tx_amiga_green_wait(mask)`: it registers, suspends
  greenly, and the realm's single `Wait()` covers the scheduler signal plus
  the union of registered waiters' masks.  Latched bits are delivered
  before every dispatch pass, and only REGISTERED waiters' bits are ever
  consumed, so a signal arriving while its thread runs stays latched for
  that thread's next wait -- no lost wakeups.  Priority keeps the baton
  model's granularity (service-call boundaries), and the reader at TX pri 1
  is dispatched ahead of the IP thread at every pass, which preserves
  thread_priorities.h's frame-drop guarantee.
- **The request gate (cycle 2, commit b7049aa5): the adopted-caller
  boundary is BUILT.**  In a green build `bsd_nx_enter()` (netx_call.c,
  the one choke point every vector passes) no longer adopts the calling
  Task.  `tx_amiga_gate_call()` captures the caller's continuation -- what
  a context switch saves IS the request -- hands it to a cached per-opener
  GREEN proxy thread, and parks the Task on a small side stack in one
  plain `Wait()`.  The vector body runs unchanged, on the caller's own
  stack memory, inside the realm: a NetX suspension mid-recv is a green
  switch, not an Exec round trip, and the completion is ONE boundary
  Signal at `bsd_nx_leave()`, after which the owner resumes the leave-side
  context and runs the epilogue as itself.  The region is migratable
  because the bracket discipline already demanded it ("inside one, nothing
  must block on anything except ThreadX").  Owner state the body consults
  -- the Ctrl-C break mask -- moves behind `bsd_break_signals()`: the
  parked owner collects its own bits, the sliced waits observe them
  through the gate, and they are re-posted at return so EINTR still leaves
  the signal set.  Anything that cannot gate (foreign task, failed bind,
  kernel down, nested stack context) falls back to the adopted-baton
  bracket, which remains fully wired, and is counted.  A dead opener's
  gate is reaped by the heartbeat when safe (dormant or suspended
  mid-flight) and deferred a beat when the realm is inside the proxy that
  instant.  Census: "gated brackets / fell back" -- a 15 s receive shows
  ~1300 gated, 0 fallbacks, and the realm-level external handoffs stay at
  ~4 (boot-time only).
- **The tick merge (cycle 2, commit 3b91b430).**  Everything one tick
  wakeup does lives in one shared service, `_tx_amiga_tick_deliver()`
  (tx_initialize_low_level.c).  In a green build, once the VERTB source
  validates, the server signals the REALM (its own scheduler signal -- no
  new bit) and the realm's loop services the tick in passing at every
  scheduler pass; the per-frame Exec switch to the pri-20 tick Task is
  gone.  The task remains as the once-a-second watchdog floor, and the
  whole tick on machines whose VERTB never fires or that fell back to
  timer.device requests.  The E-Clock stays the time base for both
  callers, so whoever runs next delivers exactly the periods that
  elapsed; a 16 s run reads 805 ticks in 16109 ms (49.97 Hz), 0 clipped,
  0 lost.  The stated trade: the wheel walk waits out a green thread's
  pass instead of preempting it (bounded by the longest pass -- the
  mDNS-yield bound -- and the watchdog's second); the CLOCK does not
  care, only wheel delivery, and the observed worst wheel skew is 2 ticks.
- **ISR bracket unchanged.**  context_save/context_restore keep their
  Forbid()+system_state shape; TX_TIMER_PROCESS_IN_ISR stays true.  From
  any foreign context `_tx_amiga_dispatch_inline()` DECLINES a green
  target and pokes the realm, so only the realm ever performs a switch.
- **Converted Exec-blocking sites** (the design's failure mode is a green
  thread blocking in Exec -- it would sleep the whole realm):
  - the SANA-II reader loop's release-Wait-reacquire bracket ->
    `tx_amiga_green_wait(wake|reap)` (sana2_rx.c);
  - synchronous device commands -> `ami_sana2_do_io()`, which is
    DoIO-in-the-bracket from ordinary contexts and SendIO + green wait from
    a green one (sana2_device.c; covers NX_LINK_ENABLE on the IP thread and
    the teardown CMD_FLUSH on the reader).
- **The stray-Wait net (probe builds) -- REPAIRED AND PROVEN in cycle 2
  (commit 9b7685a2).**  GREEN_REALM+RXPROBE routes every `Wait()` in the
  netstack/SANA-II/bsdsocket sources through `ami_green_checked_wait`:
  ordinary contexts pass straight through; a green-context Wait() is
  converted to a green wait and counted in `gs_stray_wait`, which MUST
  read zero.  Cycle 2's injected-stray drill found the net HANGING DEAD
  in the netstack/sana2 sources: their headers defined the wrapper before
  <proto/exec.h> had expanded, so the NDK's inline Wait macro silently
  replaced ours in any TU that included it later (-isystem suppresses the
  redefinition warning).  The repair forces the NDK header first, then
  takes the name.  Re-drilled: a planted Wait converts, the run completes,
  STRAY reads exactly 1; removed, it reads 0.
- **Census.**  Nine counters appended to NetStatusRxBudget and printed by
  netstat ("green:" lines): switches, external handoffs, idle waits, waits
  latched/slept, strays, gated brackets, gate fallbacks, and realm signal
  bits out.  Zero from a baton build.
- **The signal-bit audit (cycle 2, commit 33d47f85).**  Every green
  thread's CreateMsgPort()/AllocSignal() draws from the realm Task's 16
  allocatable bits.  Inventory: scheduler signal 1, per interface 2
  (reader reply port + TX reap), transient 1 per in-flight synchronous
  device command; a one-interface receive reads 5 of 16.  Exhaustion
  answers S2ERR_NO_RESOURCES cleanly; a probe-build tripwire warns if two
  waiter masks ever overlap (the one shape in which delivery could eat a
  bit another thread is owed).  The gate costs the realm NOTHING here:
  its completion bit is the OWNER's.
- **green_wait edges hardened (cycle 2, commit 1a3bce8a).**  A thread
  terminated while registered is purged twice over
  (TX_THREAD_TERMINATED_EXTENSION -> _tx_green_forget, plus
  _tx_green_deliver refusing dead slots); the waiter-table overflow
  backoff is iterative (no unbounded recursion); the zero-return contract
  (a resume that was not a delivery collects nothing; callers loop) is
  stated at the source and honoured by both existing callers.

## Gate ladder, cycle 2 (emulator: Amiberry on playhouse3)

| rung | what | result |
|---|---|---|
| a | four local configs compile (green/baton x probe/plain) | PASS, after every item |
| b | host tier, tools/ci.sh host (97 tests) | PASS ("all green"), after every item |
| c | run-ifdhcp SLIRP | PASS 22s all-ok, after items 1, 2, 4; again mid-soak |
| d | run-poolshare bridged | PASS after every item: 2.63-2.73 Mbit/s, lost=0, zw=0, census live, 0 STRAY |
| e | run-iperf SLIRP | every reachable arm passing + the documented guest-as-server skip |
| f | cards tier, run-cardsweep -c a2065,ne2000_pcmcia | PASS: both cards, both directions carried, TCP+UDP, byte counts match peer |
| g | lossgate | BLOCKED on this rig: the peer (playhouse2) has no ~/tc-cap and no passwordless sudo to bless one.  One command unblocks it: `cp /usr/sbin/tc ~/tc-cap && sudo setcap cap_net_admin+ep ~/tc-cap` on the peer. |
| h | injected-stray drill | PROVEN: planted green-context Wait() converts, run completes, STRAY == 1; and it caught the dead net first (see above) |
| i | 12-run soak + ifdhcp mid-soak | PASS: 12/12 consecutive poolshare RC=0, 2.572-2.652 Mbit/s, lost=0 and zero_windows=0 throughout, STRAY 0 every run, gate ~1240 brackets/run with 0 fallbacks, 5/16 realm bits steady; ifdhcp mid-soak rc=0.  ~8 minutes of continuous boots with no hang. |

## Emulated numbers (directional ONLY -- the emulator charges per
## instruction and batches interrupts; the verdict lives on the A1200)

Cycle-1 A/B (baton vs green core, no gate): rate +3.2%, fetch -23%,
defer/settle ~flat, drain flat.

Cycle-2 A/B (baton 4 runs vs green+gate+tick-merge 12 soak runs, same
rig, run-poolshare bridged -d 16 -s 15, per-run means averaged):

| | baton (4) | green (12) | delta |
|---|---:|---:|---:|
| rate, kbit/s | 2682 (2720/2668/2617/2724) | 2601 (2572-2652) | -3.0% |
| fetch leg mean | 4083 us | 4348 us | +6.5% |
| defer leg mean | 2395 us | 2433 us | +1.6% |
| settle leg mean | 2961 us | 2985 us | +0.8% |
| bracket entry | 222 us (adopt fast path) | 585 us (gate submission) | +363 us |
| gated brackets / STRAY per run | 0 / 0 | ~1240 / 0 | |

Reading it honestly: ON THIS EMULATOR the gate costs ~3% of rate,
and the cost is exactly its arithmetic -- ~1240 brackets x ~0.36 ms of
added submission round trip = ~3% of 15 s.  The emulated baton arm's
bracket is nearly free because poolshare's receiver runs BEHIND the
wire: recv() nearly always finds data queued, the adopt fast path takes
a free baton with no Exec switch at all, and the mid-recv suspension the
gate exists to cheapen barely occurs -- while Amiberry prices the Exec
round trips the gate ADDS at almost nothing, so its costs show and its
savings cannot.  On the physical A1200 the regime inverts: the machine
is 94% busy, the baton is rarely free mid-transfer (the fetch leg's
7-8 ms queue IS the adopt slow path), and what the gate removes there
-- the caller's Exec wake per NetX resume, the re-entry glue in fetch
-- is real CPU this emulator cannot represent.  The physical A/B stays
the verdict, judged by defer-must-SHRINK, transitions/s, and rate over
>=4 warm runs per arm.  If the hardware agrees with the emulator
instead, the documented fallback is a gate fast path: take the free
baton like the old adopt path when the realm is idle, submit only when
the stack is contended -- the correctness architecture (one idle Wait)
is unaffected by where the fast path lands.

## THE PHYSICAL A/B (cycle 3, 2026-08-25) -- the verdict

Artifact: green-realm @23defc0c, netxduo dd0d8e64, RXPROBE ON, LTO OFF,
TS OFF, TX_LAZY_COLLECT ON, MDNS ON, GREEN_REALM ON -- bsdsocket.library
404564 B + netstat 43520 B (build/cmgp on playhouse3).  Emulator gate of
the EXACT artifact first: ifdhcp SLIRP PASS 22 s all-ok; three clean
bridged poolshare runs 2.699/2.713/2.651 Mbit/s, lost=0, STRAY=0, ~1300
gated brackets/run with 0 fallbacks, 5/16 realm bits.  Deployed under the
hard rules (PUT + size-verify + staged ladder); **the green realm BOOTED
FIRST TRY on the real A1200** (back in 22 s) and survived two boots, six
transfers, and every census clean.

Control = the settle/baton build (400580), 4 warm runs the same evening,
same tool, same LAN; green 6 warm runs.  Per-run leg means by
counter-weighted netstat deltas:

| | control (4) | green (6) | delta |
|---|---:|---:|---:|
| rate, Mbit/s | 1.113/1.136/1.141/1.121 = **1.128** | 1.122/1.097/1.123/1.126/1.130/1.116 = **1.119** | **-0.8%** |
| defer leg | 2469 us | 2704 us | +9.5% |
| settle leg | 3371 us | 3569 us | +5.9% |
| fetch leg | 6347 us | 8876 us | +40% |
| baton leg (bracket entry) | 941 us | 1910 us | +103% |
| ack leg (wall) | 13715 us | 11198 us | -18% |
| post leg | 1495 us | 1117 us | -25% |
| drain leg | ~609 us | ~591 us | flat |
| sched handoffs/frame | 0.53 | **0.00** (7 boot-time total) | eliminated |
| lost / STRAY / fallbacks | 0 / - / - | 0 / 0 / 0 | clean |

The four pre-registered criteria: (a) defer did NOT shrink -- it grew
9.5%; (b) handoffs per frame PASSED, 0.53 -> 0.00, the realm really does
internalize every transition; (c) rate -0.8%, inside the +-2 noise band,
nowhere near the +5 gate; (d) lost=0, STRAY=0, 0 fallbacks throughout.

**VERDICT: NO-GO as built; the fallback is the finding.**  The hardware
sided with the emulator, and for the emulator's stated reason: the
unconditional gate submission costs ~1 ms at every bracket entry (baton
941 -> 1910 us) and stretches fetch by ~2.5 ms (the recv caller now pays a
submission round trip where the adopt fast path often took a free baton),
which exactly cancels the real wins (ack wall -2.5 ms, post -0.4 ms,
handoffs -> 0).  Mid-transfer the baton is takeable far more often than
the 94%-busy model assumed.  The **free-baton fast path** (take a free
baton like old adopt when the realm is idle, submit only under
contention) is therefore the discriminating next item -- the correctness
architecture (realm, one idle Wait, tick merge, census) is proven on
Gayle hardware and keeps criterion (b)'s clean sweep.

**Regression found and filed: the mDNS per-record yield bound does not
hold inside the realm.**  Holds ring on hardware showed mDNS Thread
passes of 100-536 ms again (site yield, states 0/7; +1..+26 over-50 ms
per run) versus the fix's <=74 ms bound under baton.  Rates stayed
healthy and lost=0 this session (ambient LAN only), but under a burst
storm the old stall mode would be back: either _nx_mdns_yield's
tx_thread_relinquish does not actually rotate the realm the way baton
preemption did, or the hold instrument mis-spans green tenure.  Must be
root-caused before any green build ships.

## CYCLE 4 (2026-08-25): the fast path, the relinquish repair, and the rematch

Artifact: green-realm @aef6e005, threadx fork @8ddf646e, netxduo dd0d8e64,
same configuration as cycle 3 (RXPROBE ON, LTO OFF, TS OFF, LAZY ON, MDNS
ON, GREEN ON) -- bsdsocket.library 405472 B + netstat 43600 B (build/cmgp
on playhouse3), deployed as DH0:bsdsocket.library.green2 + netstat.green2
under the full hard rules.

**Item 1, the free-baton fast path (commit 3eb840fc).**  bsd_nx_enter()
tries `ami_netstack_try_enter_cached()` before submitting: when the realm
is idle the bracket enters exactly the way the old adopted bracket did,
and only a contended stack pays the gate's round trip.  The atom is
`tx_amiga_adopt_try_resume()` -- takeability check, resume, take-or-back-
out all under ONE Forbid(), sound because every ThreadX-state mutator in
this port runs in task context under the core Forbid() (interrupt servers
only Signal()); a decline touches nothing and the gate's parker pokes the
realm unconditionally, so no lost-wakeup window exists on either path and
the realm's Wait() stays the only idle point when the stack is busy.  The
census attributes: gs_gate_fast / nrb_GateFast / netstat "fast takes"
partitions every bracket against gated and fallback.

**Item 2, the mDNS regression root-caused and repaired (commit aef6e005).**
It was semantics, not the instrument: `_nx_mdns_yield()`'s
tx_thread_relinquish() yields only if the READY LISTS show an equal-or-
higher-priority thread.  Under the baton they did (a device reply
signalled the reader's own Exec Task, which readied its TX_THREAD
asynchronously); inside the realm the reader's signals LATCH on the realm
Task and are delivered only at the top of the realm scheduler's loop --
which cannot run while the mDNS pass holds the machine.  Every relinquish
compared against lists frozen at pass entry and no-opped for the whole
100-536 ms pass.  The repair: TX_THREAD_RELINQUISH_PORT_PREPARE (new
whitespace-default hook in the ThreadX fork, the PORT_COMPLETION pattern)
runs `_tx_amiga_relinquish_prepare()` before the decision -- from a green
context, under one Forbid(), deliver latched waiter signals and service
owed ticks.  The tick half also closes the tick merge's stated trade: the
wheel walk now waits out at most one record, not the longest pass.

**Emulator gates** (all on the exact artifact): four configs compile, host
tier "all green"; ifdhcp SLIRP PASS; bridged poolshare x3 = 2.884/2.816/
2.825 Mbit/s, lost=0, STRAY=0, census 1383/1343/1337 FAST takes with 0
gated and 0 fallbacks, holds 0 over 50 ms (the fast path recovers cycle
2's emulated -3%); run-iperf SLIRP every reachable arm + the documented
guest-as-server skip; and the NEW storm arm -- rig lore: LAN multicast
never reaches the pcap-bridged guest, and host-to-guest from the hosting
machine does not hairpin through the switch, so the storm must be UNICAST
mDNS responses fired from a third machine (mdnsstorm.py on the peer,
25 pps x 16 records, source port 5353, AA set; on-link unicast passes
_nx_mdns_packet_address_check).  471 datagrams reached the guest stack:
run completes at 2.561 Mbit/s (mDNS CPU), lost=0, STRAY=0, holds ring
0 over 50 ms.

## THE PHYSICAL REMATCH (cycle 4) -- the verdict

Control = cycle 3's fresh settle-build numbers (reused per the rules).
Two boots (18 s and 23 s returns), nine transfers, every one lost=0.

| | control (4) | cycle-3 green (6) | cycle-4 green2 (6) |
|---|---:|---:|---:|
| rate, Mbit/s | **1.128** | 1.119 (-0.8%) | 1.143/1.073/1.111/1.150/1.158/1.155 = **1.132 (+0.35%)** |
| baton leg (bracket entry) | 941 us | 1910 us | **1031 us** (warm runs 4-6: 959) |
| fetch leg | 6347 us | 8876 us | 8504 us (runs 4-6: 7645) |
| defer leg | 2469 us | 2704 us | 2702 us |
| settle leg | 3371 us | 3569 us | 3596 us |
| ack leg (wall) | 13715 us | 11198 us | 12416 us |
| post leg | 1495 us | 1117 us | 1625 us |
| drain leg | ~609 us | ~591 us | ~584 us |
| sched handoffs/frame | 0.53 | 0.00 | **0.00** (7 boot-time) |
| census per run | - | ~1300 gated | **~1460 fast / ~27 gated / 0 fallback** |
| lost / STRAY | 0 / - | 0 / 0 | 0 / 0 |

The storm run (mdnsburst on playhouse3, 0.4 s interval, during a
transfer): 0.850 Mbit/s, lost=0, and the ring added **ZERO holds over
50 ms** -- max stays the 101 ms boot entry.  The relinquish repair holds
on Gayle hardware TIGHTER than the old <=74 ms baton bound, and the stall
mechanism (which needs >200 ms of ACK dead air) is structurally gone; the
rate cost under storm is the known mDNS CPU arithmetic, finely
interleaved.  The responder answered during and after
(mdnsask: amiga-1200.local A 192.168.1.219).  Post-reboot sanity: 1.154
Mbit/s, baton 856 us.

**The five pre-registered criteria:**

- (a) baton back near control with fast-path dominance: **PASS** --
  1031 us vs 941 (was 1910), and 98% of mid-transfer brackets take the
  fast path (~1460 fast vs ~27 gated, 0 fallbacks).
- (b) handoffs/frame ~0: **PASS** -- 0.00, boot-only, both boots.
- (c) rate >= +3%: **NO** -- +0.35%, inside the +-2% band.  Per the
  pre-registration this is the rate-neutral-but-architecturally-superior
  outcome, and the close-out below is filed accordingly.
- (d) storm holds bounded <=~100 ms, no stall dips: **PASS** -- zero
  over-50 ms holds under storm on hardware; no dead-air mechanism left.
- (e) lost=0, STRAY=0 throughout: **PASS** -- every emulated and
  physical run, storm included.

**CLOSE-OUT (filed): keep the branch as the foundation; do not ship for
speed.**  The fast path did exactly what cycle 3's verdict predicted --
it removed the gate's unconditional cost (rate -0.8% -> +0.35%, baton
1910 -> 1031) while keeping every architectural win: handoffs/frame 0.00,
one idle Wait proven over four cycles, the request gate standing ready
under contention, and an mDNS bound now TIGHTER than the baton build's.
What it did not do is find new speed: the glue the realm removes is real
but the brackets' remaining costs (defer, fetch) are dominated by work
the realm cannot delete, and +0.35% does not clear a ship-for-speed bar.
Recommendation: green-realm stays the future foundation -- any lever
that needs realm-side structure (deferred el3 drain into a green thread,
tick-merge economics, sub-50 ms scheduling bounds) builds here -- but
AMINETXDUO_GREEN_REALM stays default OFF for release, and the settle
lineage remains the shipping configuration.  The machine is left on the
green2 build deliberately: it is the best-measured configuration of this
cycle (rate tie, strictly better scheduling bounds), verified live over
two boots, with the full recovery ladder parked on DH0.

## Known-unfinished list (next cycle resumes here)

1. **Lossgate rung** once the peer has tc-cap (one setcap command, above).
2. **Owner-death edges of the gate**: the heartbeat reap is built and
   exercised only by inspection; a targeted test (kill an opener mid-recv
   under the emulator) would close it.  Kernel-stop with a parked gated
   caller is unsupported-by-refcount, as it always was for adopted ones.
3. **AMINETXDUO_LOG in probe builds**: AMI_WARN/AMI_INFO compile out
   without it, which is why the net's conversions count but do not log on
   the soak rig.  Consider forcing AMINETXDUO_LOG on for RXPROBE builds so
   a stray's warning is seen the day the counter moves.
4. **WaitIO/WaitPort are outside the net**: the tripwire covers Wait()
   only.  The converted sites use CheckIO loops, so nothing known blocks,
   but the net cannot prove that class the way it proves Wait().

## How to build and run it

    cmake -B build/cmgreen -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
          -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_RXPROBE=ON -DAMINETXDUO_LTO=OFF \
          -DAMINETXDUO_GREEN_REALM=ON
    tests/tools/run-ifdhcp.sh -b build/cmgreen                      # SLIRP
    tests/perf/run-poolshare.sh -b build/cmgreen -B ens18 -P <peer> -d 16 -s 15
    # then read "receive budget:" + the "green:" census in the guest capture;
    # gs_stray_wait / "STRAY" must be zero on every run, "fell back" should be
    # zero and "gated brackets" in the hundreds; "N of 16 realm signal bits
    # out" is the audit's live figure (5 on one interface).
