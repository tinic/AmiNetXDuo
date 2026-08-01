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

Nothing. The last one, publishing `NetStackQuery` / `NetStackControl`, was
decided and shipped in 0.16.2; what its constants are frozen at is in
`docs/NDK-ADDENDUM.md` with the rest of the ABI.

## Decided against — do not "fix"

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
