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
## Open — no decision taken
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
- **IPv6 group membership (`IPV6_JOIN_GROUP`) is absent.** The IPv4 side is
  done (below); this is not, and is a separate decision because the numbers are
  worse. `NX_ENABLE_IPV6_MULTICAST` grows every `NX_IP` by 172 bytes whether or
  not anything joins, where `nx_igmp_enable()` grows it by nothing, and there is
  no MLD anywhere in the vendored tree -- no `nx_mld_*.c` exists, so a join
  reaches the driver and no report is ever sent, and a querying switch stops
  forwarding the group. The reasoning is beside the define in
  `port/netxduo-amiga/inc/nx_user.h`.
- **RFC 3542: the subset worth having is built, send halves included.**
  Assessed and implemented 2026-07-31, finished the same day. No new LVOs:
  it rides `sendmsg`/`recvmsg`, and `struct msghdr` was already the 28-byte
  4.4BSD shape. `src/bsdsocket/cmsg.c` and `include/aminetxduo/cmsg.h`.

  Shipped: `IPV6_RECVPKTINFO`/`IPV6_PKTINFO` (receive *and* the sticky and
  per-datagram send source, on UDP and on raw), `IPV6_RECVHOPLIMIT` and
  `IPV6_HOPLIMIT` in both directions, `ICMP6_FILTER` with its six macros, and
  the IPv4 half -- `IP_PKTINFO` and `IP_RECVDSTADDR`. `MSG_CTRUNC` now means
  what it says. A caller declares its control buffer with `CMSG_BUFFER()`,
  which is the union a `char[]` cannot be.

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

  **The hop limit was a two-part fix, and the smaller part was ours.** The
  reason the send half was first left out -- "nothing applies a per-socket TTL
  to a UDP send either" -- was an argument for doing both, and both are done.
  `bsd_mcast_prepare_send()` was writing `NX_IP_TIME_TO_LIVE` into
  `nx_udp_socket_time_to_live` for every unicast destination, discarding
  `as_Ttl`; it now writes `as_Ttl`, so `IP_TTL` and `IPV6_UNICAST_HOPS` reach
  the wire on UDP as they already did on raw. That fixed IPv4 only, because
  `_nxd_udp_socket_send()` honours the socket field on IPv4 and substitutes the
  IP-wide `nx_ipv6_hop_limit` on IPv6, in the same function, three lines apart
  -- the same asymmetry `amiga-ipv6-raw-hop-limit` fixed for raw.
  `amiga-ipv6-udp-hop-limit` in the NetX fork is the other half; the PR body
  there is the argument, including what it changes for an addon. A per-datagram
  `IPV6_HOPLIMIT` on `sendmsg` then rides the same field, and -1 means "the
  socket's own" as RFC 3542 §6.3 says.

  **A source on a raw socket is honoured; on TCP it is refused and always will
  be.** The checksum argument that refused raw was backwards:
  `bsd_raw_send_v6()` selects the source *before* it computes the ICMPv6
  checksum, so a named source is the one the checksum covers -- it feeds the
  same `nxd_ip_raw_packet_source_send()` a bind already fed. TCP is different in
  kind and not in degree: a stream's source is fixed when the SYN goes out and
  every segment carries `nx_tcp_socket_connect_interface`, so there is nothing
  per-write to name. Naming it at `connect()` is a real gap and a separate one,
  closed in the fork by `nxd_tcp_client_socket_source_connect()`; the per-write
  refusal in `bsd_cmsg_parse()` stands whatever `socket.c` does with that.

  **Loopback has an index now: `NX_LOOPBACK_INTERFACE + 1`, called `lo0`.** The
  convention was already "NetX slot + 1" -- that is what `if_nametoindex()` and
  `rtm_index` both count -- so loopback needed no new convention, only the two

- **Publish `NetStackQuery` / `NetStackControl`?** They are still private, at
  -0x366/-0x36c. They are what a third-party `netstat` would need, and
  `ShowNetStatus`, `netstat` and `arp` already depend on them being stable, so
  the recommendation is to publish. Not done because publishing freezes
  `NetStatusHeader` and every `NETCTRL_*` request struct, which is the owner's
  call. Two lines when decided: `netstatus.h` into `PUBLIC_HEADERS` in
  `tools/stage-developer.sh`, and an `AMI_NETSTATUS_MIN_REVISION` beside the
  existing magic.

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

- **The `sana2_rx.c` reader orphan does not reproduce under emulation.**
  `ami_sana2_rx_stop()`'s last-resort path logs `reader N did not stop;
  leaking its stack` and leaks 32 KB when a SANA-II driver ignores `AbortIO`,
  which Commodore's a2065.device 2.16 is documented to do. Thirteen full
  teardowns under Amiberry with that driver logged it zero times, so the
  emulated card returns its queued `CMD_READ`s where the real one may not.
  `run-cycledrill.sh` greps the serial log for it on every run and prints the
  count; `AMINETXDUO_CYCLE_ORPHAN_FATAL=1` makes it fail. Confirming it needs
  real hardware. Checked 2026-07-31.

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
