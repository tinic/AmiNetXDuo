# Backlog

What is outstanding, what was decided against, and why. Findings come mostly from
the NDK 3.2 autodoc audit (four passes over all 122 documented entries) and from
the memory-floor and stress work.

The autodoc is at `NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc`. **`grep` silently
fails on it** — `file` misidentifies it as "GTA in-game text". Read it with python.

---

## Open — no decision taken

- **DHCP and TLS have no fuzz driver.** `tests/fuzz/` covers bpf, config, dns
  and mdns; the two parsers an attacker reaches most directly are missing.
  DHCP option parsing runs on every boot against whatever the LAN answers, and
  the TLS record layer and X.509 path are the headline feature. With no MMU a
  parser bug is not a crashed process, it is arbitrary code execution with the
  machine's privileges. The harness pattern already exists, so this is cheap
  relative to what it covers. Raised in external review 2026-07-31.
- **No open/expunge/reopen drill.** The soak suite covers steady state; what
  historically kills long-lived Amiga stacks is cycling -- Online/Offline
  bounces and library expunge/reopen. `sana2_rx.c`'s last-resort path still
  leaks a reader stack when a driver ignores `AbortIO` (Commodore's
  a2065.device 2.16 does), which emulation will not reproduce. Raised in
  external review 2026-07-31.
- **`bind()` to a specific local address is a silent no-op.** `socket.c` records
  it in `as_LocalAddr` so `getsockname()` reports it, and nothing enforces it --
  the comment there has said so for a long time, but it never reached this file.
  This is the one place the codebase breaks its own refuse-don't-ignore rule,
  and the worst shape of it: a tool binding a listener to 127.0.0.1 gets an
  all-interfaces listener while `getsockname()` confirms the lie. It also
  silently defeats every `-b` / `--interface` / `-s` flag (curl, nc, dig, ntp).

  NetX has no address-taking bind, so enforcement is ours to build, and the two
  directions differ:
  - **Outbound is already possible.** `nxd_udp_socket_source_send()` is wired
    into `transfer.c` as of 2026-07-31 for RFC 4007 zones; honouring a bound
    source address on UDP send is the same call with the index taken from
    `as_LocalAddr` instead of the zone. Cheap now.
  - **Inbound needs a filter**: check the arrival address in the UDP receive
    notify against `as_LocalAddr` and drop mismatches; for TCP, check the
    accepted connection's local address and reset mismatches.
  - **Interim, if enforcement lags**: refuse with `EADDRNOTAVAIL` rather than
    lie. Accept ANY, loopback, and -- wider than the obvious rule -- the
    machine's own interface address when it has one interface, since that is
    unambiguous and is what an `--interface` flag usually resolves to.

  A listener that claims loopback and answers the LAN is a security lie, so
  that direction should fail loudly even before the filter exists.
- **IPv4 multicast is absent** -- no `IP_ADD_MEMBERSHIP`, `IP_MULTICAST_IF`,
  `IP_MULTICAST_TTL`, `IP_MULTICAST_LOOP`, no `ip_mreq`. Reopened 2026-07-31:
  it had been closed on the grounds that "nothing in the tree needs the
  socket-level API", which is the wrong test -- the point of the project is
  running other people's tools, and SSDP/UPnP and any ported mDNS open their
  own multicast sockets. The vendored NetX mDNS covers `.local` for us and
  covers nothing for them.

  Not purely exposure work: `nx_igmp_enable()` is never called, so IGMP has to
  be turned on first, which is a memory cost the 1 MB floor tier has to be
  weighed against. RFC 1112 membership is the target; RFC 3678 source filtering
  is not, and can wait indefinitely. `NX_ENABLE_IPV6_MULTICAST` is the same
  argument on the v6 side, for a non-floor tier.
- **`ShowNetServices` cannot browse every type at once.** With no type it runs the
  RFC 6763 §9 meta-query and lists the types present; listing every instance of
  every type would mean starting one continuous query per type found. They would
  all run concurrently, so it costs one more window rather than one per type — but
  it multiplies what lands in the peer cache, and the cache size was already the
  thing that decided whether an answer had an address in it or not.
- **A browse answer with no address is not chased.** When the PTR and SRV arrive
  without the A record in the same response, the row prints the target host and
  "no address" — the vendored module fills `service_ipv4` only from an A record
  already in the cache and never asks for one. Raising the peer cache to 32 KB
  made it rare on the LAN it was measured against, which is a mitigation rather
  than a fix: the right answer is to resolve the SRV target when the A is absent.
