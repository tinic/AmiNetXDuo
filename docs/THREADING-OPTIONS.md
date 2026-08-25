# Design memo: single-thread execution models for AmiNetXDuo

2026-08-24, branch `event-budget` (@2ab83e15), repo /home/turo/anxd-pollfix.
Read-only design study.  All numbers are the measured campaign state
(docs/RECEIVE_BUDGET.md, docs/PHYSICAL_RX_A1200.md, memory
project_anxd_transfer_rate) on the physical A1200 (14 MHz 68EC020, 2 MB chip,
3c589) at ~1.15 Mbit/s TCP receive, 94% CPU busy.

## 0. The frame every option must be judged in

The machine is 94% busy.  RECEIVE_BUDGET.md's own verdict stands: **only
removed WORK counts; moving the same instructions to another thread moves the
wait, not the rate.**  So each option is priced by the CPU work its handoffs
cost today, not by the elapsed legs (settle 2.9 ms and fetch 7.3 ms are
wall-clock spans that OVERLAP real processing; they are not 10 ms of
removable idle).

Per-frame arithmetic at 1.15 Mbit/s: 1.15e6/8/1460 ≈ 98 data frames/s → ~10.2
ms/frame, of which ~9.6 ms is work (94% busy).  ACKs ~74/s ≈ 0.75/frame.
Named per-frame work:

| item | ms/frame | source |
|---|---:|---|
| ISR fused FIFO copy | ~1.3 | flight-4 / profile (12.8% busy) |
| drain leg (reader dequeue → deferred receive) | 0.62 | budget |
| settle CPU (NetX IP+TCP processing per segment) | ~0.9 measured | settle dissection 2026-08-25 (demux 0.45 + state 0.47); the leg's other ~2.5 ms is defer, scheduling overlap not work |
| recv copy-out (`n68k_copy_bytes_mv20`) | ~0.7 | profile (6.6% busy) |
| TX true CPU (reap 0.23 + stuff 0.29 + post floor ~1.0 per ACK) | ~1.1 | push dissection ×0.75 ACK/frame |
| TX exec glue (Signal→deferred→IP round trip nested in BeginIO) | ~0.5 | ~0.7 ms/ACK ×0.75 |
| scheduler/baton transitions | ~0.4 | 841/10 s measured ≈ 4% CPU |
| NetX tail + app + system ambient | ~3.8 | remainder |

**The whole federation (thread handoffs, baton passes, event flags, exec
glue) is worth ≈ 0.9–1.5 ms of the 9.6 ms frame — 10–15%, not 2×.**  The
AmiTCP_NG 906-vs-412 KB/s comparison is a DIFFERENT machine and driver
(X-Surf-100); PHYSICAL_RX_A1200.md explicitly forbids conflating it with this
profile.  On this machine the dominant costs are the two required copies
(19.4% busy), the PIO ISR, NetX per-segment processing, and the TX path.  No
threading surgery recovers those.  That bounds everything below.

## 1. What the tree actually does today (evidence)

**Priorities** (src/thread_priorities.h): SANA-II readers TX-pri 1, IP thread
2, adopted callers 16.  All ThreadX threads are hosted 1:1 on Exec Tasks at
Exec pri 1 (`TX_AMIGA_TASK_PRIORITY`, port/threadx-amiga/inc/tx_port.h:499);
ThreadX priority is arbitrated by the baton, not by Exec.

**The baton port**: a thread runs iff it is `_tx_thread_current_ptr`
(port/threadx-amiga/src/tx_thread_schedule.c:44-104).  There are no register
context switches at all — `tx_thread_stack_build.c:17` "There is no stack
frame to build in a hosted port"; each ThreadX thread is an Exec Task parked
in `Wait()` on a run signal (`_tx_amiga_thread_park`,
tx_thread_system_return.c:153).  Every ThreadX handoff is therefore an Exec
Signal + Wait + Exec dispatch; `_tx_amiga_dispatch_inline()`
(tx_amiga_internal.h:156-183) already collapsed the 3-switch version to 1
Exec switch per handoff.  "Interrupt" context = Forbid() + system_state++
(tx_thread_context_save.c); tick = VERTB int server
(tx_initialize_low_level.c:1066) waking a pri-20 tick Task;
`TX_TIMER_PROCESS_IN_ISR` is defined (tx_port.h:128) — timer expiration runs
in the tick Task's context_save bracket, not on a timer thread.

