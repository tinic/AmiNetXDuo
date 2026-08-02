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

- **AmiTCP_NG needs arexx and the math libraries; that was the `errno 43`**,
  found 2026-08-02. Two earlier attempts read its `AddNetInterface` failure --
  `EPROTONOSUPPORT` out of `socreate()` with an empty `protosw`, and on the
  second attempt no open of `a2065.device` at all -- as a protocol-domain
  initialisation fault, and guessed the cause was its harness wanting AmigaOS
  3.2 against our bare Kickstart 3.1 directory boot.

  **That guess was wrong.** The startup needs `rexxsyslib.library` and the math
  libraries in `LIBS:`, which a bare boot does not carry and a full Workbench
  does. With them staged it comes up. The OS version was never the problem, so
  do not go looking for a 3.2 image.

  This makes the third arm of the stack comparison reachable. It was left
  unmeasured only because playhouse3 was too contended to run it cleanly --
  several Amiberry instances on `ens18` sharing an emulated MAC, which silently
  spoils both guests. Give each guest a distinct MAC and check the lease.
  `tests/perf/run-stackprof.sh` is the matched harness; the `ours` and
  `roadshow` arms both leased 192.168.1.133, so a third arm has to be checked
  against that rather than assumed.

- **SACK, held pending evidence that there is loss to recover from**, assessed
  2026-08-02. AmiTCP_NG has it; we have none -- **zero mentions of SACK
  anywhere in the vendored NetX Duo**, headers or source.

  The prerequisite is met, which is the part that could have blocked it
  architecturally: NetX Duo does queue out-of-order segments, inserting in
  sequence order through `nx_tcp_socket_receive_queue_head`
  (`nx_tcp_socket_state_data_check.c`), so there are holes for SACK blocks to
  describe.

  Scope. The RECEIVER half is what would help our read deficit -- advertise
  SACK-permitted in the SYN, generate blocks from the receive queue's holes,
  fit them in the ACK option space -- so that a sending Linux retransmits only
  what is missing rather than NewReno's one segment per round trip. The sender
  half, parsing a peer's blocks and retransmitting selectively, is separate and
  larger. All of it is inside vendored TCP: the substitution trick that gives
  us `_nx_ip_checksum_compute` works for one function and does not scale to a
  receive path.

  **That is a patch to NetX Duo and an upstream contribution, not a fork we
  carry.** SACK is a real gap in an industrial TCP stack, and this project has
  an established route upstream with a recipe recorded alongside the existing
  work. Upstreamed, the diff goes away -- materially cheaper than the fork this
  entry first described.

  **Two justifications, and they are independent.** One: it may account for the
  read deficit -- but SACK improves loss RECOVERY, and a capture comparing us
  against Roadshow on a matched rig is in flight. If that shows near-zero
  retransmits, SACK cannot explain a 2x gap and does not become urgent. Two: it
  is worth contributing to NetX Duo whatever our own benchmark says, and that
  does not wait on anything. Only the PRIORITY is held, not the case.

  Note the correlation is weaker than it looks: in the user benchmarks that
  prompted this, AmiTCP_NG reads at 906/422/376/111 KB/s -- between us and
  AmiTCP 4.6, which is the read champion on three of four machines. Whether 4.6
  has SACK is unknown, so SACK does not obviously separate the field.

- **tcpdrill queues IPv6 neighbour-discovery frames as if they were results.**
  `pump()` drops IPv4 traffic that is not TCP to the peer -- mDNS, DHCP, IGMP
  -- and says so at length, but the same filter passes every `ether=0x86dd`
  frame straight into the queue, so a router solicitation or a multicast-listener
  report lands where the next `tx` expects a segment. That is one frame of skew
  and every later assertion in the case is wrong. `w01` and `x01` fail this way
  on a run where nothing else does, which is exactly the intermittency the
  filter was added to remove. The fix is the same test one family over; it was
  not made alongside the `send()` work because it shifts which frames other
  cases see and wants a run of its own.
