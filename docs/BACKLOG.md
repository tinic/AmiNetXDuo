# Backlog

What is outstanding, what was decided against, and why. Findings come mostly from
the NDK 3.2 autodoc audit (four passes over all 122 documented entries) and from
the memory-floor and stress work.

The autodoc is at `NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc`. **`grep` silently
fails on it** — `file` misidentifies it as "GTA in-game text". Read it with python.

---

## Open — no decision taken

- **Three nx_secure over-reads: FIXED 2026-07-31 on the fork.**
  `_nx_secure_x509_asn1_tlv_block_parse()` loaded the ASN.1 tag before the
  length check; `_nx_secure_tls_process_serverhello()` read the ciphersuite and
  compression method having bounded only the session ID; and
  `_nx_secure_tls_process_certificate_request()` read the certificate-type
  count with the only guard sitting inside the TLS 1.3 arm. All three were
  reachable from the wire, all found by `fuzz_tls_record` / `fuzz_tls_x509` at
  zero slop, all confirmed under ASan.

  Fixed on `tinic/netxduo` branch `amiga-nx-secure-bounds`, merged to
  `amiga-integration` as `a1036f03`, submodule bumped. Both drivers now run at
  `FR_KNOWN_SLOP` / `FX_KNOWN_SLOP` 0 -- 200k mutations each, clean -- which is
  the proof, since they needed 3 and 1 bytes of padding to tolerate the reads
  before. Commits are unsigned; DCO is the author's to add before submission.
- **TLS parsers that need crypto have no fuzz driver.** `fuzz_tls_record` and
  `fuzz_tls_x509` cover the record header, the handshake header, ServerHello
  and its extensions, CertificateRequest, the Certificate message and the
  X.509 DER walk. ServerKeyExchange, CertificateVerify and Finished are not
  covered: all three dispatch on a negotiated ciphersuite into
  `NX_CRYPTO_METHOD` entries, so a driver has to link `nx_crypto` and
  `ami_tls_crypto.c` -- and `ami_tls_crypto.c` includes `exec/types.h` and
  casts pointers to a 32-bit `ULONG`, so it needs a 32-bit host build the way
  `fuzz_mdns` does. Neither is `tls_resume_take_ticket()`, which is what parses
  a TLS 1.2 NewSessionTicket (nx_secure only parses one under TLS 1.3, which is
  off), nor `tls_store.c`'s issuer-name walk; both live in files that include
  `proto/dos.h` and do not build on a host. The header comment in
  `tests/fuzz/CMakeLists.txt` is the current list.
- **No open/expunge/reopen drill.** The soak suite covers steady state; what
  historically kills long-lived Amiga stacks is cycling -- Online/Offline
  bounces and library expunge/reopen. `sana2_rx.c`'s last-resort path still
  leaks a reader stack when a driver ignores `AbortIO` (Commodore's
  a2065.device 2.16 does), which emulation will not reproduce. Raised in
  external review 2026-07-31.
- **TCP cannot be made to send from a bound address, only checked.** The rest
  of `bind()` source selection is done: `bsd_source_select()` (socket.c) maps a
  bound address or an RFC 4007 zone to the index `nxd_udp_socket_source_send()`
  and `nxd_ip_raw_packet_source_send()` take, so UDP and raw leave from the
  address that was asked for. TCP has no such call --
  `_nxd_tcp_client_socket_connect()` runs its own route lookup with no hint and
  sends the SYN before it returns -- so `connect()` runs the same two lookups
  first and refuses (`EADDRNOTAVAIL`) when the answer is not the bound source.
  What that costs is the case BSD allows and we do not: source on one
  interface, route out of the other. Closing it means a
  `nxd_tcp_socket_source_send()` in the NetX fork, which is a submodule change
  and a fourth upstream PR.

  What a one-interface guest can show is shown, by `tests/tools/run-srcsel.sh`
  (13 checks, green under Amiberry on SLIRP): the bound address really is the
  source the receiver sees, on the interface and on loopback, and both
  refusals fire. What it cannot show is the disagreement this is about --
  source on one interface, route out of the other -- and one arm of the TCP
  check goes with it: `EADDRNOTAVAIL`, for a bound address that *can* reach the
  destination while the stack would still leave by somewhere else, needs two
  interfaces on one subnet to produce. Reasoned about, not measured.
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
- **`bind()` outbound source selection is not done.** Inbound is: a completed
  TCP connection that arrived on another interface is reset in `bsd_accept()`,
  and a datagram that did is released in `bsd_recv_udp()`, both through
  `bsd_bind_wants_interface()`, so `nc -l 127.0.0.1` means what it says and a
  specific address is no longer refused. What is left is the send direction --
  a socket bound to one address should send *from* it, and today NetX picks by
  route. `nxd_udp_socket_source_send()` is already wired for RFC 4007 zones
  and takes the same kind of index, so UDP is small; TCP has no source-send
  equivalent and needs a decision, the same one RFC 4007 needs.
