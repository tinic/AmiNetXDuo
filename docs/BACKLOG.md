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
`LC_ALL=C grep -a`. This entry's own RFC 3542 assessment was built on one of
those empty results and was wrong for a day.

---

## Open — no decision taken

- **Publish `NetStackQuery` / `NetStackControl`?** They are still private, at
  -0x366/-0x36c. They are what a third-party `netstat` would need, and
  `ShowNetStatus`, `netstat` and `arp` already depend on them being stable, so
  the recommendation is to publish. Not done because publishing freezes
  `NetStatusHeader` and every `NETCTRL_*` request struct, which is the owner's
  call. Two lines when decided: `netstatus.h` into `PUBLIC_HEADERS` in
  `tools/stage-developer.sh`, and an `AMI_NETSTATUS_MIN_REVISION` beside the
  existing magic.

## Decided against — do not "fix"

- **RFC 3542, the decisions it fixed**, 2026-07-31. The subset worth having is
  built (`src/bsdsocket/cmsg.c`, `include/aminetxduo/cmsg.h`); what follows is
  the part that is permanent, not the part that is done.
  - **`CMSG_ALIGN` is 4 bytes.** Every 32-bit BSD used it and the NDK's own
    `struct cmsghdr` is already the 12-byte shape it implies. Every ancillary
    buffer any caller ever builds depends on this; it does not move.
  - **The NDK's `CMSG_NXTHDR` and `CMSG_FIRSTHDR` are unusable and are
    replaced.** `CMSG_NXTHDR` expands to an `ALIGN()` no NDK header defines, so
    a translation unit that uses it does not compile; `CMSG_FIRSTHDR` returns
    `msg_control` without testing `msg_controllen`, which RFC 3542 §20.3.1
    names. `CMSG_LEN` and `CMSG_SPACE` are genuinely absent. All four come from
    `aminetxduo/cmsg.h`; do not reach for the NDK's.
  - **Both the BSD and the Linux option numbers are accepted, and a reply uses
    whichever the caller enabled with** -- enabling with 36 gives `cmsg_type`
    46, enabling with 49 gives 50. Same terms as `IPV6_V6ONLY`.
  - **`IP_PKTINFO` takes 8**, which is `IP_RETOPTS` in this NDK -- a 4.3BSD
    option no AmigaOS stack implemented. The same trade `IPV6_TCLASS` made
    against `IPV6_PATHMTU`.
  - **A per-write source or hop limit on TCP is refused, permanently.** A
    stream's source is fixed when the SYN goes out; there is nothing per-write
    to name. Naming it at `connect()` was the real gap and is closed separately
    by `nxd_tcp_client_socket_source_connect()` in the fork.
  - **Loopback's index is its NetX slot + 1, named `lo0`**, and appears in the
    RFC 3493 trio only. `ObtainInterfaceList()`, `QueryInterfaceTagList()` and
    `SIOCGIFCONF` still do not list it: those are about SANA-II interfaces a
    caller can configure.
  - **Not implemented and not planned**: `IPV6_RTHDR`, `HOPOPTS`, `DSTOPTS`,
    `RTHDRDSTOPTS`, `PATHMTU`, `RECVPATHMTU`, `USE_MIN_MTU`, `DONTFRAG`,
    `NEXTHOP` -- extension headers and path-MTU state NetX Duo does not expose.

- **A browse reports the whole peer cache, not the browse window**, and stays
  that way, 2026-07-31. `netstack_mdns_browse_collect()` walks
  `nx_mdns_service_lookup()` by index, which is the whole peer cache, and
  nothing ages entries from our side -- the module expires them by TTL, and an
  unplugged machine sends none of RFC 6762 §10.1's goodbyes. That part is mDNS
  working as designed and needs no fix. The claim it made was wrong and is
  fixed: the output said "what answered in the window", true on a cold cache
  and false on a warm one, and now says what the machine has heard recently and
  that a listing may have gone. What is left is the behaviour -- filter to
  entries actually refreshed inside the window. `nx_mdns_rr_elapsed_time` and
  `nx_mdns_rr_remaining_ticks` carry the freshness, but `NX_MDNS_SERVICE` does
  not, so it means walking the RR cache instead of the public lookup. Not worth
  that trade until someone reports a switched-off machine lingering, which
  every other mDNS browser does too. Still true after the address chasing and
  `ALL` landed: neither of them needed an RR walk, so nothing is written that
  this would reuse.