- **Once the WebDAV server works, two things follow**, noted 2026-08-02 so they
  are designed for rather than retrofitted.

  **An installer option.** The server should be something a novice install can
  turn on, which means it has to start from `S:User-Startup` with no Shell
  attached -- no stdout to print to, no console to take Ctrl-C from. A tool
  written around `tool_error()` and `tool_break()` cannot be started at boot
  without rework, so the daemon path has to exist from the beginning even if
  the installer question comes later. The installer's own constraints are
  already recorded: `askchoice` labels are capped at 22 characters and anything
  level-dependent must run after `(welcome)`.

  **Advertise `_webdav._tcp` over mDNS**, which is what makes the Amiga appear
  in Finder's sidebar and in a Linux file manager without anyone typing an
  address. That is the difference between a server you have to know about and
  one that shows up.

  **This needs new public API.** `include/aminetxduo/netstack.h` exposes browse
  only -- `netstack_mdns_browse_start`/`stop`/`collect` and
  `netstack_mdns_hostname`. There is no publish side. The vendored addon has
  `_nx_mdns_service_add()` and `_nx_mdns_service_delete()`
  (`third_party/netxduo/addons/mdns/nxd_mdns.h:1278`), so this is plumbing
  rather than protocol work, but it is a new entry point and publishing one
  fixes its shape -- see what publishing `NetStackQuery`/`NetStackControl` in
  0.16.2 committed us to.

- **Where a transfer's time actually goes, measured 2026-08-02 -- and it is not
  where this document has been saying.** A sampling profiler now exists
  (`tests/perf/prof/`, branch `prof`, off unless `-DAMINETXDUO_PROFILER=ON`,
  no `src/` changes). 1 MB TCP transfer, A1200/68020, 1000 Hz, 4411 samples in
  the transfer phase, 0.2% unattributed:

  | category | wire | loopback |
  |---|---|---|
  | NetX Duo protocol | 25.6% | 16.7% |
  | ThreadX + Amiga port | 23.3% | 16.2% |
  | Kickstart (Exec) | 19.2% | 19.4% |
  | copy (net68k asm) | 19.0% | 34.5% |
  | checksum | 12.3% | 12.6% |

  **The "roughly 78% is inside NetX Duo's protocol processing" claim, derived by
  subtracting measured primitives from a measured transfer, is wrong.**
  Copy and checksum are 31.3% rather than about 22%, and the remainder is not
  mostly protocol code: **ThreadX and Exec together are 42.5%, larger than NetX
  Duo's own 25.6%.** The largest non-copy cost is scheduling glue --
  `_tx_thread_interrupt_restore` and `_tx_thread_interrupt_disable`, the
  Forbid/Permit wrappers, at 6.9%, plus Exec's `Reschedule`, `Switch`,
  `Dispatch` and `Supervisor` at 10.5%. That is the 214 us per-call bracket
  showing up directly, and it says the bracket work is aimed at the right thing
  and that there is more there than the bracket alone.

  Top single entries, wire: `n68k_copy_bytes` 18.2%, `n68k_sum_longwords` 9.5%,
  `_tx_thread_interrupt_restore` 4.2%, `Supervisor` 3.4%, `Reschedule` 2.8%.
  Top 24 is 73.0%.

  **It holds in a real application, but only just, 2026-08-02.** A `fitz`
  transfer through the shared library, now that the profiler can name functions
  inside it, renormalised over the same four categories and with Exec's idle
  loop excluded:

  | | NetX Duo | copy+checksum | Exec | ThreadX | ThreadX+Exec |
  |---|---|---|---|---|---|
  | this bracket test, wire | 25.6% | 31.3% | 19.2% | 23.3% | **42.5%** |
  | fitz, whole run | 33.2% | 32.3% | 20.7% | 13.8% | **34.5%** |
  | fitz, read arm | 33.2% | 34.8% | 20.3% | 11.7% | **32.0%** |
  | fitz, write arm | 33.1% | 30.0% | 21.0% | 15.8% | **36.8%** |

  So the ordering survives on a write and over the run as a whole, and the
  margin collapses from 16.9 points to 1.3 -- and on a read it reverses. What
  moves is ThreadX's own share, 23.3% down to 13.8%: this bracket test drives
  one socket from one task as fast as it can, where a real client spends much
  of a read waiting, so the per-call bracket is amortised over more bytes.
  **Quote the 42.5% as what a tight single-socket loop costs, not as what an
  application sees.**

  **A CIA timer cannot be used for this and fails silently.**
  `AddICRVector()` arbitrates the ICR vector, not the hardware. CIA-B timer B
  ran at a correct 1000 Hz and stopped at the first `ami_millis()`, because
  timer.device's MICROHZ unit took it back; CIA-B timer A then ran 0.4-1.5 s and
  stopped with `ciaicr=$85`, an interrupt raised and never acknowledged -- the
  Exec EXTER race, where Exec clears `INTREQ` around the CIA ICR read and an
  interrupt landing in that window leaves the line asserted with no further edge
  possible. **Both failures still produced eight correctly-sampled PCs, which is
  enough to rank functions convincingly and is pure noise.** The source is now
  audio channel 3 at level 4 -- no latching chip in the acknowledge path, and
  level 4 also sees inside the level 2/3 handlers where a SANA-II receive runs
  -- and `prof_start()` measures each candidate over eight windows, rejecting
  any that does not hold rate in every one.

  Two attribution notes worth keeping. Exec keeps INLINE code in some jump-table
  slots rather than a `JMP`, `Forbid` and `Permit` among them, which lost 7.2%
  until a PC inside `[base-negsize, base)` was attributed to that slot. And
  `Disable()` masks INTENA, so those sections are unsampled and their time lands
  on whatever runs next; `Forbid()` does not mask interrupts and is sampled
  normally, which is what matters here since that is where the bracket lives.

  Not yet run on a 68000: fs-uae aborts host-side on this machine for the
  A500/A500+/A600/A2000 profiles before the guest boots. Verified on 68020
  (97/98/96% containment against assembly kernels with explicit end labels,
  sample share within 0.1 points of wall clock) and 68030 (100%).