- **A browse reports the whole peer cache, not the browse window.**
  `netstack_mdns_browse_collect()` walks `nx_mdns_service_lookup()` by index,
  which is the whole peer cache, and nothing ages entries from our side -- the
  module expires them by TTL, and an unplugged machine sends none of RFC 6762
  §10.1's goodbyes. That part is mDNS working as designed and needs no fix.
  The claim it made was wrong and is fixed: the output said "what answered in
  the window", true on a cold cache and false on a warm one, and now says what
  the machine has heard recently and that a listing may have gone. What is left
  is the behaviour -- filter to entries actually refreshed inside the window.
  `nx_mdns_rr_elapsed_time` and `nx_mdns_rr_remaining_ticks` carry the
  freshness, but `NX_MDNS_SERVICE` does not, so it means walking the RR cache
  instead of the public lookup. Not worth that trade until someone reports a
  switched-off machine lingering, which every other mDNS browser does too.

- **RFC 4007 §11 is done for UDP; TCP and the config keys are not.**
  Done: the text layer (`ami_config_parse_ip6_zone` /
  `ami_config_format_ip6_zone`, covered in `test_config.c`), `getnameinfo()`
  printing a zone for link-local, `getaddrinfo()` accepting one as a number or
  an interface name, and `send`/`sendto`/`sendmsg` honouring it --
  `bsd_ip6_zone_source()` maps the interface index to the `nx_ipv6_address[]`
  index `nxd_udp_socket_source_send()` wants.

  Left:
  - **TCP.** There is no `nxd_tcp_socket_source_send()`, so `connect()` to a
    zoned link-local address stores `as_ScopeId` and then routes normally.
    Decide what it should do -- refuse, or bind the source address first --
    before claiming TCP support.
  - **Raw** (`bsd_send_raw`) ignores the zone the same way.
  - `DEVS:NetInterfaces` keys (`GATEWAY6`, `ADDRESS6`) still call the plain
    parser, so a zoned value there is a clean refusal.
  - No emulator coverage: the UDP path is verified only by the host parser
    tests. It wants two interfaces to be meaningful, and the lab guest has one.
- **Ship a Developer drawer: the NDK addendum.** ACCEPTED 2026-07-31. The
  archive ships no headers, so nothing we add past the NDK's 0..143 range can
  be reached by anyone else's code. Plan and the three permanent ABI decisions
  (LVO slots, `CMSG_ALIGN` for m68k, `IPV6_*` option numbers) are in
  `docs/NDK-ADDENDUM.md`. The four RFC 3493 vectors exist as of revision 3 and
  are verified on the guest, but nothing outside this tree can reach them until
  the drawer ships -- that is what this item is.
- **RFC 3542 (Advanced Sockets API for IPv6) is absent.** Assessed 2026-07-31.
  Feasible without patching NetX Duo, and cheaper than the `if_*` four because
  it needs **no new LVOs** -- it rides `sendmsg`/`recvmsg`, which exist, and
  `struct msghdr` is already the 28-byte 4.4BSD shape with `msg_control` at
  offset 16.

  What has to be invented is *header* ABI: `struct cmsghdr` and
  `CMSG_FIRSTHDR`/`NXTHDR`/`DATA`/`LEN`/`SPACE` are **not in the NDK at all**.
  Defining them means fixing `CMSG_ALIGN` for m68k, and every later caller is
  stuck with whatever we pick. **Decided 2026-07-31: 4 bytes.** It is what every
  32-bit BSD used, it keeps `struct cmsghdr` at 12 bytes, and nothing about m68k
  argues for more -- wider alignment would only waste buffer space.

  Feasibility checked: `NX_PACKET` carries `nx_packet_ip_interface` (the arrival
  interface) and `nx_packet_ip_header` (from which the hop limit reads), so both
  options worth having can be filled from what NetX already hands us.

  Worth implementing, in this order:
  - `IPV6_RECVPKTINFO` / `IPV6_PKTINFO` (`struct in6_pktinfo`: `ipi6_addr`,
    `ipi6_ifindex`) -- which interface and local address a datagram arrived on,
    and which to send from. Lets a responder answer on the interface a query
    came in on; starts to matter at two interfaces.
  - `IPV6_RECVHOPLIMIT` / `IPV6_HOPLIMIT` -- `traceroute6`, and an mDNS
    responder checking the arriving hop limit (RFC 6762's source-address check;
    confirm the exact requirement before relying on it).
  - `ICMP6_FILTER` with `struct icmp6_filter` and its six macros (RFC 3542
    §3.2) -- `ping6`/`traceroute6` filtering ICMPv6 by type on a raw socket.

  **Do the IPv4 half in the same stroke**: `IP_PKTINFO` / `IP_RECVDSTADDR` is
  the same cmsg plumbing, and it is what the dnsmasq / unbound / tftpd class of
  UDP server actually requires -- a server that cannot tell which of its own
  addresses a query arrived on answers from the wrong one. Accept both the
  FreeBSD and Linux spellings, as `AMI_IPV6_V6ONLY_BSD`/`_LINUX` already do.

  Not worth it: `IPV6_RTHDR`, `HOPOPTS`, `DSTOPTS`, `RTHDRDSTOPTS`, `PATHMTU`,
  `RECVPATHMTU`, `USE_MIN_MTU`, `DONTFRAG`, `TCLASS`, `NEXTHOP`.

  `in6.c` refuses these options because `recvmsg()` always reports
  `msg_controllen == 0`, which stays correct until this is built. The
  `transfer.c` comment that justified it reasoned about SCM_RIGHTS -- the wrong
  RFC -- and was corrected in 625e5df.

