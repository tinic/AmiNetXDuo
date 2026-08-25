# The green realm: option 4's prototype, and where it stands

2026-08-25, branch `green-realm` (off `event-budget`), repo
/home/turo/anxd-pollfix.  The design brief is docs/THREADING-OPTIONS.md
section 5; the glue inventory it removes is docs/RECEIVE_BUDGET.md's
(~0.9-1.2 ms/frame of Exec Signal/Wait/dispatch round trips at ~1.15 Mbit/s
on the physical A1200).  EMULATOR-ONLY so far: nothing on this branch has
touched the physical machine, and nothing may until the endurance story is
much longer than one evening's.

## What is built and real

`-DAMINETXDUO_GREEN_REALM=ON` (default OFF; OFF builds the baton port byte
for byte -- all four local configurations compile, green and baton, probe
and plain).

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
- **ISR/tick bracket unchanged.**  context_save/context_restore keep their
  Forbid()+system_state shape; the tick stays on its pri-20 Task;
  TX_TIMER_PROCESS_IN_ISR stays true.  From any foreign context
  (tick, adopted callers) `_tx_amiga_dispatch_inline()` DECLINES a green
  target and pokes the realm, so only the realm ever performs a switch.
- **Converted Exec-blocking sites** (the design's failure mode is a green
  thread blocking in Exec -- it would sleep the whole realm):
  - the SANA-II reader loop's release-Wait-reacquire bracket ->
    `tx_amiga_green_wait(wake|reap)` (sana2_rx.c);
  - synchronous device commands -> `ami_sana2_do_io()`, which is
    DoIO-in-the-bracket from ordinary contexts and SendIO + green wait from
    a green one (sana2_device.c; covers NX_LINK_ENABLE on the IP thread and
    the teardown CMD_FLUSH on the reader).
- **The stray-Wait net (probe builds).**  GREEN_REALM+RXPROBE routes every
  `Wait()` in the netstack/SANA-II sources through `ami_green_checked_wait`:
  ordinary contexts pass straight through; a green-context Wait() is
  converted to a green wait, warned about, and counted in `gs_stray_wait`,
  which MUST read zero.  The baton bracket refuses a green thread the same
  loudly-counted way.
- **Census.**  Six counters appended to NetStatusRxBudget and printed by
  netstat ("green:" lines): switches, external handoffs, idle waits, waits
  latched/slept, strays.  Zero from a baton build.

## Adopted callers: what this cycle built (the honest interim)

The memo's full boundary -- marshalling every bsdsocket vector through the
netx_call.c choke point onto per-caller green proxy threads, one Signal
back -- is NOT built.  What is built is coexistence: adopted application
Tasks keep their own Exec Task and the baton protocol unchanged.  The realm
hands them the baton by Signal exactly as the old scheduler Task did, and
takes it back at their next yield; their fast path (adopt_resume taking a
free baton without waking anyone) still applies and is what they nearly
always use (the emulated census shows 4 external handoffs against 1384
green switches in a 15 s receive).  So the glue REMOVED is the
stack-internal federation (reader <-> IP <-> mDNS <-> timers), which is
where the budget located it; the per-recv adopt/park glue REMAINS and is
next cycle's item.

## Gate ladder, all emulator (Amiberry on playhouse3, A2065 guest)

| rung | what | result |
|---|---|---|
| a | four local configs compile (green/baton x probe/plain) | PASS |
| b | host tier, tools/ci.sh host (97 tests + tree checks) | PASS ("all green"; two pre-existing breaks on event-budget mended en route: run-poolshare's missing HARNESSES row, test_neighbour's ami_eclock_rate link) |
| c | run-ifdhcp SLIRP | PASS, RC=0, 35 ok -- the campaign's standard result.  (A bridged run also completes with the documented 10.0.2.x-literal failures only.) |
| d | run-poolshare bridged | PASS, RC=0: 2.76 Mbit/s, lost=0, zero_windows=0, pool 142-fewest of 205, budget block sane, green census live, 0 STRAY |
| e | run-iperf SLIRP | rc=77 with every reachable arm passing + the guest-as-server skip -- identical to the branch-point's documented result |
| f | endurance, 3 consecutive poolshare runs | PASS: RC=0 all three, 2.72-2.78 Mbit/s, lost=0, zero_windows=0 throughout |

