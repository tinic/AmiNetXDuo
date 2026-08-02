# Backlog

What is outstanding, what was decided against, and why. Findings come mostly
from the NDK 3.2 autodoc audit (four passes over all 122 documented entries)
and from the memory-floor and stress work.

The autodoc is at `NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc`. **`grep`
silently fails on it** — `file` misidentifies it as "GTA in-game text". Read it
with python.

**The NDK headers have the same trap.** `m68k-amigaos/ndk-include` is Latin-1
and carries a `©`, so a plain `grep -r` reads those files as binary and finds
nothing — an empty result there means "not read", not "not present". Use
`LC_ALL=C grep -a`. A whole RFC 3542 assessment was once written on one of those
empty results.

---

## Open — no decision taken

- **What Roadshow and AmiTCP_NG actually do in their SANA-II copy hooks**,
  measured 2026-08-01 with `tests/tapprobe/` (an instrumented copy of
  tcpdrill's device that installs itself, launches a foreign stack against it
  and records an event ring). Three leads came out of it.

  **Neither stack folds the checksum into the copy.** Roadshow was measured two
  ways: scribbling `0xEE` over the source buffer the instant the hook returned
  left every reply intact at all four alignments, so it reads the source
  exactly once; and at its best alignment its hook costs 133 ns/B against 158
  for a plain `movem.l` copy of the same data on the same machine, which leaves
  no budget for per-longword arithmetic. AmiTCP_NG is settled from its GPL
  source: `m_copy_to_mbuf` is a plain `bcopy`, `#define`d to `CopyMem`, and its
  own comment says the MOVE16 fast path is deliberately not used on the SANA
  path. So a melded copy-and-sum would be novel here, not catching up -- the
  claim that the other stacks already do it does not survive contact.

  **Roadshow asks for aligned buffers through an extension we do not have.**
  Its buffer-management list has four entries: `S2_CopyToBuff`,
  `S2_CopyFromBuff`, and `S2_DMACopyToBuff32` / `S2_DMACopyFromBuff32`
  (`S2_Dummy+8` and `+9`, absent from `src/sana2/sana2_device.h`). Asking the
  DMA hook for a buffer returned one at 0 mod 4 and a frame delivered through
  it arrived intact. This is how a stack GETS the alignment we measured we do
  not have, and for a DMA-capable card it removes the copy rather than
  optimising it. Worth finding out how many real drivers use it before
  building anything.

  **Roadshow is faster than us at the alignment real drivers hand over.**
  Per-byte `S2_CopyToBuff` cost, two-point fit over 64 and 1024-byte payloads
  so no fixed per-call cost is in the number:

  | src align | Roadshow | ours (`n68k_copy_bytes`) |
  |---|---|---|
  | 0 mod 4 | 133 | 158 |
  | 1 mod 4 | 715 | 205 |
  | 2 mod 4 | **182** | **204** |
  | 3 mod 4 | 716 | 204 |

  We are flat where it falls off a 5.4x cliff, but it beats us by 11% at the
  2 mod 4 that 8 of 9 real drivers deliver. That is a target needing no new
  protocol machinery.

  Also measured: Roadshow keeps 36 reads outstanding (32 IPv4 + 4 ARP, matching
  its documented `iprequests`/`arprequests` defaults) against our
  `AMI_SANA2_RX_MAX_DEPTH` 32, and reposts before the answer goes out on the
  same IORequest, 7.5-8.3 ms after the reply against our ~1 ms.

  AmiTCP_NG could not be run against the synthetic device: it opens it, does
  `S2_DEVICEQUERY` and `S2_GETSTATIONADDRESS`, then `AddNetInterface` fails
  with errno 43 `EPROTONOSUPPORT` and it never posts a `CMD_READ`. That is
  `socreate()` finding no `protosw`, inside its own startup, on both A1200 and
  8 MB A3000 profiles. Not our bug and not worth debugging further.

- **The tcpdrill device starts offline, which hangs any stack that does not
  send `S2_ONLINE`**, found 2026-08-01. Roadshow stops after
  `S2_ADDMULTICASTADDRESS`, arms an `S2_ONEVENT` and waits, so its interface
  never comes up and it hangs silently at bring-up. Real Ethernet drivers are
  live once configured; ours is not. Invisible to us because we send
  `S2_ONLINE` ourselves, so this only bites when tcpdrill is pointed at another
  stack -- which is exactly what makes it worth fixing before the next probe.