**RX path**: el3 ISR drains the FIFO into pre-posted CMD_READ buffers
(single-copy Phase A) and ReplyMsg→Signal wakes the reader Task.  The reader
loop (src/sana2/sana2_rx.c:1144-1194) releases the baton around `Wait()`
(`ami_sana2_block_enter/leave` → ami_netstack_baton_release/acquire,
src/netstack/netstack_baton.c:294-454), then drains replies holding the
baton.  Delivery (sana2_rx.c:413-548) calls
`_nx_ip_packet_deferred_receive(iface->ip, packet)` (:523), which queues on
`nx_ip_deferred_received_packet_head` and sets `NX_IP_RECEIVE_EVENT`
(third_party/netxduo/common/src/nx_ip_packet_deferred_receive.c:80-105).  The
IP thread (nx_ip_thread_entry.c:232-336) wakes on the event flag, takes
`nx_ip_protection`, and runs `_nx_ip_packet_receive()` per packet, which for
TCP calls `_nx_tcp_packet_process` DIRECTLY because it is the IP thread
(nx_tcp_packet_receive.c:142-148).

**recv path**: every libary vector brackets with `bsd_nx_enter()`
(src/bsdsocket/netx_call.c:103-144 → `ami_netstack_enter_cached`,
netstack.c:276; adoption via `tx_amiga_adopt_thread/resume`,
port tx_amiga_adopt.c:213/400).  Bracket price measured ~270 µs cached / ~790
µs cold (transfer.c:1184-1186).  `bsd_recv_tcp` (transfer.c:1210) calls
`nx_tcp_socket_receive` through `bsd_wait_sliced` (select.c:388-452, 10-tick
slices).  During streaming the caller is usually SUSPENDED on the socket's
receive suspension list (nx_tcp_socket_receive.c:244-267), and NetX already
does direct suspension handoff: `nx_tcp_socket_state_data_check.c:1114-1170`
dequeues the packet, writes it into the suspended thread's
`tx_thread_additional_suspend_info` return slot, restores window credit, and
resumes the thread — all on the IP thread.  The notify callback fires at
:1207-1213.  The fetch leg (7.3–8.2 ms) is the time between that resume and
the pri-16 caller actually running + dequeuing: it queues behind the reader
and IP thread for the rest of the burst, by design (thread_priorities.h).

## 2. Option 1 — full single-thread (everything cooperative on one thread)

What it means: no IP thread, no reader threads; one loop polls the SANA-II
reply ports, runs IP/TCP processing inline, runs timers, and services BSD
calls as continuations.

What breaks in NetX Duo:

- `nx_ip_create` unconditionally creates the IP helper thread
  (nx_ip_create.c:223, `NX_THREAD_EXTENSION_PTR_SET(&ip_ptr->nx_ip_thread ...)`)
  and large parts of the core test identity against it:
  nx_tcp_packet_receive.c:102, nx_icmp_packet_receive.c:151,
  nx_igmp_packet_receive.c:103 (defer vs process-inline),
  nx_tcp_socket_receive.c:244, nx_tcp_server_socket_accept.c:184,
  nx_tcp_socket_send_internal.c:1342, nxd_tcp_client_socket_connect.c:601,
  nx_tcp_socket_disconnect.c:305 (refuse to suspend ON the IP thread).  A
  true single thread IS the IP thread, so every blocking socket call returns
  NX_NO_PACKET instead of suspending — the entire blocking BSD API
  (recv/send/connect/accept/select, the whole `bsd_nx_enter` surface: 60+
  sites across transfer.c, socket.c:15, options.c:16, interfaces.c, etc.)
  must be rewritten as an event loop with continuations or as
  poll-with-backoff.  That reintroduces exactly the EAGAIN-from-blocking-recv
  defect class select.c:322-355 documents against AmiTCP/Roadshow.