- **RFC 3678 source filtering stays out**, 2026-07-31.
  `IP_ADD_SOURCE_MEMBERSHIP`, `IP_BLOCK_SOURCE` and the `MCAST_*` family need
  IGMPv3, which the vendored NetX Duo does not implement -- it speaks IGMPv2
  and the source lists have nowhere to go. RFC 1112 membership is what shipped
  (`src/bsdsocket/mcast.c`) and is what SSDP, UPnP and a ported mDNS actually
  call.

- **IPv4 multicast**, done 2026-07-31, entry kept for the cost.
  `IP_ADD_MEMBERSHIP`, `IP_DROP_MEMBERSHIP`, `IP_MULTICAST_IF`,
  `IP_MULTICAST_TTL` and `IP_MULTICAST_LOOP` over `nx_igmp_enable()`, in
  `src/bsdsocket/mcast.c`; `bind()` to a class D address is accepted, which it
  was not, because that is how an SSDP receiver is written. Measured: **3,888
  bytes** on the floor build (3,696 of code, 192 of membership table) and 3,532
  on the default one, plus 12 bytes per open socket. No packet-pool or `NX_IP`
  growth at all -- `nx_ipv4_multicast_entry[7]` is unconditional in `NX_IP` and
  `nx_igmp_enable()` only fills in three function pointers. On by default;
  `-DAMINETXDUO_MULTICAST=OFF` in the `68000-minimal` drawer, with the other
  four optional features.

- **RFC 6724 default address selection**, 2026-07-31: does not apply here. It
  sorts a list of candidate destinations, and `getaddrinfo()` returns at most
  one address per family (the resolver under it answers with a single address,
  not a set -- see `src/bsdsocket/addrinfo.c`). With two entries at most its
  rules collapse to "which family first", which is answered deliberately: IPv6
  then IPv4. It would start to matter only if the resolver ever returned
  address sets.

- **RFC 5952 IPv6 text representation**: conformant, 2026-07-31, and now
  pinned. §4.1 leading zeros, §4.2.1 maximum compression, §4.2.2 no `::` for a
  lone zero group, §4.2.3 first of equal runs, §4.3 lowercase, §5 embedded IPv4
  -- all verified in `src/config/test/test_config.c`. §4.2.3 was the one
  previously untested.

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

- **`tests/clients/run-argvexit.sh -A` times out, and the toolchain is not
  why.** An earlier version of this entry read `fix-crt0`'s "matched 0 site(s)"
  as the crt0 having moved out from under the script, and concluded that
  `--wrap=main` might not be reached. That was wrong: 0 sites is the immune
  case, which the script's own docstring says both defects reach once they are
  fixed upstream. `--check` against the pinned toolchain on playhouse2 and
  playhouse3 alike reports `11 ok, 1 skipped` for the frame skew and `2 call
  site(s) already push __argv by value` for the argv indirection, so a ported
  client gets a correct `argv` and `--wrap=main` is reached. The probe's
  timeout is the harness, and the `longjmp` fix in `__wrap__exit()` is still
  unverified by measurement. Found 2026-07-31.

- **`/opt/amiga` on playhouse2 carries the argv bug in all eleven `crt0.o`.**
  Locally built, GCC 16.1.1b, and `--check` reports `11 buggy` -- it has the
  compiler fix for the frame skew and not newlib's `120371e` for the argv
  declaration. Nothing releases through it, so this is a note about the box
  rather than about a build: anything built there against `-lc` without running
  `fix-toolchain-crt0.py` first hands ported clients `&__argv`. Found
  2026-07-31.