- **IPv6 group membership (`IPV6_JOIN_GROUP`) is absent.** The IPv4 side is
  done (below); this is not, and is a separate decision because the numbers are
  worse. `NX_ENABLE_IPV6_MULTICAST` grows every `NX_IP` by 172 bytes whether or
  not anything joins, where `nx_igmp_enable()` grows it by nothing, and there is
  no MLD anywhere in the vendored tree -- no `nx_mld_*.c` exists, so a join
  reaches the driver and no report is ever sent, and a querying switch stops
  forwarding the group. The reasoning is beside the define in
  `port/netxduo-amiga/inc/nx_user.h`.
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
  Still true after the address chasing and `ALL` landed: neither of them needed
  an RR walk, so nothing is written that this would reuse.

- **RFC 4007 §11 is done for the socket API; the config keys are not.**
  Done: the text layer (`ami_config_parse_ip6_zone` /
  `ami_config_format_ip6_zone`, covered in `test_config.c`), `getnameinfo()`
  printing a zone for link-local, `getaddrinfo()` accepting one as a number or
  an interface name, and `send`/`sendto`/`sendmsg` honouring it on UDP and raw
  alike -- `bsd_source_select()` maps the interface index to the
  `nx_ipv6_address[]` index the `source_send()` calls want. `connect()` on TCP
  checks the zone against the route and refuses rather than ignoring it; see
  the TCP entry above for why it cannot do better.

  Left:
  - `DEVS:NetInterfaces` keys (`GATEWAY6`, `ADDRESS6`) still call the plain
    parser, so a zoned value there is a clean refusal.
  - No emulator coverage for the zone itself: verified only by the host parser
    tests. It wants two interfaces to be meaningful, and the lab guest has one.
    `tests/tools/run-srcsel.sh` covers the IPv4 half of the same machinery.
- **Ship a Developer drawer: the NDK addendum.** ACCEPTED 2026-07-31. The
  archive ships no headers, so nothing we add past the NDK's 0..143 range can
  be reached by anyone else's code. Plan and the three permanent ABI decisions
  (LVO slots, `CMSG_ALIGN` for m68k, `IPV6_*` option numbers) are in
  `docs/NDK-ADDENDUM.md`. The four RFC 3493 vectors exist as of revision 3 and
  are verified on the guest, but nothing outside this tree can reach them until
  the drawer ships -- that is what this item is.
- **Ship a Developer drawer: the NDK addendum.** SHIPPED 2026-07-31 for RFC
  3493 section 4. `developer/` holds the SFD and the generated
  clib/inline/proto/pragmas/lvo set; `tools/stage-developer.sh` assembles the
  drawer and both `dist/make-dist.sh` and the CMake build call it, so the
  archive's copy is the one `tests/tools`' `IfNames` was compiled against --
  against the staged drawer alone, with no path into `include/`.
  `tools/gen-developer.sh --check` runs in `ci.sh`'s cross stage wherever the
  toolchain has an `sfdc`.

  Widened the same day to carry every definition we make that the NDK lacks,
  not only the new vectors. `include/aminetxduo/in6.h` is the second published
  header: `IPPROTO_IPV6`, `PF_INET6`, `INET6_ADDRSTRLEN`, the three `IPV6_*`
  options, `IN6ADDR_*_INIT`, the `IN6_IS_ADDR_*` macros,
  `struct sockaddr_storage`, `AI_ADDRCONFIG`, and the `sockaddr_in6` offset
  trap written out for callers. `bsdsocket_internal.h` includes it and aliases
  its `AMI_IPV6_*_BSD` names to it, so there is one copy of each number.
  `AI_V4MAPPED` is deliberately left undefined and `sockaddr_storage`
  deliberately has no `ss_family`; `docs/NDK-ADDENDUM.md` has both reasons.

  Left:
  - **RFC 3542** is not in it. It adds no vectors, so it lands as another
    header beside `aminetxduo/ifindex.h` and the drawer's shape does not
    change; `developer/sfd/aminetxduo_lib.sfd` carries the marker saying so.
  - **IPv6 multicast** (`IPV6_JOIN_GROUP`, `IPV6_LEAVE_GROUP`,
    `struct ipv6_mreq`) is absent from the NDK and would belong in `in6.h`.
    IPv4 multicast needs nothing: the NDK has the whole set.
  - **`NetStackQuery`/`NetStackControl` are still private.** Recommendation:
    publish them, because they are what a third-party `netstat` needs and
    `ShowNetStatus`, `netstat` and `arp` already depend on them being stable.
    Not done here: publishing freezes `NetStatusHeader` and every
    `NETCTRL_*` request struct, and that is the owner's call. Adding them is
    two lines -- `netstatus.h` to `PUBLIC_HEADERS` in
    `tools/stage-developer.sh`, and an `AMI_NETSTATUS_MIN_REVISION` beside
    the existing magic.
  - No `.info` for the drawer's own contents beyond `ReadMe.info`; the
    headers are for a cross-compiler, not for Workbench.