- **A profiled `fitz` transfer says the read direction is not CPU-bound, and
  that the profiler cannot see inside `bsdsocket.library`,** 2026-08-02.
  `tools/profiler/Profile` wrapped around a real `fitz mount` of a LAN peer --
  the shared library, not a statically linked test binary -- A3000 under
  bridged Amiberry, 4 MB file in 32 KB chunks, three passes each way, 1000 Hz,
  30978 samples over 30.87 s, 0 interrupts dropped and 0.0% unsampled. Split by
  window, using FitzBench's own per-pass timings to place the boundaries:

  | | read (12.7 s) | write (6.8 s) |
  |---|---|---|
  | Exec idle loop | 59.7% | 22.1% |
  | `bsdsocket.library` | 31.7% | 63.9% |
  | Exec, real work | 3.8% | 7.1% |
  | `a2065.device` | 2.4% | 4.3% |
  | Fitz itself | 1.5% | 0.3% |

  **Reads leave the machine 60% idle**, so nothing done to our own code can
  move the read figure on this rig; writes are the CPU-bound direction, which
  is what `fitzbench`'s write figure measuring buffer acceptance looks like
  from the other side. The idle is one address, `$00f81496`, Exec's own idle
  loop. It is *reported* as `exec.library/Dispatch`, that being the nearest
  preceding jump-table entry, and a 7 s window with the mount up and no traffic
  is 99.0% that single address -- which is what identifies it as idle rather
  than as scheduling, and is worth doing before reading any Exec share.
  Exec's real work is 9.4% of the busy CPU on reads: `Signal` 1.9%, `Permit`
  1.4%, `Switch` 1.0%, `PutMsg` 0.7%.

  **Fitz itself is 0.76% of the run**, and 87% of that is its `memcpy`. Every
  other function of its own is single-digit samples -- `send_all` 6, `get_u32`
  4, `recv_all` 3, `do_rpc` 2 -- and the released vbcc binary measures 1.16%
  with the same shape. There is nothing in Fitz's code worth optimising.

  What is worth changing in Fitz is read-ahead depth rather than code:
  `amiga-client.c` `h_read()` issues one synchronous `do_rpc()` per
  `DEF_RA_SIZE` window, 32 KB, with nothing pipelined behind it, where the
  write path has `drain_async_writes()`. Mounting `BUFS 131072` takes read from
  869 to 993 KB/s and `BUFS 262144` to 1029, write unchanged. Idle stays above
  54% throughout, so round trips are a real but not the binding constraint;
  the rest looks like Amiberry's a2065 frame pacing, which this rig cannot
  separate from the wire.

  **The tool named `bsdsocket.library` by module and nothing else, and 93.8% of
  its samples landed in the unnamed body.** That was 78.7% of the busy CPU on a
  read and 82.0% on a write reduced to one line. `prof.c` recorded a range per
  library from the hull of its jump-table targets, which here was two hulls of
  99 KB against a 474 KB code hunk, and everything between them resolved to
  `(unattributed)`.

  **DONE, 2026-08-02.** The library carries five self-identifying longwords
  saying where its seglist is, `prof.c` scans for them and checks the answer
  against the jump-table hull it already had, and `profreport.py --lib` reads
  the library's own map and objects. On the re-run the previously unnamed
  samples are 100.0% named with no residue, and `(unattributed)` over the whole
  profile falls from 34.0% to 2.5% -- that remainder being the `fitz` handler,
  which is a different program the run never loaded, and a few Kickstart
  addresses far from any entry point. `tools/profiler/ReadMe` has the
  convention and why it is a scanned record rather than an offset.