- `tx_event_flags_get(..., TX_WAIT_FOREVER)` in nx_ip_thread_entry.c:239 —
  the loop's idle wait — must be replaced by a poll/wait on the union of Exec
  signals (SANA-II ports, VBL, client requests).
- Timers: TX_TIMER_PROCESS_IN_ISR already runs expiration in the tick
  bracket; that survives, but every timer callback that assumes it does not
  run under application call state gains new reentrancy.
- DHCPv6 needs its threads (thread_priorities.h:50-69 documents a real
  deadlock: `_nx_dhcpv6_request()` sleeps until the client thread is idle —
  on one thread that never happens without a full CPS rewrite of the vendored
  client).

Scope, honestly: this is not a port change, it is a different product.  It
touches the vendored NetX core (or shims every identity check), the entire
bsdsocket blocking surface, netstack bring-up, DHCP/mDNS/AutoIP hosting.
Estimate: 3–6 months, high regression risk across 97 host tests + both
emulated boards + the physical rig, for a measured upside of ~+10–15%
(≈1.25–1.35 Mbit/s).  Not recommended.

## 3. Option 2 — caller-context work stealing (drain the IP queue before suspending)

Mechanics investigated:

- Queue: `nx_ip_deferred_received_packet_head`, appended under TX_DISABLE,
  `NX_IP_RECEIVE_EVENT` set (nx_ip_packet_deferred_receive.c:80-105).  The IP
  thread's drain loop (nx_ip_thread_entry.c:297-336) is queue-plus-mutex, not
  thread-identity magic: a caller holding `nx_ip_protection` could run the
  identical dequeue + `_nx_ip_packet_receive()` body.
- BUT thread identity bites one layer down: `_nx_tcp_packet_receive`
  (nx_tcp_packet_receive.c:102) sees the caller is not `&ip_ptr->nx_ip_thread`
  and RE-QUEUES the segment onto `nx_ip_tcp_queue_head` + sets
  `NX_IP_TCP_EVENT` — a stolen drain that stops there produces a SECOND
  handoff, strictly worse.  The steal must therefore also invoke
  `(ip_ptr->nx_ip_tcp_queue_process)(ip_ptr)` (same body the IP thread runs
  at nx_ip_thread_entry.c:339-351) after draining the deferred queue.  ICMP
  and IGMP (:151/:103) defer the same way; leaving their events for the real
  IP thread is benign (it wakes, finds the queue empty, loops).
- Protection: everything must run under `nx_ip_protection`
  (nx_ip_thread_entry.c:242).  Note the mutex is created TX_NO_INHERIT
  (nx_ip_create.c:203); in the baton model that is survivable (priorities act
  only at ThreadX service boundaries, tx_thread_context_restore.c comment),
  but every `tx_mutex_put`/`tx_event_flags_set` inside stolen processing is a
  preemption point where the pri-1 reader or pri-2 IP thread takes the baton
  back — the steal will be interleaved, and the pri-16 holder of
  nx_ip_protection then blocks the pri-2 IP thread on the mutex with no
  inheritance: the classic inversion, bounded only by the cooperative model.
- Reentrancy: two adopted callers cannot literally run concurrently (one
  baton), and the queue surgery is under TX_DISABLE + mutex, so correctness
  is achievable; the danger is the callback set now running in caller
  context: `bsd_tcp_receive_notify` etc. (select.c:304-318) Signal the very
  task that is executing them — harmless-looking, but the settle/fetch budget
  probes and the notify ordering assumptions (budget.c single-stamp) break,
  and `bsd_recv_parked`'s bracket-avoidance predicate (transfer.c:1188)
  assumes NetX state is only mutated by the IP thread while the caller is
  outside the bracket.
- Hook point exists cleanly: in `bsd_recv_tcp` (transfer.c:1240-1256), before
  `bsd_wait_sliced` with a blocking wait, the bracket is already held
  (`bsd_nx_need`, :1245) — a port/netstack call `ami_ns_ip_drain()` could run
  there, then retry `nx_tcp_socket_receive(NX_NO_WAIT)`.