- **Receive is the slow direction, on every machine measured**, 2026-08-01.
  bifat benchmarked four stacks on four machines, `timecmd copy` each way
  against a mounted fileserver. We are first or second on send in all four and
  third or fourth on receive in all four: A3000/060 + X-Surf-100 699 KB/s
  against AmiTCP 4.6's 1103 (0.15.0); A500/1230-50 + X-Surf-500 389 against
  569; A1200/060 + CNet16 400 against 454; A500 68000/14MHz + X-Surf-500 115
  against 175. Send, same machines: 1044 (first), 346 (first), 580 (second),
  142 (first). AmiTCP 4.6 is the exact mirror -- best receive, worst send,
  everywhere -- so both stacks sit at opposite ends of one tradeoff rather than
  one being slow.

  The deficit grows with link speed. Worst on X-Surf-100 behind an 060 (-37%),
  smallest on CNet16 where the card dominates and the stacks converge. A gap
  that widens with packet rate is a per-packet cost, not a bandwidth-
  proportional one.

  Three sizing constants were the obvious suspects and all three are already
  large enough on these machines -- checked by reading, not measured:

  - The advertised window is not binding. `ami_bsd_tcp_window()` gives a lone
    socket the `BSD_TCP_WINDOW_CEILING` 32768. Window limits throughput at
    window/RTT, so 32 KB would have to meet a 47 ms RTT to cap 700 KB/s, and a
    LAN is under 1 ms. Window is what limits the loopback and long-path cases
    in `tests/trace/`; it is not what limits these.
  - The packet pool is not it. `AMI_POOL_MAX_PACKETS` caps it at 256 whatever
    `AvailMem()` says, so every machine here above roughly 8 MB gets the same
    pool as every other.
  - Nor is SANA-II read depth. That cap puts `ami_sana2_rx_start()` at
    `AMI_SANA2_RX_MAX_DEPTH` 32 on the same machines, which is about 45 ms of
    frames at X-Surf-100 rates. The depth-4 floor that lost SYN/ACKs under
    `run-curlverify.sh -p` applies to the 1 MB machine, not to these.

  So it is in the per-packet receive path -- copies through
  `ami_sana2_copy_from_buff`, checksum, or the reader-to-IP-thread handoff --
  and it needs measuring rather than more reading. `tests/tcpdrill` drives a
  synthetic interface and can count what a receive costs without a card.

  One caveat before treating our send figures as a win: a `copy` to a mounted
  fileserver can be measuring buffer acceptance rather than throughput, the
  same way `fitzbench`'s write figure does. It biases every stack the same way,
  so the cross-stack *ranking* stands; what is less safe is the within-stack
  read-versus-write asymmetry. Worth asking how large `largefile` was.

- **PARKED 2026-08-01: the melded copy-and-checksum.** Built, measured, wired
  and proven correct, then parked because it does nothing on real hardware.
  Branches `meld` (the primitive) and `wiring` (the receive path, commit
  `bfa9937`) on origin; nothing is on `main` and the default build is unaffected.

  What it is worth, where it fires: `n68k_copy_sum_longwords()` copies and sums
  in one pass, 177.21 + 201.35 = 378.56 ns/B separate against 256.00 melded --
  32.4% off the pair on a 68020. Wired into the receive path behind
  `AMINETXDUO_RX_COPY_SUM` (default OFF) the whole receive pair went 389.33 ->
  312.92, about 20%; the stamp write, prefix subtraction and acceptance checks
  eat roughly a third of the primitive's gain.

  Why it is parked: **the device's buffer is misaligned on 8 of 9 real
  drivers.** Measured under WinUAE against ariadne, ariadne_ii, x-surf,
  x-surf-100 (Z2 and Z3), hydra, a2065 and cnet -- every one hands
  `S2_CopyToBuff` a pointer at 2 mod 4, and the fast path declines. Under a 400
  pkt/s flood the a2065 gave 1266 consecutive misses and zero stamps, so this is
  structural rather than incidental. Only `eb920.device` (ASDG LAN Rover) is
  aligned, and it could not complete a bulk transfer to benchmark. A read/write
  A/B on a2065 duly showed nothing, because both arms were the same code at
  runtime.

  Two things are worth keeping from it regardless. The correctness work is
  real: 16 tcpdrill cases including a damaged zero-padded tail byte, plus a
  mutation test (injecting `+ 1` into the short-circuit fails all 16) proving
  the path was live rather than decoration. And it found a genuine hazard --
  **the stash survives into the transmit path**, where the same function runs to
  INSERT a checksum, so a received frame whose transport checksum is never
  computed returns to the pool with a live sentinel; case `s16` put three
  segments on the wire with `BAD-TCP-CHECKSUM` before the fix. That is closed by
  clearing the sentinel in a wrapper around `nx_packet_allocate`.

  What would revive it: an opposite-parity path in the melded routine (2 mod 4
  is word aligned, so 16-bit reads are legal even on a 68000), or drivers
  adopting `S2_DMACopyToBuff32`. Note also that no other stack does this --
  Roadshow reads the source exactly once and AmiTCP_NG's hook is a plain
  `bcopy` -- so there is no precedent to borrow from, and the 68020 cost model
  says instructions rather than bus cycles are the currency, which is what
  killed the `swap`-based recombination idea for 2 mod 4 (see RESEARCH.md 86).