- **Roadshow reads twice as fast as we do while spending MORE CPU per byte, so
  the read gap is not CPU work,** 2026-08-02. `tests/perf/run-stackprof.sh`
  holds the rig fixed and makes the library the only variable: bridged
  Amiberry A3000, Kickstart 3.1 40.68, one `a2065.device`, the released `fitz`
  binary, `FitzBench KB=4096 CHUNK=32768 REPS=3`, `fitz-serve` on playhouse4.
  Both stacks came up behind the same MAC and took the **same DHCP lease**,
  192.168.1.133 -- that check is in here because an earlier stack comparison
  turned out to have run its two arms at different addresses. 1000 Hz, no
  samples dropped, 0.0% unsampled. Two profiled runs per stack:

  | | AmiNetXDuo read | Roadshow read | AmiNetXDuo write | Roadshow write |
  |---|---|---|---|---|
  | throughput | 980, 982 KB/s | **1259, 1909 KB/s** | **1929, 2058 KB/s** | 1254, 1301 KB/s |
  | Exec idle loop | **65.1, 65.3%** | 23.9, 50.7% | 19.0, 20.2% | 10.9, 12.7% |
  | busy CPU | 34.7% | 76.1% | 79.8% | 89.1% |
  | **CPU per MB moved** | **363, 366 ms** | 409, 417 ms | **399, 432 ms** | 704, 716 ms |

  **The read arm is the one a user notices and it is not ours to fix in code.**
  We move half Roadshow's bytes with the machine two-thirds idle, and we do it
  for FEWER milliseconds of CPU per megabyte than Roadshow spends. Something
  that is 12% cheaper per byte and 2x slower is not losing on CPU work. The
  clinching number is Roadshow's own spread: its two runs read at 1259 and
  1909 KB/s for 5001 and 4912 ms of busy CPU -- **the busy CPU per byte is
  constant and only the waiting changes.** Both stacks are limited by
  something that is not the processor.

  The module split, at module granularity for both because only our library
  carries the `ProfSegTag` (`sp-ours2` and `sp-rs2`):

  | | AmiNetXDuo read | Roadshow read | AmiNetXDuo write | Roadshow write |
  |---|---|---|---|---|
  | Exec idle loop | 65.3% | 23.9% | 20.2% | 10.9% |
  | the stack's own library | 23.8% | 51.8% | 60.7% | 60.8% |
  | unnamed, beside that library | 0.0% | 4.6% | 0.0% | 9.0% |
  | Exec, real work | 4.5% | 3.5% | 11.0% | 5.2% |
  | `a2065.device` | 2.5% | 4.2% | 6.4% | 4.9% |
  | `timer.device` | 0.3% | **5.1%** | 0.4% | **7.7%** |
  | the `fitz` handler | 2.9% | 5.7% | 0.0% | 0.0% |

  Two things in that table are not artefacts. **Roadshow runs its timers
  through `timer.device` and we do not** -- 5.1% and 7.7% of its arms against
  0.3% and 0.4% of ours, which is most of why its write direction costs 704 ms
  a megabyte against our 399. And the "unnamed, beside that library" row is
  Roadshow's own code: a library with no seglist tag is bracketed by the hull
  of its jump-table targets, which brackets the ENTRY POINTS and not the body,
  so its named share is a floor rather than a figure. `profsplit.py` counts
  those samples on their own line instead of naming them out of somebody
  else's build.

  **It is not `fitz`'s read-ahead either.** `h_read()` issues one synchronous
  `do_rpc()` per window with nothing pipelined behind it, so a deeper window
  was the obvious candidate. Mounting `BUFS 262144` -- four times the default
  -- takes read from 982 to 1026 KB/s and leaves **idle at 65.9%, unmoved**.
  Quadrupling the client's read-ahead changes neither the waiting nor the
  throughput, so the constraint is below it: our receive path, or the
  emulator's a2065 frame pacing, which this rig cannot separate from the wire.
  A capture on the host is what would separate them, and that is the next
  measurement rather than a code change.

  **The profiler needs nothing of ours to do this.** It ran with no
  `bsdsocket.library` on the disk at all (the `profspin` proof), and against
  Roadshow's library with ours absent from the staged volume -- md5-verified,
  not assumed -- resolving Kickstart, `a2065.device`, `timer.device` and the
  foreign library by module throughout. Sampling was proved on this rig before
  any of it was believed: `profspin` under Amiberry/A3000, containment and
  proportionality both within 0.2 points of the program's own clock, 0
  failures. The sampler also does not move what it measures: unprofiled
  control runs read 940 (ours) and 1834 (Roadshow) against profiled 980 and
  1909.

