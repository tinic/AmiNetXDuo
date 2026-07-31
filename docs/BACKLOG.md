# Backlog

What is outstanding, what was decided against, and why. Findings come mostly from
the NDK 3.2 autodoc audit (four passes over all 122 documented entries) and from
the memory-floor and stress work.

The autodoc is at `NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc`. **`grep` silently
fails on it** — `file` misidentifies it as "GTA in-game text". Read it with python.

**The NDK headers have the same trap.** `m68k-amigaos/ndk-include` is Latin-1 and
carries a `©`, so a plain `grep -r` reads those files as binary and finds nothing
— an empty result there means "not read", not "not present". Use `LC_ALL=C grep -a`.
This entry's own RFC 3542 assessment was built on one of those empty results and
was wrong for a day.

---

## Open — no decision taken

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
- **An expunge/reopen cycle loses about 12.6 KB.** Found by the drill below:
  eight `OpenLibrary` / last-`CloseLibrary` / `ACTION_DIE` to `TCP:` /
  `RemLibrary` / reopen cycles lose 12,612 bytes each, dead linear, measured
  at the same fully-settled instant of each cycle. Plain open and close leaks
  nothing at all -- twenty-four nested pairs per cycle move `AvailMem()` by
  zero -- so it is the expunge-and-reload path specifically and not the
  library's ordinary lifetime. `nsl_AllocLive` reads 21 at every reopen and
  `NETSTATUS_HEALTH` shows no growth anywhere, so it is *not* going through
  `ami_alloc()`: it is raw `AllocMem`, a `CreateNewProc` stack, or something
  DOS holds. No Task or Process is left behind (the drill counts them), which
  rules out the most alarming explanation. Not yet attributed.
  `tests/tools/run-cycledrill.sh` gates it as a regression budget against the
  recorded figure, so it catches the leak getting worse and not the leak.
  Found 2026-07-31 on Amiberry/A1200/a2065-on-SLIRP.