Honest win: during streaming the caller is usually already suspended inside
`nx_tcp_socket_receive` when frames arrive, so the steal window (recv entry
with data in flight but not yet processed) is narrow.  What it removes is at
most one dispatch round trip per recv() call (~29 calls/s at 4 KB reads) and
occasionally an IP-thread wake: ≤0.1–0.3 ms/frame → **≤ +1–3%**, for real
reentrancy and inversion risk inside vendored code paths.  Weeks of careful
work, worst win/risk ratio of the four.  Not recommended.

## 4. Option 3 — IP-thread direct completion of a pending recv

NetX already does half of this: data_check's suspension handoff
(nx_tcp_socket_state_data_check.c:1114-1170) gives the suspended receiver the
packet without touching the notify→fetch path.  What it does NOT do is the
COPY: the caller still has to wake (fetch leg), copy out
(`n68k_copy_bytes_mv20`), release the packet, and re-suspend — per packet
returned, with per-iteration mutex get/put.

The extension: a pending-receive descriptor on AmiSocket (`as_RxDirect`: user
buffer ptr/len/filled, break-out conditions).  `bsd_recv_tcp` publishes it
(under the bracket), then Wait()s on a plain Exec signal.  The receive notify
callback — which already runs on the IP thread at data_check:1212, mutex held
— loops `nx_tcp_socket_receive(..., NX_NO_WAIT)` (legal on the IP thread; the
:244 guard just prevents suspension), copies into the user buffer, releases
packets, and Signal()s the caller once when the request is satisfied
(len reached / would-block-after-data, matching bsd_recv_tcp's current
loop-exit rules at transfer.c:1221-1310).  Window credit and the SWS
window-update ACK then happen at dequeue time on the IP thread
(nx_tcp_socket_receive.c:198-231) — window reopens at settle time instead of
fetch time, a latency bonus that costs nothing.