## Decided against — do not "fix"

- **RFC 6724 default address selection**, 2026-07-31: does not apply here. It
  sorts a list of candidate destinations, and `getaddrinfo()` returns at most
  one address per family (the resolver under it answers with a single address,
  not a set -- see `src/bsdsocket/addrinfo.c`). With two entries at most its
  rules collapse to "which family first", which is answered deliberately:
  IPv6 then IPv4. It would start to matter only if the resolver ever returned
  address sets.
- **RFC 5952 IPv6 text representation**: conformant, 2026-07-31, and now pinned.
  §4.1 leading zeros, §4.2.1 maximum compression, §4.2.2 no `::` for a lone zero
  group, §4.2.3 first of equal runs, §4.3 lowercase, §5 embedded IPv4 -- all
  verified in `src/config/test/test_config.c`. §4.2.3 was the one previously
  untested.

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
  machine holding the address to test against. The result code is downstream
  of that, and inventing one without the probe would be a worse answer than
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
- **`MSG_OOB` refused by the msghdr forms**; `MSG_DONTROUTE` accepted and ignored.
- **`listen()` backlog is 8**, where the doc's BUGS claims a silent limit of 5.
- **`Dup2Socket` allows any in-range slot** — the doc's `EBADF` wording would make
  `dup2` onto a free slot illegal, defeating the call.
- **Request-count defaults** (ours sized from the packet pool, not 32/32/4).
- **`IP_DEFAULT_TTL` is 128**, doc says 64. Deliberate, 2026-07-31.
- **`gai_strerror()` takes its argument in A0.** The autodoc says D0; the SFD and
  pragmas say A0, and callers link against the pragma. The doc is wrong.
- **`bpf_open()` returns 0 for channel 0**, against the doc's *"handle > 0"* —
  RESEARCH §60 decoded Roadshow's own libpcap using it as a 0-based channel.
- **`OpenLibrary` allows Tasks and non-opener Processes**, which the doc denies.
  More permissive, never less.
- **`GetDefaultDomainName()` refuses rather than truncates.**
- **Packet pool sizing left alone**, 2026-07-31 — the heuristic reaches the floor
  on small machines (1 free of 17 observed on 1 MB), so the minimum must stay.
- **No stripped drawer beyond `68000-minimal`**, 2026-07-31 — the full build is
  what generates useful bug reports.
- **Two physical NICs**, dropped 2026-07-31: `romtype_restricted()` keeps only the
  first card, so it is untestable, and the complexity is not worth a rare case.
  Recovery SHA `d22a33e`.

- **A Kickstart 1.3 build**, dropped 2026-07-31: TheWire13 already covers that
  platform. Our side was closer than expected -- tag walking is hand-rolled so
  `utility.library` is never needed, the library's DOS surface is all V33, and
  the 68000 build ships -- but `ReadEClock` is V36 and everything 0.14.0 did for
  clock correctness rests on it; a 1.3 fallback is the 50 Hz CIA/VBlank source,
  exactly one tick of resolution, which retires the timer budget. The tools are
  the volume (`ReadArgs` 27 sites, `FreeArgs` 191, `VPrintf` 12). Never
  established whether any SANA-II driver runs under V34 at all, which was the
  gating question.

## Environment and tooling

- **`STATE=down` has no harness.** It is honoured as of 2026-07-31 but only the
  config parser is covered; no emulator run boots an interface configured down.

- **`playhouse2` had uncomputed TX checksums**; `ethtool -K eth0 tx off` was applied
  2026-07-31 and verified (`/usr/sbin/ethtool -k eth0` -> `tx-checksumming: off`;
  `/usr/sbin` is not on a non-login ssh PATH, so query it by full path). Same defect
  playhouse4 had. `run-fitzbench.sh` still refuses it outright and can be relaxed.
- **`run-fitzbench.sh` prints a write figure that is not a rate** — it stops timing
  when the write call returns, not when data drains. Guest-timed 1718 KB/s against
  a measured wire rate of 364. Reads agree between clocks; writes diverge ~4.7x.
- **cppcheck**: baseline is from 2.20.0, gate hosts have 2.17.1, so the stage skips
  itself. Install 2.20.0 or regenerate to make it gate again.
- **FS-UAE cannot boot headless on playhouse3** (`FATAL: [GLAD] …`). Harnesses take
  `-A` to use Amiberry instead, and `-a ARGS` to pass arguments.