## Decided against — do not "fix"

- **The ThreadX tick rate stays at 50 and changing it does nothing**,
  2026-08-01. Swept 20/40/50/60/80/100 Hz, 8 runs per arm, arms interleaved so
  run-order drift could not settle on one; 48 runs, all PASS. The between-arm
  span is not larger than the within-arm spread on any metric -- on write it is
  smaller (2 against 4 KB/s), on read and on the read/write ratio it is the same
  magnitude -- and the scatter is not monotone in rate, with 80 Hz reading
  higher than 60.

  The counters show the mechanism rather than just the null. **Wakeups stay at
  ~850 whatever the rate**, because the source is `timer.device UNIT_VBLANK` and
  the knob does not touch it. Below 50 Hz the extra wakeups are simply empty
  (59% idle at 20 Hz); above it they deliver catch-up bursts (841 of 848
  wakeups deliver more than one tick at 100 Hz). Delivery stays pinned at the
  ~20 ms wakeup either way, so a faster tick buys more ticks and not more timely
  ones. **Instantaneous skew was 0 in all 48 runs** and nothing was ever
  clipped, lost, deferred or over budget: the timer was already keeping up, so
  there was nothing to win.

  Going slower does hand back real work -- 20 Hz delivers 339 ticks instead of
  867 and skips 493 wheel walks -- and it bought 0 KB/s outside the noise.
  Nothing broke at 20 Hz either. The argument against it is not throughput:
  `src/bsdsocket/options.c:83` derives `SO_RCVTIMEO`/`SO_SNDTIMEO` granularity
  from `1000000/NX_IP_PERIODIC_RATE`, so 20 Hz would coarsen socket timeout
  resolution from 20 ms to 50 ms.

  **Two traps for anyone who tries this anyway.** `-D` cannot set the knob:
  `port/netxduo-amiga/inc/nx_user.h:34` hard-defines `NX_IP_PERIODIC_RATE 50`
  unconditionally and is included before `nx_port.h`'s `#ifndef` fallback, so
  overriding `TX_TIMER_TICKS_PER_SECOND` alone leaves the periodic rate behind
  and silently rescales every TCP timer by the ratio -- a sweep that looks
  plausible and measures nothing. Both headers have to move together. And there
  is no delayed-ACK confound to isolate: `nx_tcp_enable.c:111` computes
  `_nx_tcp_ack_timer_rate` as `ceil(NX_IP_PERIODIC_RATE / NX_TCP_ACK_TIMER_RATE)`,
  so the ACK interval self-scales to a constant 200 ms at every rate.

- **RFC 3542's extension headers stay unimplemented.** `IPV6_RTHDR`,
  `HOPOPTS`, `DSTOPTS`, `RTHDRDSTOPTS`, `PATHMTU`, `RECVPATHMTU`,
  `USE_MIN_MTU`, `DONTFRAG` and `NEXTHOP` are extension-header and path-MTU
  state NetX Duo does not expose, so there is nothing under them to reach. The
  rest of RFC 3542 is built; what its constants were fixed at, and why they
  cannot move, is in `docs/NDK-ADDENDUM.md`.

- **A browse reports the whole peer cache, not the browse window**, and stays
  that way, 2026-07-31. `netstack_mdns_browse_collect()` walks
  `nx_mdns_service_lookup()` by index, which is the whole cache, and nothing
  ages entries from our side -- the module expires them by TTL, and an unplugged
  machine sends none of RFC 6762 §10.1's goodbyes. Filtering to entries actually
  refreshed inside the window means walking the RR cache instead of the public
  lookup: `nx_mdns_rr_elapsed_time` and `nx_mdns_rr_remaining_ticks` carry the
  freshness and `NX_MDNS_SERVICE` does not. Not worth that until someone reports
  a switched-off machine lingering, which every other mDNS browser does too.