Everything lives in src/bsdsocket (select.c notify + transfer.c recv path);
no vendored-code edits, no new NetX entry points.  Constraints: the callback
must never suspend (select.c:340-345 rule — copy + Signal comply); MSG_PEEK /
MSG_WAITALL / OOB and the RxPending interplay must be excluded or handled
(fall back to today's path for anything but the plain streaming read);
`bsd_recv_parked` and the budget probes need adjusting.

Removed work: per-recv wake glue (adopt resume + park + baton reacquire
~0.3–0.5 ms per recv), per-packet mutex/bracket churn in the copy loop, and
the caller's re-suspend cycle — ~0.2–0.5 ms/frame at 29 recv/s, more if the
app reads small.  **Expected +2–5%** (~1.18–1.21 Mbit/s), better ACK timing,
and it makes throughput much less sensitive to application read size (the
currently-unmeasured lever 1).  2–4 weeks including gating.  Best near-term
ratio.

## 5. Option 4 — native ThreadX green threads inside one Exec task

> STATUS 2026-08-25: PROTOTYPED on branch `green-realm` behind
> `-DAMINETXDUO_GREEN_REALM` (default OFF).  The core below is real -- m68k
> stack switching, the realm's single idle Wait(), the converted reader/DoIO
> sites, a stray-Wait assert net -- and the emulator gate ladder is green.
> Adopted callers keep the baton (the request-gate boundary is NOT built).
> docs/GREEN-REALM.md is the state document; this section remains the design
> brief it was built from.

Proposal: real m68k context switching in the port, all stack-internal threads
cooperating inside ONE Exec Task ("the realm"), Exec primitives only at the
client boundary.

What the port replaces (all in port/threadx-amiga/src/):

- `tx_thread_stack_build.c` builds no frames today (:17) and instead creates
  an Exec Task per thread (:92-99).  Green version: build a genuine 68k
  initial frame on the ThreadX-owned stack (tx_thread_create already supplies
  stack_start/size) — entry PC, saved d2-d7/a2-a6 slots, ~40 lines of layout,
  the shape every bare-metal ThreadX port uses.  There is no reference 68k
  port in the tree, but n68k_iocopy.S / cpucal.S show the toolchain's asm
  conventions; a movem.l-based `_tx_thread_context_switch` is ~60-100
  instructions total.
- `tx_thread_schedule.c` stops being a baton arbiter: the realm task's main
  loop becomes: pick `_tx_thread_execute_ptr`, stack-switch into it; when all
  ThreadX threads are suspended, fall into the realm idle wait —
  `Wait(union of registered Exec sigmasks)` — and on wake, convert signals to
  ThreadX events and re-dispatch.  `_tx_thread_system_return` becomes a real
  switch back to the scheduler stack instead of `_tx_amiga_thread_park`'s
  Signal/Wait.
- `tx_thread_context_save/restore` keep their Forbid()+system_state shape for
  the VERTB tick and the device ISR boundary — that bracket already models
  interrupts correctly and is untouched conceptually.
- The baton machinery (netstack_baton.c, `ami_sana2_block_enter/leave`)
  dissolves INSIDE the realm: a reader that today releases the baton around
  `Wait(rx->wake_mask)` (sana2_rx.c:1165-1183) instead registers its sigmask
  with the realm and suspends its green thread on a ThreadX event; the realm
  idle loop owns the one real Wait().  Same for `DoIO()` at sana2_rx.c:938
  (must become SendIO + realm-collected reply) and the TX reap signal.

ISR path: unchanged at the device end — el3 drains in the ISR into pre-posted
buffers and ReplyMsg/Signal.  The Signal now lands on the realm task; the
realm (Exec pri 1, same as today's reader task) wakes, and its dispatcher
resumes the highest-priority green thread = the reader at TX-pri 1.  The
frame-drop guarantee (thread_priorities.h:27-33) is actually enforced by the
ISR + pre-posted CMD_READs plus the reader's Exec pri-1 wake; a single realm
task at Exec pri 1 preserves it exactly, PROVIDED the realm never lets a
low-TX-priority green thread run unpreempted past a reader wake — which the
stock ThreadX preemption check at every service boundary gives us, same
granularity as today (context_restore comment: priorities honoured at API
boundaries).  The tick merges: the VERTB server can Signal the realm instead
of the pri-20 tick Task, and the realm runs `_tx_timer_interrupt()` inside
the existing context_save bracket; TX_TIMER_PROCESS_IN_ISR stays true.

Adopted callers: a foreign Exec Task cannot be a green thread (its stack and
dispatch belong to Exec).  The boundary becomes a request gate: `bsd_nx_enter`
turns into "enqueue closure on the realm's request port, Signal realm, Wait
for completion Signal"; the realm runs the vector body on a per-caller green
proxy thread (which may suspend in NetX freely — it IS a ThreadX thread), and
copies results into caller memory before the completion Signal.  This is
option 3's direct-completion generalized to every vector: recv becomes
publish-buffer → one boundary round trip.  The `bsd_nx_enter` surface is ~60
call sites but ONE choke point (netx_call.c:103) — the vector bodies
themselves run unchanged on the proxy, so the rewrite is the gate + proxy
pool + result marshalling, not 60 rewrites.  The THREADS_ONLY identity
checks are all satisfied (proxies are real ThreadX threads, never the IP
thread).

Forbid()/preemption story: today Forbid() is the core lock and Exec pri 1
makes stack tasks preempt pri-0 apps.  One realm task at pri 1 keeps that
external story identical; internally, Forbid/Permit pairs per handoff vanish
(a green switch needs no Exec arbitration), which is part of the measured
transition cost.  Debuggability improves: one task, one stack-per-thread but
one crash context; the health-mark/zombie/reap machinery
(tx_thread_schedule.c:107-378) largely disappears rather than gaining cases.

Wins, same arithmetic: removes ALL internal Exec round trips — scheduler
transitions ~0.4 ms/frame, TX-completion exec glue ~0.5 ms/frame, the
Forbid-heavy dispatch glue inside drain/settle (~0.1–0.3) — and keeps one
boundary Signal pair per recv (~0.4 ms × 29/s ≈ 0.12 ms/frame, already paid
today as the fetch wake).  Net removable ≈ 0.9–1.2 ms/frame → **+10–13%**
(~1.27–1.31 Mbit/s), with a structural bonus: every future lever (lazy TX
reap, tick-batched ACK collection, reader inlining) becomes a function call
instead of a cross-task protocol.

Cost/risk: port-layer rewrite, NetX/ThreadX cores untouched (better than
option 1), no vendored-internals reentrancy (better than option 2).  New
failure class: a green thread that blocks in Exec directly (any stray
DoIO/Wait inside stack code) now hangs the whole realm — every such site must
be found and converted (sana2_rx.c:938/:1166, sana2_tx reap, netstack DHCP
timer waits, resolver...).  Estimate 2–3 months including a long emulator
soak; the m68k switch asm itself is small, the Exec-wait inventory is the
real work.

## 6. Expected wins, side by side

| option | work removed (ms/frame of 9.6) | rate at 14 MHz | scope | risk |
|---|---:|---:|---|---|
| 1 full single-thread | ~1.0 | ~1.29 Mbit/s (+12%) | 3–6 months, NetX+BSD rewrite | very high |
| 2 caller work-steal | ≤0.3 | ≤1.19 (+1–3%) | 2–3 weeks | high (inversion, vendored paths) |
| 3 IP-thread direct completion | 0.2–0.5 | 1.18–1.21 (+2–5%) | 2–4 weeks, src/bsdsocket only | low-medium |
| 4 green-thread port | 0.9–1.2 | 1.27–1.31 (+10–13%) | 2–3 months, port only | medium (realm-block hangs) |

None closes the X-Surf 906-vs-412 gap; that comparison needs its own profile
on that hardware before any threading conclusion is drawn from it.

## 7. Recommendation

Rank: **4 > 3 > 1 > 2** on strategy; **3 first** on sequencing.

1. Prototype option 3 now (touch: src/bsdsocket/transfer.c `bsd_recv_tcp`,
   src/bsdsocket/select.c notify set, AmiSocket in bsdsocket_internal.h).
   It is the largest win available without leaving src/bsdsocket, it
   de-risks option 4's boundary design (the request-gate IS option 3's
   contract), and it neutralizes the recv-size sensitivity.
   Fold in the already-recommended lazy TX-completion cut (sana2_tx.c defer
   gating) in the same campaign — independent +4–5%.
2. Green-light option 4 as the successor only if, after 3 + the TX cut, the
   budget still shows ≥0.8 ms/frame in transitions+glue (re-measure with the
   existing budget instrument before committing months).
3. Do not build options 1 or 2.

Gating plan (behavior change ⇒ full harness): emulator first on both boards
(`tools/emu-net-run.sh` A2065 + ne2000_pcmcia), then
`tests/perf/run-poolshare.sh` (budget capture), `tests/tools/run-ifdhcp.sh`,
`tests/tools/run-iperf.sh`; host tests (97) must stay green; physical deploy
non-LTO only, artifact-size verified, recovery libraries staged
(DH0:bsdsocket.library.0252, SYS:rx-current-prof.library).

Instrument for failure modes: fetch/settle legs (existing NETSTATUS_RXBUDGET)
must SHRINK, not move; peer-side `ss -ti` retransmissions and zero-window
counts (regression = notify/window ordering broken); `bs_StateShared/StateMax`
(netstack_baton.c:279) for system-state leaks; rx overruns / device drops
(readers starved); ACK cadence via ackscope (delayed window updates); for
option 3 specifically: a counter for direct-completions vs fallback path, and
an assert that the notify callback never observes itself suspended.

## Critical files for implementation

- /home/turo/anxd-pollfix/src/bsdsocket/transfer.c (bsd_recv_tcp, the completion consumer)
- /home/turo/anxd-pollfix/src/bsdsocket/select.c (notify callbacks that become the completer)
- /home/turo/anxd-pollfix/third_party/netxduo/common/src/nx_tcp_socket_state_data_check.c (the suspension-handoff model being extended)
- /home/turo/anxd-pollfix/port/threadx-amiga/src/tx_thread_schedule.c (+ tx_amiga_internal.h) (what option 4 replaces)
- /home/turo/anxd-pollfix/src/sana2/sana2_rx.c (reader loop, the Exec-wait inventory for option 4)