- **AmiTCP_NG 4.1.4-beta will not come up on a Kickstart 3.1 directory boot,
  on a REAL `a2065.device`,** 2026-08-02. It was to be the third arm above.
  `AddNetInterface` fails with `errno 43` `EPROTONOSUPPORT` and the run stops
  there; `GetNetStatus` then hangs. This is the same failure a previous
  investigation hit against a synthetic SANA-II device, so the device is not
  what causes it: the emulator log records **no open of `a2065.device` at
  all**, and it fails identically with AmiTCP_NG's own
  `Storage/NetInterfaces/A2065` configuration file rather than ours. The
  staging is not short of anything either -- its `db/` is at `SYS:AmiTCP`,
  which is one of the two fallbacks its library assigns for itself when no
  `AmiTCP:` exists, and its own `ng_readconfig_noargs()` returns TRUE with no
  config file rather than refusing to start.

  `EPROTONOSUPPORT` out of `socreate()` means the protocol switch is empty,
  which means `domaininit()` never ran or was undone. In `src/kern/
  amiga_main.c` the self-starting path reaches it through half a dozen
  `goto fail` points, and one of them fires. **Which one was not chased, and
  should not be: it is somebody else's stack.** The likeliest reason is the
  environment rather than a defect -- their harness mounts a full **AmigaOS
  3.2** Workbench as `SYS:`, and this is a bare directory hard drive on
  Kickstart 3.1 with only the assigns `envsetup` makes. `~/amiga-assets/wb`
  has 2.04 through 3.1 and no 3.2, so that hypothesis cannot be tested here.
  Reproducing it would mean a 3.2 install, and then all three arms would have
  to move onto it for the comparison to still be matched.