- **The `sana2_rx.c` reader orphan does not reproduce under emulation.**
  `ami_sana2_rx_stop()`'s last-resort path logs `reader N did not stop;
  leaking its stack` and leaks 32 KB when a SANA-II driver ignores `AbortIO`,
  which Commodore's a2065.device 2.16 is documented to do. Thirteen full
  teardowns under Amiberry with that driver logged it zero times, so the
  emulated card returns its queued `CMD_READ`s where the real one may not.
  `run-cycledrill.sh` greps the serial log for it on every run and prints the
  count; `AMINETXDUO_CYCLE_ORPHAN_FATAL=1` makes it fail. Confirming it needs
  real hardware. Checked 2026-07-31.
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
- **IPv6 group membership (`IPV6_JOIN_GROUP`) is absent.** The IPv4 side is
  done (below); this is not, and is a separate decision because the numbers are
  worse. `NX_ENABLE_IPV6_MULTICAST` grows every `NX_IP` by 172 bytes whether or
  not anything joins, where `nx_igmp_enable()` grows it by nothing, and there is
  no MLD anywhere in the vendored tree -- no `nx_mld_*.c` exists, so a join
  reaches the driver and no report is ever sent, and a querying switch stops
  forwarding the group. The reasoning is beside the define in
  `port/netxduo-amiga/inc/nx_user.h`.
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
  - **RFC 3542** is in it as of the same day: `include/aminetxduo/cmsg.h`, the
    third published header. It adds no vectors, so the drawer's shape did not
    change.
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
- **RFC 3542: the subset worth having is built; the send half of
  `IPV6_HOPLIMIT` is not.** Assessed and implemented 2026-07-31. No new LVOs:
  it rides `sendmsg`/`recvmsg`, and `struct msghdr` was already the 28-byte
  4.4BSD shape. `src/bsdsocket/cmsg.c` and `include/aminetxduo/cmsg.h`.

  Shipped: `IPV6_RECVPKTINFO`/`IPV6_PKTINFO` (receive *and* the sticky and
  per-datagram send source), `IPV6_RECVHOPLIMIT` (receive), `ICMP6_FILTER` with
  its six macros, and the IPv4 half -- `IP_PKTINFO` and `IP_RECVDSTADDR`.
  `MSG_CTRUNC` now means what it says.

  **The assessment was wrong about the NDK.** `<sys/socket.h>` does define
  `struct cmsghdr` (12 bytes) and `CMSG_DATA`/`FIRSTHDR`/`NXTHDR`, and
  `<netinet/in.h>` defines `IP_RECVDSTADDR` as 7. Two of those three macros are
  unusable as shipped: `CMSG_NXTHDR` expands to an `ALIGN()` no NDK header
  defines, so any file that uses it fails to compile, and `CMSG_FIRSTHDR`
  returns `msg_control` without testing `msg_controllen`, which RFC 3542 §20.3.1
  calls out by name. Both are replaced in `aminetxduo/cmsg.h`, along with
  `CMSG_LEN` and `CMSG_SPACE`, which are genuinely absent. **`CMSG_ALIGN` is 4
  bytes**, as decided.

  Two more numbering decisions, on the same terms `IPV6_V6ONLY` was:
  - The IPv6 options answer to both the BSD and the Linux numbers, and
    *whichever a caller enables an option with is the numbering it gets back as
    `cmsg_type`*. Enabling with 36 gives `cmsg_type` 46; enabling with 49 gives
    50.
  - `IP_PKTINFO` takes 8, which is `IP_RETOPTS` in this NDK. That is a 4.3BSD
    get/set of arriving IP options, refused here and never implemented by any
    AmigaOS stack -- the same trade `IPV6_TCLASS` made against `IPV6_PATHMTU`.

  **Not implemented, and why:**
  - **The send half of `IPV6_HOPLIMIT`.** A `sendmsg` carrying one is refused
    with `EINVAL` rather than ignored. Nothing applies a per-socket TTL to a UDP
    send today either -- `IP_TTL` and `IPV6_UNICAST_HOPS` are stored and read
    back but never reach `nx_udp_socket_time_to_live` -- so a per-datagram hop
    limit would be the only one of the three that worked, which is a worse
    answer than a clean refusal. Do the sticky one first, then this.
  - **A source on a raw or TCP socket.** A raw IPv6 send picks its own source
    because the ICMPv6 checksum has already been computed over it (`raw.c`), and
    TCP has no per-write source. Both refuse the cmsg.
  - `IPV6_RTHDR`, `HOPOPTS`, `DSTOPTS`, `RTHDRDSTOPTS`, `PATHMTU`,
    `RECVPATHMTU`, `USE_MIN_MTU`, `DONTFRAG`, `NEXTHOP` -- extension headers and
    path-MTU state NetX Duo does not expose. Not planned.

  **The loopback interface has no index, so `ipi6_ifindex` is 0 over `::1`.**
  NetX Duo parks it at `nx_ip_interface[NX_MAX_PHYSICAL_INTERFACES]`, past the
  end of the range this library numbers, so `if_indextoname()` cannot name it
  and `rtm_index` does not report it either. `ipi6_addr` is still filled in, and
  a datagram off a real interface reports 1 or 2. Giving loopback an index means
  moving the `if_nametoindex()` / `rtm_index` convention, which
  `aminetxduo/ifindex.h` says is one decision -- raise it as that, not here.

  Verification: the ABI is pinned with `_Static_assert` in `cmsg.c` (every
  offset, and the `CMSG_*` arithmetic), and `tests/ipv6/ipv6_socket_test.c` --
  which links against none of our code -- runs 119 checks over the macros, the
  option round-trips, a datagram over `::1` with both objects attached and the
  answer sent back with an `IPV6_PKTINFO` source, and the IPv4 half over
  127.0.0.1. Green on Kickstart 3.1 / 68020 under FS-UAE, 2026-07-31. That
  harness is tier 2, so CI checks that it builds and not that it passes.

  The emulator run is what found both loopback edges: the index above, and
  `::1` living in `nx_ipv6_address[NX_MAX_IPV6_ADDRESSES]` rather than inside
  the configurable range, which made naming it as a send source fail for an
  address the machine plainly had. `bsd_ip6_zone_source()` in `transfer.c` has
  the same bound and is right to -- a zone only ever qualifies a link-local.

## Decided against — do not "fix"

- **A browse reports the whole peer cache, not the browse window**, and stays that way, 2026-07-31.
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