- **RFC 3678 source filtering stays out**, 2026-07-31.
  `IP_ADD_SOURCE_MEMBERSHIP`, `IP_BLOCK_SOURCE` and the `MCAST_*` family need
  IGMPv3, which the vendored NetX Duo does not implement -- it speaks IGMPv2
  and the source lists have nowhere to go. RFC 1112 membership is what shipped
  (`src/bsdsocket/mcast.c`) and is what SSDP, UPnP and a ported mDNS actually
  call.

- **RFC 6724 default address selection**, 2026-07-31: does not apply here. It
  sorts a list of candidate destinations, and `getaddrinfo()` returns at most
  one address per family (the resolver under it answers with a single address,
  not a set -- see `src/bsdsocket/addrinfo.c`). With two entries at most its
  rules collapse to "which family first", which is answered deliberately: IPv6
  then IPv4. It would start to matter only if the resolver ever returned
  address sets.

- **`vsyslog()` stays `ENOSYS`**, 2026-07-31. The two tags that aim it,
  `SBTC_LOG_FILE_NAME` and `SBTC_LOG_HOOK`, are refused (above), so a syslog
  that reached only the serial debug log would give a caller no way to direct
  its output or read it back. `LOGSTAT`/`LOGMASK`/`LOGFACILITY`/`LOGTAGPTR`
  stay stored and unread, which costs a caller nothing. If syslog is ever
  wanted it is those three together, not the call on its own.

- **`AAMR_AddressInUse` / `AAMR_MaskChangeFailed` are never produced**,
  2026-07-31. Answering the first truthfully means duplicate address detection
  -- probing for the address before committing to it -- which is the real
  feature and a change in what the stack puts on the wire, needing a second
  machine holding the address to test against. The result code is downstream of
  that, and inventing one without the probe would be a worse answer than
  `AAMR_Ignored`. Raise it as DAD if it is wanted, not as a code.

- **`SBTC_IP_FILTER_HOOK` and the `mbuf_*` family**, 2026-07-31. The hook hands
  a filter an mbuf chain, so servicing it means synthesising BSD mbufs around
  NX_PACKETs for every IP packet in and out -- a packet-buffer abstraction the
  stack does not otherwise have, on the hot path, for a facility whose only
  real caller is a firewall nobody has asked for. Both stay stubbed together.

- **`IFQ_MaxReadRequests` / `IFQ_MaxWriteRequests` stay unanswered**,
  2026-07-31. The autodoc types them `(LONG)` where all 40 neighbours are
  `(LONG *)`, and on a query a bare `LONG` has nowhere to put the answer, so it
  is almost certainly a typo -- but writing through a `ti_Data` that a caller
  passed as a scalar would corrupt its memory, and there is no way to tell the
  two apart at the call. They fall to the `default:` branch, which ignores
  unknown tags rather than refusing them, so nothing else in the list is lost.
  The same reasoning is in `src/bsdsocket/interfaces.c` beside the tag.

- **`SBTC_LOG_FILE_NAME` and `SBTC_LOG_HOOK` are refused**, 2026-07-31. The
  autodoc sanctions it in their own entries: "This tag is an extension to the
  AmiTCP V4 API and cannot be expected to be supported by older
  'bsdsocket.library' versions." Neither constant is in the NDK headers either,
  so a caller has to define it before it can pass one. Refusing costs a rare
  caller a tag list it was told to expect to lose.

- **`sendto()` with an address on a connected UDP socket.** Doc says `EISCONN`;
  everything portable dropped that rule.

- **UDP send with no destination returns `EDESTADDRREQ`**, not `-udp-`'s
  `ENOTCONN`. Ours is what 4.4BSD returns and what portable code checks.

- **`listen()` on an unbound socket fails** rather than auto-binding.

- **`MSG_OOB` refused by the msghdr forms**; `MSG_DONTROUTE` accepted and
  ignored.

- **`listen()` backlog is 8**, where the doc's BUGS claims a silent limit of 5.

- **`Dup2Socket` allows any in-range slot** — the doc's `EBADF` wording would
  make `dup2` onto a free slot illegal, defeating the call.

- **Request-count defaults** (ours sized from the packet pool, not 32/32/4).