- **RFC 3542 (Advanced Sockets API for IPv6) is absent.** Assessed 2026-07-31.
  Feasible without patching NetX Duo, and cheaper than the `if_*` four because
  it needs **no new LVOs** -- it rides `sendmsg`/`recvmsg`, which exist, and
  `struct msghdr` is already the 28-byte 4.4BSD shape with `msg_control` at
  offset 16.

  What has to be invented is *header* ABI, but LESS of it than this entry first
  said. Re-audited 2026-07-31 against `ndk-include` with `LC_ALL=C grep -a` --
  a plain `grep -r` reads those headers as binary, because they are Latin-1
  and carry a `©`, and silently finds nothing:

  - **Already in `<sys/socket.h>`:** `struct cmsghdr` (12 bytes:
    `socklen_t` + `LONG` + `LONG`), `CMSG_DATA`, `CMSG_FIRSTHDR`,
    `CMSG_NXTHDR`. Do not define a second `struct cmsghdr`.
  - **Missing:** `CMSG_LEN`, `CMSG_SPACE`, `CMSG_ALIGN`, and the bare
    `ALIGN()` that the NDK's own `CMSG_NXTHDR` expands to and that nothing in
    the NDK defines -- so `CMSG_NXTHDR` as shipped does not compile.

  `CMSG_ALIGN` is still ours to fix and every later caller is stuck with it.
  **Decided 2026-07-31: 4 bytes.** It is what every 32-bit BSD used, nothing
  about m68k argues for more, and it agrees with the `struct cmsghdr` the NDK
  already has -- 12 bytes needs no padding at 4 and would gain 4 wasted bytes
  at 8.

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

- **RFC 3678 source filtering stays out**, 2026-07-31. `IP_ADD_SOURCE_MEMBERSHIP`,
  `IP_BLOCK_SOURCE` and the `MCAST_*` family need IGMPv3, which the vendored NetX
  Duo does not implement -- it speaks IGMPv2 and the source lists have nowhere to
  go. RFC 1112 membership is what shipped (`src/bsdsocket/mcast.c`) and is what
  SSDP, UPnP and a ported mDNS actually call.
- **IPv4 multicast**, done 2026-07-31, entry kept for the cost. `IP_ADD_MEMBERSHIP`,
  `IP_DROP_MEMBERSHIP`, `IP_MULTICAST_IF`, `IP_MULTICAST_TTL` and
  `IP_MULTICAST_LOOP` over `nx_igmp_enable()`, in `src/bsdsocket/mcast.c`;
  `bind()` to a class D address is accepted, which it was not, because that is how
  an SSDP receiver is written. Measured: **3,888 bytes** on the floor build (3,696
  of code, 192 of membership table) and 3,532 on the default one, plus 12 bytes per
  open socket. No packet-pool or `NX_IP` growth at all -- `nx_ipv4_multicast_entry[7]`
  is unconditional in `NX_IP` and `nx_igmp_enable()` only fills in three function
  pointers. On by default; `-DAMINETXDUO_MULTICAST=OFF` in the `68000-minimal`
  drawer, with the other four optional features.
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