- **A cycle-attribution profiler, to find where a transfer's time actually
  goes.** The copy and checksum together are about 20% of a wire transfer and
  roughly 78% is unaccounted for inside NetX Duo's protocol processing.
  AmigaOS has no profiler, and `tests/perf/perf_test.c` can only say where the
  time is NOT.

  A working prototype exists on branch `agent/moira-eval`: `moiraprof.cpp`
  produces callgrind-style flat and inclusive profiles with call edges and
  **zero source changes**, at 22-25 M instructions/s, which is 17x a 14 MHz
  68020 -- a 20-second Amiga run profiles in about a second. No instruction
  hook is needed; Moira's `execute()` is public and runs exactly one
  instruction, so the profiler is an outer loop rather than a callback and can
  stop and inspect anywhere. Inclusive costs come from decoding `jsr`/`bsr`/
  `rts` into a shadow stack, which avoids `-finstrument-functions` skewing the
  small leaf functions that matter here.

  Two traps already found and handled, which any reimplementation will hit:
  statics are invisible in `HUNK_SYMBOL` and their cycles land silently on the
  preceding global (recover via the link map plus per-object `nm`), and the
  shadow stack has to unwind on stack-pointer regression because ThreadX swaps
  stacks, so an rts-only stack drifts out of step at every context switch.

  **The vehicle should be vAmiga headless, not a flat memory image.** A flat
  image needs 41 distinct Exec and dos entry points for
  `port/threadx-amiga/src/` and `src/netstack/` alone (54 tree-wide), before a
  SANA-II device to originate packets and timer.device as the kernel's clock --
  that is writing a small AmigaOS, and the resulting profile would EXCLUDE
  Exec, when `Forbid`/`Signal`/`Wait` are part of the 78% being hunted. vAmiga
  builds a real headless target (`VAHeadless`), its `Core/` tree is plain C++
  with its own CMake, RetroShell scripts can insert a disk and boot Kickstart so
  it batches in CI, and its `MoiraConfig.h` is byte-identical to upstream on the
  one flag the profiler needs. Decisively, it HAS the memory system, so the
  11-15% gap between bare Moira and real measurements closes.

  Note this is worth doing for attribution only. Moira must not be used to tune
  against -- see the entry below for why.

- **The 68000 byte-loop fallback costs 6.1x, not the 4x the source says**,
  measured 2026-08-01 on a cycle-exact A500: 5.4 us/B against 0.89 for the
  `movem.l` path. `src/net68k/n68k_copy.S` takes it whenever `to` and `from`
  disagree in bit 0, because on a 68000 a misaligned word access is an address
  error rather than a slow path.

  It should never fire on the receive path -- the alignment census says the
  cases that occur are 0 mod 4 (application buffers, packet prepend pointers)
  and 2 mod 4 (eight of nine real drivers), both of which match the
  destination's parity. So this is only a cost if some path hands over an ODD
  buffer, and nothing has been measured doing that. Worth finding out whether
  any real driver does before deciding it is theoretical, because 6.1x is a
  bigger number than anything else measured in the data path today.

- **The 68000 numbers for both net68k primitives**, measured 2026-08-01 under
  WinUAE 6.0.3 on a purpose-built cycle-exact A500 profile
  (`C:\aminetxduo\run\m0ab\config.uae` on winbuilder; nothing like it existed,
  every prior cycle-exact config there is a 68030). Harness on branch
  `m68000-ab`: both predecessors assembled alongside the shipped versions in
  ONE binary, plus a second assembly of each shipped sequence at a different
  address as a floor check.

  Both changes help MORE on a 68000 than on the 68020 they were tuned against.
  Copy -13.1% at 0 mod 4 and 2 mod 4 (the only alignments that occur; 1 and 3
  are flat because both implementations fall into the same byte loop and cannot
  differ). Checksum -23.9% at 1460 B and **-29.4% at 20 bytes**, which is the IP
  header this stack checksums on every packet both directions -- the short-call
  path was the thing most likely not to transfer, given a 68000 has no
  instruction cache, and it is the best row in the table. Pipeline ceiling
  161 -> 175 KB/s. **One implementation for all four targets is right; no
  per-CPU selection is warranted.**

  Fidelity was checked rather than assumed: `cpucal` prints ADD.L at 8.00
  cycles against a published 8 and MOVE.L at 4.00 against 4, and a model with a
  flat per-instruction cost would print those equal. The same probe on the
  existing A3000 profile charges MULU.L 3.88 cycles against a published 44, so
  **numbers taken from that profile mean nothing** -- worth knowing before
  anyone quotes one. `cpucal` needed its two multiply kernels gated out to
  build for a 68000 at all, since MULU.L 32x32 does not exist on the part.

  Two caveats recorded with the data: the implied clock is 6.69 MHz against a
  PAL A500's 7.09, a uniform ~6% consistent across every kernel that looks like
  OS interrupt service and applies equally to both arms of every A/B; and
  per-sample spread reaches 15% in quantised ~1.5 ms steps, so single samples
  are useless and everything above is best-of-nine.

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
  32.4% off the pair on a 68020.  Both halves have since moved and the melded
  routine has not: the copy is 159 (RESEARCH.md 86) and the checksum 149.8
  (RESEARCH.md 87), so the pair is ~309 against 256 and the margin is 17%, not
  32%.  Reviving this means rewriting the melded loop around movem.l and the
  chained addx first, or the comparison is against primitives that no longer
  exist. Wired into the receive path behind
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