- **`tests/ipv6/ipv6_socket_test.c` runs green but is not in CI.** 129 checks,
  0 failures, on an emulated A1200 under Amiberry on 2026-07-31 -- the whole
  RFC 3542 surface end to end, including the checks that postdate the 119-check
  figure quoted elsewhere: `IP_PKTINFO` and `IP_RECVDSTADDR` arriving on one
  datagram, `ipi_spec_dst`, both hop-limit paths read off the wire, and the
  arrival index through `if_indextoname()` and back. It is tier 2, so `ci.sh`
  builds it and does not run it; it needs a Kickstart ROM, which only the lab
  machine has. Run it with
  `. ~/amiga-assets/env.sh && ./tests/ipv6/run-socket-fsuae.sh -A` on
  playhouse3 -- FS-UAE cannot boot headless there, which is why the harness
  gained `-A`. The test writes its results to the **serial log**, not to the
  guest's `stdout.txt`; that file holds unrelated bytes and reading it looks
  like a crash.
- **The two-interface source case is proved on a host, not on a guest.** TCP
  now leaves from the address `bind()` named --
  `nxd_tcp_client_socket_source_connect()` in the NetX fork -- and the case it
  exists for is two interfaces on one subnet, source on one and route out of
  the other. That is asserted by
  `tests/netstack/host/test_tcp_source_connect_host.c`, which compiles the real
  connect, route lookup and SYN build against an `NX_IP` with two
  `nx_ip_interface[]` entries filled in and checks the source address the SYN
  carried. What is not shown is the same thing through two real SANA-II
  drivers. Amiberry does put two cards in the machine -- `a2065` plus
  `ariadne`, both logged and mapped into Zorro II -- but with the second card
  present `AddNetInterface eth0` hangs on the A2065, which comes up on its own
  in the same tree on the same run script, and the serial log is empty. It
  hangs with the second card on SLIRP as well as bridged, so it is the board
  and not the backend. The 8 MB of Zorro II Fast RAM sitting directly below
  both cards is the first suspect (RESEARCH.md 85, 76). `SrcProbe` already
  takes the second address and a destination for when this is fixed.

- **The `sana2_rx.c` reader orphan does not reproduce under emulation.**
  `ami_sana2_rx_stop()`'s last-resort path logs `reader N did not stop; leaking
  its stack` and leaks 32 KB when a SANA-II driver ignores `AbortIO`, which
  Commodore's a2065.device 2.16 is documented to do. Thirteen full teardowns
  under Amiberry with that driver logged it zero times, so the emulated card
  returns its queued `CMD_READ`s where the real one may not.
  `run-cycledrill.sh` greps the serial log for it on every run and prints the
  count; `AMINETXDUO_CYCLE_ORPHAN_FATAL=1` makes it fail. Confirming it needs
  real hardware. Checked 2026-07-31.

- **`STATE=down` has no harness.** It is honoured as of 2026-07-31 but only the
  config parser is covered; no emulator run boots an interface configured down.

- **`playhouse2` had uncomputed TX checksums**; `ethtool -K eth0 tx off` was
  applied 2026-07-31 and verified (`/usr/sbin/ethtool -k eth0` ->
  `tx-checksumming: off`; `/usr/sbin` is not on a non-login ssh PATH, so query
  it by full path). Same defect playhouse4 had. `run-fitzbench.sh` still
  refuses it outright and can be relaxed.

- **`run-fitzbench.sh` prints a write figure that is not a rate** — it stops
  timing when the write call returns, not when data drains. Guest-timed 1718
  KB/s against a measured wire rate of 364. Reads agree between clocks; writes
  diverge ~4.7x.

- **cppcheck**: baseline is from 2.20.0, gate hosts have 2.17.1, so the stage
  skips itself. Install 2.20.0 or regenerate to make it gate again.

- **FS-UAE cannot boot headless on playhouse3** (`FATAL: [GLAD] …`). Harnesses
  take `-A` to use Amiberry instead, and `-a ARGS` to pass arguments.