- **`IP_DEFAULT_TTL` is 128**, doc says 64. Deliberate, 2026-07-31.

- **`gai_strerror()` takes its argument in A0.** The autodoc says D0; the SFD
  and pragmas say A0, and callers link against the pragma. The doc is wrong.

- **`bpf_open()` returns 0 for channel 0**, against the doc's *"handle > 0"* —
  RESEARCH §60 decoded Roadshow's own libpcap using it as a 0-based channel.

- **`OpenLibrary` allows Tasks and non-opener Processes**, which the doc
  denies. More permissive, never less.

- **`GetDefaultDomainName()` refuses rather than truncates.**

- **Packet pool sizing left alone**, 2026-07-31 — the heuristic reaches the
  floor on small machines (1 free of 17 observed on 1 MB), so the minimum must
  stay.

- **No stripped drawer beyond `68000-minimal`**, 2026-07-31 — the full build is
  what generates useful bug reports.

- **Two physical NICs**, dropped 2026-07-31: `romtype_restricted()` keeps only
  the first card, so it is untestable, and the complexity is not worth a rare
  case. Recovery SHA `d22a33e`.

- **A Kickstart 1.3 build**, dropped 2026-07-31: TheWire13 already covers that
  platform. Our side was closer than expected -- tag walking is hand-rolled so
  `utility.library` is never needed, the library's DOS surface is all V33, and
  the 68000 build ships -- but `ReadEClock` is V36 and everything 0.14.0 did
  for clock correctness rests on it; a 1.3 fallback is the 50 Hz CIA/VBlank
  source, exactly one tick of resolution, which retires the timer budget. The
  tools are the volume (`ReadArgs` 27 sites, `FreeArgs` 191, `VPrintf` 12).
  Never established whether any SANA-II driver runs under V34 at all, which was
  the gating question.

## Environment and tooling

- **A task can hold about five library bases that use a `WaitSelect()`
  timeout.** Each base takes an event signal from the calling task
  (`library.c:244`), and the first timeout takes another for the timer
  (`select.c:475`), out of the 32 bits a Task has. `OpenLibrary()` refuses once
  they run out, which is the right answer -- better than succeeding and failing
  the first `WaitSelect()`. Found 2026-07-31 when `run-cycledrill.sh` first
  exercised a timeout on every nested base and eight opens got five. The drill
  now asks for four. A program that opens many bases and uses timeouts on all
  of them will meet this; nothing in the tree does.

- **`tests/clients/run-argvexit.sh` is gone, and what it was for is measured
  elsewhere.** It never completed a run. Under Amiberry on playhouse3 the guest
  boots, `ToolsSmoke` starts, and `DH0:tools.txt` gets as far as the
  `===== SYS:ArgvExit =====` header and then stops: no output, no return code,
  no `.done`, deadline. So the plumbing works and `ArgvExit` itself never comes
  back out of `SystemTagList()` -- making it run is a guest-side debug of the
  probe, not a fix to a shell script. The toolchain was ruled out separately:
  `tools/fix-toolchain-crt0.py --check` against the pinned toolchain on
  playhouse2 and playhouse3 alike reports `11 ok, 1 skipped` for the frame skew
  and `2 call site(s) already push __argv by value` for the argv indirection,
  which is the immune case, not a missing patch site.

  The 256 KB per-invocation stack leak it was written for is covered:
  `clients/dropbear/run-fsuae.sh -A` runs `dbclient` six times in one boot and
  `ClientRun` prints `AvailMem()` after every command, so a per-invocation leak
  is a constant step down the list. That measurement found the leak
  (266,368 bytes a run) and proved the fix (0). A harness that has never run
  looks like coverage and is not, which is how the leak survived the first
  time. Removed 2026-07-31.

- **Amiberry's A600 PCMCIA emulation does not work, for any stack.** On an
  A1200 with `-N ne2000_pcmcia` the emulated RTL8019 and Rolf Anders'
  `cnet.device` drive a full DHCP lease -- ours reports `eth0: online, address
  10.0.2.15` and the Roadshow 1.15 demo reports the same address from the same
  card. Move the identical staging to `-m A600` and both fail: ours cannot open
  the device, Roadshow says `Could not add interface "eth0" (Input/output
  error)`. Not a memory limit -- the failing A600 run had 4.6 MB free. So a
  PCMCIA test has to run on the A1200 profile, and an A600 failure says nothing
  about the code. The Roadshow demo is the control that establishes this and
  lives in `~/amiga-assets/stacks/` on playhouse3, reached by
  `AMINETXDUO_CMP_ROADSHOW`. Found 2026-08-01.

  Not memory either, which was the other candidate: an A1200 given
  `chipmem_size=2;bogomem_size=0;fastmem_size=0` -- 1 MB of chip and nothing
  else, the supported floor -- brings the same card up and leases an address,
  with 374,760 bytes still free afterwards. Every A600 run was also a 1 MB run,
  so the two were confounded until that was measured separately. It is also the
  first time the README's 1 MB floor has been shown with a live interface
  rather than inferred; `run-oommsg.sh` only proves the other end, that 512 KB
  cannot start the stack.