- **Moira cannot be tuned against, and neither can Musashi**, evaluated
  2026-08-01, branch `agent/moira-eval`. Both were considered for host-side
  cycle counting so the data path could be optimised without booting an
  emulator. The verdict is narrow and worth keeping precise.

  **Moira's 68000 instruction timing is exact, in a configuration it does not
  ship.** `MoiraConfig.h` defaults `MOIRA_PRECISE_TIMING` to false, which makes
  `SYNC(x)` a no-op in `MoiraMacros.h`, so data-dependent costs are computed and
  discarded -- MULS.W is charged its worst case 54 whatever the operand.
  `MOIRA_MIMIC_MUSASHI` defaults true and its own comment says to turn it off
  for accuracy. Both are unconditional `#define`s, not runtime options. Patched,
  it matches published M68000PRM figures 30/30, including MOVEM.L (An)+ at
  12+8n across six register counts. Unpatched, 29/30.

  **Its 68020 has no instruction cache at all**, measured rather than assumed:
  the same loop body from 64 to 640 bytes costs exactly 8.000 cycles per pair at
  every size, with the only decline being the fixed `dbf` amortising. There is
  no turn at 256 bytes, soft or otherwise, and FS-UAE's 020 does show one. So it
  cannot reproduce the I-cache behaviour that both of the 2026-08-01 data-path
  optimisations were tuned against, and **must not be used to choose unroll
  depth**.

  Against real measurements it does not reconcile and the errors do not share a
  sign: 68020 copy +23%, 68000 checksum at 20 B -41%, and the copy's alignment
  penalty comes out +1.2% where the machine gives +31%. The reason is
  structural -- **Moira is a CPU, not a machine.** A harness gives it flat
  always-ready RAM, so there is no chip-RAM contention, no prefetch overlap, no
  cache and no unaligned-access penalty, which is the very effect
  `n68k_copy.S` aligns its destination to avoid. Supplying all that means
  writing the Amiga.

  **Musashi is worse for this, not better.** Its 68020, 68030 and 68040 share
  one cycle table, verified by dumping it, and that table is the 020 best case
  -- i.e. a permanent 100% instruction-cache hit. `USE_ALL_CYCLES()` also
  charges a whole timeslice to a spin loop, which would invent a hotspot on
  exactly the busy-waits a network stack does.

  **Nobody has a cycle model above the 68020.** WinUAE's own 020+ cores have
  their cycle accumulation `#if 0`'d out in `gencpu.cpp`, the timing tables
  surviving only as comments, and Toni Wilen writes "cycle-exact" in scare
  quotes for 030/040/060 in his own source. His `cputester` validates TIMING
  only on 68000/68010 at +/-2 cycles and is a functional oracle above that.
  This is a field-wide gap rather than a Moira shortcoming, and it means the
  68060 machines our fastest users run cannot be measured by anyone -- so
  `movem.l`'s cheapness and the flat two-accumulator result stay 68020-only
  evidence permanently.

  What Moira IS good for is attribution, and that stays open rather than
  rejected: see the entry above.

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