## Emulated numbers (directional ONLY -- the emulator charges per
## instruction and batches interrupts; the verdict lives on the A1200)

Same tree, same rig, back to back: cmbase (RXPROBE, baton) three runs vs
cmgreen (RXPROBE+GREEN) four runs, run-poolshare bridged -d 16 -s 15,
per-run means averaged:

| | baton (3 runs) | green (4 runs) | delta |
|---|---:|---:|---:|
| rate, kbit/s | 2687 (2729/2667/2667) | 2774 (2755/2772/2728/2840) | +3.2% |
| fetch leg mean | 4122 us | 3190 us | -23% |
| defer leg mean | 2310 us | 2272 us | -1.6% |
| settle leg mean | 2870 us | 2824 us | -1.6% |
| drain leg mean | 503 us | 504 us | 0 |
| baton leg (adopted bsd_nx_enter, both arms) | 221 us | 221 us | 0 |
| green switches / external / idle waits per run | 0 | ~1400 / ~5 / ~765 | |
| stray Exec Waits from green context | n/a | 0 in every run | |

Reading it honestly: the emulated win shows up where the adopted caller
queues behind the stack's internal ladder (fetch, -23%) and in the rate
(+3.2%); defer barely moves HERE because Amiberry delivers interrupts in
batches and the emulated Exec round trip was already cheap.  On the
physical machine defer is 2.5 ms and 74% of settle, and the transition
glue it contains is exactly what the green switch removes -- that is the
number the physical A/B must watch.  ~1400 switches per 15 s receive is
~93/s of handoffs that used to be Exec round trips; only ~5 realm-level
external handoffs per run confirms adopted callers nearly always ride
their existing fast path.  Every delta above is directional, not the
verdict.

## Known-unfinished list (next cycle resumes here)

1. **The adopted-caller boundary** (memo's request gate): bsd_nx_enter ->
   request queue -> green proxy pool -> one completion Signal.  Removes the
   per-recv adopt/park glue and the fetch leg's re-entry cost; option 3's
   direct-complete machinery (flag exists, default OFF) is the design to
   generalize.  Until then adopted callers still hold the baton while the
   realm idles.
2. **The tick merge.**  The VERTB server still wakes the pri-20 tick Task;
   the memo's end state has it Signal the realm and run
   `_tx_timer_interrupt()` inside the realm's own bracket, removing the
   tick Task and its per-tick Exec switch.  Untouched this cycle -- the
   bracket semantics are identical either way.
3. **Signal-bit budget.**  All green threads' MsgPorts now allocate out of
   the realm Task's 32 signal bits (measured usage: scheduler 1 + 2-3 per
   interface + reap + timer ports; fine for one or two interfaces, worth an
   audit before more).
4. **green_wait edge cases:** a thread resumed by something other than
   signal delivery returns 0 from tx_amiga_green_wait (callers loop
   safely); waiter-table overflow busy-yields; a green thread deleted while
   registered is purged via _tx_green_forget -- none observed, all lightly
   exercised only.
5. **Longer soak.**  One evening of emulator gates is not an endurance
   story.  Before any physical thought: repeated cards/cards6 tiers, the
   lossgate rig, a multi-hour soak, and the FREEZE-DIAGNOSTIC.md checklist
   against a deliberately-injected stray Wait.
6. **Physical A/B** (a later cycle, explicitly out of scope here):
   non-LTO probe build, artifact-size verified, recovery ladder staged;
   judge by the budget legs (defer must SHRINK, not move), transitions/s,
   and rate over >=4 warm runs per arm.

## How to build and run it

    cmake -B build/cmgreen -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
          -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_RXPROBE=ON -DAMINETXDUO_LTO=OFF \
          -DAMINETXDUO_GREEN_REALM=ON
    tests/tools/run-ifdhcp.sh -b build/cmgreen                      # SLIRP
    tests/perf/run-poolshare.sh -b build/cmgreen -B ens18 -P <peer> -d 16 -s 15
    # then read "receive budget:" + the "green:" census in the guest capture;
    # gs_stray_wait / "STRAY" must be zero on every run.