- **`/opt/amiga` on playhouse2 carries the argv bug in all eleven `crt0.o`.**
  Locally built, GCC 16.1.1b, and `--check` reports `11 buggy` -- it has the
  compiler fix for the frame skew and not newlib's `120371e` for the argv
  declaration. Nothing releases through it, so this is a note about the box
  rather than about a build: anything built there against `-lc` without running
  `fix-toolchain-crt0.py` first hands ported clients `&__argv`. Found
  2026-07-31.

- **The two-interface source case is proved on a host, and cannot be proved on
  a guest.** TCP leaves from the address `bind()` named --
  `nxd_tcp_client_socket_source_connect()` in the NetX fork -- and the case it
  exists for is two interfaces on one subnet, source on one and route out of the
  other. `tests/netstack/host/test_tcp_source_connect_host.c` asserts it against
  an `NX_IP` with two `nx_ip_interface[]` entries filled in, compiling the real
  connect, route lookup and SYN build and checking the source the SYN carried.

  What cannot be shown is the same thing through two live SANA-II drivers, and
  the reason recorded here was wrong. It said `AddNetInterface eth0` hangs with
  a second card present. It does not: with `a2065` on Zorro and the NE2000 PC
  Card on PCMCIA -- two different buses, and Fast RAM held to 4 MB so it stays
  clear of the credit-card window at $600000 -- `AddNetInterface eth0 eth1`
  returns 0, `eth0` leases an address, and `ShowNetStatus` lists both
  interfaces with `eth1` offline and the right advice against it.

  The real limit is that `cnet.device` will not open while the A2065 is
  present, though it opens perfectly well on its own, and Amiberry offers
  exactly two network boards -- `a2065` and `ne2000_pcmcia` -- so that is the
  only pair there is. Two Zorro cards were the earlier attempt and are what the
  hang belonged to. `SrcProbe` already takes a second address and a destination
  for a machine that can host one. Measured 2026-08-01.

- **The `sana2_rx.c` reader orphan does not reproduce under emulation.**
  `ami_sana2_rx_stop()`'s last-resort path logs `reader N did not stop; leaking
  its stack` and leaks 32 KB when a SANA-II driver ignores `AbortIO`, which
  Commodore's a2065.device 2.16 is documented to do. Thirteen full teardowns
  under Amiberry with that driver logged it zero times, so the emulated card
  returns its queued `CMD_READ`s where the real one may not.
  `run-cycledrill.sh` greps the serial log for it on every run and prints the
  count; `AMINETXDUO_CYCLE_ORPHAN_FATAL=1` makes it fail. Confirming it needs
  real hardware. Checked 2026-07-31. Nothing is
  outstanding in the code: the free sits outside the started gate where the
  teardown owns it, and `run-cycledrill.sh` greps every run for the message and
  can be made to fail on it. What is missing is a sighting, and only a driver
  that really ignores `AbortIO()` can provide one.

- **`run-fitzbench.sh` refuses `playhouse2` outright** over the uncomputed TX
  checksums that `ethtool -K eth0 tx off` fixed there on 2026-07-31, and can be
  relaxed. Query the setting by full path -- `/usr/sbin` is not on a non-login
  ssh PATH.

- **`run-fitzbench.sh` prints a write figure that is not a rate** — it stops
  timing when the write call returns, not when data drains. Guest-timed 1718
  KB/s against a measured wire rate of 364. Reads agree between clocks; writes
  diverge ~4.7x.

- **cppcheck**: baseline is from 2.20.0, gate hosts have 2.17.1, so the stage
  skips itself. Install 2.20.0 or regenerate to make it gate again.

- **FS-UAE cannot boot headless on playhouse3** (`FATAL: [GLAD] …`). Harnesses
  take `-A` to use Amiberry instead, and `-a ARGS` to pass arguments.
