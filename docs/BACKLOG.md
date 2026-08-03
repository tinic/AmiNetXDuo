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

### From the 0.16.7 audit and RFC survey (2026-08-02)

Conformance status and citations are in `CONFORMANCE.md`. This is the work list.
Items fixed the same day are omitted.

**bsdsocket API**

| Item | Cite |
|---|---|
| `SBTC_FDCALLBACK` stored, never invoked | `errno.c:386`, `library.c:281` |
| `SBTC_SIG_ADDRESS_CHANGE_MASK` stored, never signalled | `bsdsocket_internal.h:441-444` |
| `SO_REUSEADDR/REUSEPORT/BROADCAST/OOBINLINE/SNDBUF` succeed, no effect | `options.c:105-122`, `:148-189` |
| `SO_RCVBUF` on TCP calls a function compiled out without `NX_ENABLE_LOW_WATERMARK`; status discarded | `options.c:171-178` |
| `TCP_NODELAY` returns success before checking `optval`/`optlen`/type | `options.c:221-223` |
| `IP_TTL`/`IP_TOS`/`IPV6_TCLASS` applied on raw only | `raw.c:559`, `socket.c:1354`, `:1366` |
| `IP_TTL` = 256 succeeds, puts 0 on the wire; IPv6 siblings range-check | `options.c:139-145` |
| `SO_ERROR` zeroed before a copy-out that can fail | `options.c:161-162` |
| multicast `optlen` 2 unhandled; big-endian reads the high byte | `mcast.c:313-338` |
| `SO_LINGER` negative → infinite tick count, `CloseSocket()` never returns | `options.c:254-276` |
| `TCP_MAXSEG` discards NetX status, accepts negatives as 4-billion MSS | `options.c:254-276` |
| `SO_KEEPALIVE` writes live NetX state with no ThreadX bracket | `options.c:322-325` |
| `bsd_no_aliases[1]` is one writable static returned to every opener, non-const | `netdb.c:47-57` |
| `cmsg.c` Linux aliases 49/50/51 collide with BSD `IPV6_HOPOPTS`/`DSTOPTS`/`RTHDR` — a BSD `IPV6_DSTOPTS` reads `struct in6_pktinfo` from the caller's buffer | `cmsg.c` |

**Resolver**

| Item | Cite |
|---|---|
| `EAI_AGAIN` never returned; the backend distinguishes the cases and `h_errno` gets it right | `addrinfo.c:476-516`, `resolver.c:150-168` |
| DNS mutex contention reported as `HOST_NOT_FOUND` (`TX_NOT_AVAILABLE` unmapped) | `netstack_dns.c:170-198` |
| Resolver calls uninterruptible: 3 retries × 5 servers, doubling, no break-signal, mutex held throughout | `resolver.c:18`, `:53-54` |
| No negative caching; every miss re-queries every server | `nxd_dns.c:4464` |

**DNS security** — these compose; see the CNAME note below before starting

| Item | Cite |
|---|---|
| No source-address validation; `nx_udp_source_extract` unused by `addons/dns` though `tftp`/`snmp`/`BSD` use it | `nxd_dns.c:4279-4360` |
| QDCOUNT=0 skips name, type and class checks | `:4497` |
| AUTHORITY records accepted as answers, cached under their own owner | `:4803-4805`, `:4866` |
| TTL sign bit unmasked → ~68-year entry | `:8102` |
| Elapsed TTL by integer tick division; `last_used` reset on hit | `:9117`, `:9137` |
| TC ignored, no TCP fallback, no EDNS0. `NX_DNS_TC_FLAG` defined, never read | `nxd_dns.h:124` |
| Reverse path validates only the ID | `:3800-3858` |
| Question comparison case-sensitive; cache path is not — breaks DNS-0x20 | `:4515` vs `:9933` |
| `.local` leaks on the IPv6 path; `addrinfo.c:476` calls it first | `netstack_dns.c:401-446` |
| `254.169.in-addr.arpa.` reverse leaks; fix is an immediate negative | `netstack_dns.c:367-398` |

**A bailiwick check must be implemented together with CNAME chain following.**
CNAME processing is compiled out (`NX_DNS_ENABLE_EXTENDED_RR_TYPES` undefined),
so the following A record — owner = CNAME target — is accepted only because no
owner-name check exists. Adding the check alone would fail every CNAME-hosted
name. Present behaviour: those records cache under the target, so the queried
name always misses, and a CNAME-only response yields `NX_DNS_QUERY_FAILED`.

**DHCP**

| Item | Cite |
|---|---|
| No ARP probe, no DHCPDECLINE. Upstream code exists behind `NX_DHCP_CLIENT_SEND_ARP_PROBE`, defined nowhere; our test phase `#ifdef`'d out. Cost +3.9 s/boot | `docs/DEVELOPMENT.md:606-614` |
| No gratuitous ARP after a DHCP address is set | `nx_arp_gratuitous_send()` uncalled |
| ARP conflict defended but never reported; `nx_interface_ip_conflict_notify_handler` registered nowhere | |
| Option 121 (classless static routes) never requested; we ask for 33 only | `netstack.c:1052`, `:1137` |
| No RFC 3396 long-option concatenation; first fragment wins | `nxd_dhcp_client.c:7526` |
| Option 52 (overload) unhandled | |
| `nxd_dhcp_client.c:6939`, `:6974` compare seconds against ticks | |

**TCP**

| Item | Cite |
|---|---|
| SACK send side: we advertise SACK-Permitted and never parse incoming blocks. `NX_TCP_SACK_KIND` defined and written, never read | `nx_tcp.h:93`, `nx_tcp_sack_option_build.c:212` |
| RFC 3708 (sender-side spurious-retransmission detection from received D-SACK) absent. Applies to the write direction only. The `sack-transmit` branch discards ranges at or below `SND.UNA` as "D-SACK or stale" — correct for RFC 2018, but it drops the information RFC 3708 needs | unlanded branch |
| Broadcast SYN answered (destination unchecked) + one half-open slot per listener → port dead 127 s | `nx_tcp_packet_process.c:517-542`, `socket.c:1729-1730` |
| RFC 5961 absent: in-window RST resets, in-window SYN resets and tears down, RST at any sequence when `RCV.WND==0` | `nx_tcp_socket_packet_process.c:234-271`, `:165-167` |
| ICMP errors never reach TCP or UDP | `nx_icmpv4_packet_process.c:143-171` |
| MUST-23: R2 for SYN is 127 s, needs 180. `NX_TCP_MAXIMUM_RETRIES 7` → 255 s, `connect()` blocks that long | `nx_user.h:201`, `:204` |
| Sender SWS avoidance absent; Nagle absent | `nx_tcp_socket_send_internal.c:455-470` |
| Receiver SWS has no `min(MSS, RCV.BUFF/2)` floor | `nx_tcp_socket_receive.c:210-218` |
| Restart-after-idle absent; no per-socket last-send time | |
| TIME-WAIT 2MSL not restarted on a retransmitted FIN | `nx_tcp_socket_packet_process.c:456-459` |
| In-window RST destroys TIME-WAIT (RFC 1337) | `:234-248` |
| Listen path sets IW = 1 MSS where the other three paths do RFC 3390 | `nx_tcp_packet_process.c:734-735` |
| RST-in-response-to-RST hole on the malformed-option path | `nx_tcp_socket_packet_process.c:298-332` |
| UDP demux ignores the 4-tuple | `nx_udp_packet_receive.c:247` |
| UDP checksum verified at dequeue, not enqueue | `nx_udp_socket_receive.c` |
| `o02_duplicate_segment` fails: a duplicate of acknowledged data is not re-acked when the receive queue is non-empty | `tests/tcpdrill` |

**IPv6**

| Item | Cite |
|---|---|
| PMTUD off and no 1280 cap; RFC 8201 §1 permits omitting PMTUD only with the cap. PTB not dispatched. RA MTU option discarded on the same `#ifdef` | `nx_user.h:651-661`, `nx_icmpv6_packet_process.c:210-216` |
| No fragment reassembly, either family | `nx_ip_fragment_enable()` uncalled |
| No MLD; recorded rationale is wrong (snooping filters, it does not forward) | `nx_mld.h` stub, `nx_user.h:636-645` |
| A-bit test nested inside L-bit: prefix A=1 L=0 forms no address | `nx_icmpv6_process_ra.c:310`, `:332` |
| No RFC 7559 RS backoff: fixed 4 s, stop after 3 | `nxd_ipv6_router_solicitation_check.c:86-107` |
| No interface but 0 ever sends an RS — `ipv6_enable()` runs before the attach loop | `netstack.c:648` vs `:658-678` |
| IGMP all-hosts filter for interface 1 is a race with the IP thread; unresolved statically | `nx_ip_thread_entry.c:526-545` |
| No preferred lifetime, `NX_IPV6_ADDR_STATE_DEPRECATED` assigned nowhere — no graceful window before an address vanishes | `nx_api.h:959` |
| No privacy addresses (8981) or opaque IIDs (7217); MAC in every global address | |
| No RDNSS (8106) — IPv6-only link yields addresses but no DNS | `nx_icmpv6.h:68-74` |
| No ICMPv6 error rate limiting; §2.4(e.3) enforced at one call site only | `nx_icmpv6_send_error_message.c` |
| No 1280 MTU floor on a SANA-II device reporting less | `sana2_device.c:183-201` |
| Raw oversize send still drops after success | `transfer.c:722-741` |

**TLS**

| Item | Cite |
|---|---|
| Hostname check compares CN before SAN and returns | `nx_secure_x509_common_name_dns_check.c:92-97` |
| Chain walk has no depth counter and no visited set — two cross-signed certs loop | `nx_secure_x509_certificate_chain_verify.c:86-160` |
| Fatal alerts and a bare FIN are both reported to the application as end of stream; the `CLOSE_NOTIFY_RECEIVED` arm is unreachable in a non-DTLS build | `tls_conn.c:713-723` |
| No revocation of any kind | |
| PKCS#1 v1.5 parser: byte 0 unchecked, any nonzero padding accepted, trailing data ignored, OID discarded | `nx_secure_x509_pkcs7_decode.c:104`, `:113-121`, `:187-197` |
| `&&` where PKCS#1 wants `\|\|` in the strict verifier | `nx_secure_tls_process_certificate_verify.c:681` |
| CBC explicit IV is the previous ciphertext block | `nx_secure_tls_record_payload_encrypt.c:186-188` |
| Padding-validity timing: valid is *faster* by up to a record of SHA-256 | `nx_secure_tls_process_record.c:317-346` |
| No encrypt-then-MAC (7366) | |
| RSA-MD5/RSA-SHA1/ECDSA-SHA1 in the X.509 table and advertised | `ami_tls_crypto.c:1538-1540` |
| Static-RSA suites still offered | `:1580-1581` |
| No extended master secret, while we do resume | |
| `REQUIRE_RENEGOTIATION_EXT` not defined — handshake completes with an un-upgraded server | `nx_secure_tls_process_serverhello.c:292-304` |
| keyUsage fails open when the extension is absent | `nx_secure_x509_certificate_verify.c:102-115` |
| No EKU check (parser exists, no call site), no nameConstraints, no critical-extension rejection | |
| Outer `signatureAlgorithm` overwrites the inner one; verification uses the unauthenticated value | `nx_secure_x509.c:195`, `:518`, `:868` |
| No minimum RSA modulus | |
| `NX_SECURE_KEY_CLEAR` undefined — session keys left in freed heap | |
| Resumption stores master secrets cleartext, no `SetProtection`, 24 h cap never fires without an RTC | `tls_resume.c:568`, `:718`, `:439` |
| X25519/Ed25519 implemented in `crypto68k` and not wired into TLS | |
| `TLSRandom()` packs all four bytes of a masked draw — every fourth byte has bit 7 clear | `tls_conn.c:986-993` |
| ECDSA DER: only the `0x30` tag checked, no INTEGER tags, no minimal-encoding check | `nx_crypto_ecdsa.c:324` |
| Entropy: ~18 credited bits on a no-RTC machine (clock guard credits 8 for uptime), no reseeding, no persisted seed, no health tests. `is_seeded` is not on the private context vector, so `tls.library` cannot check it | `ami_random.c:590`, `nxcontext.h:191-203` |

**httpd**

| Item | Cite |
|---|---|
| `Content-Length` accumulates with no overflow check; `4294967306` wraps to 10, `5abc` parses as 5; duplicates take the last | |
| TE + CL both accepted (precedence correct); should be 400. TE matched by 7-char prefix, so `gzip, chunked` is missed and `chunkedX` matches | |
| Chunk size shifts unbounded; 9+ hex digits wrap and `100000000` reads as terminator, rest parsed as a pipelined request | |
| Header values over 255 bytes truncated silently; for `Destination:` a truncated path becomes a valid target and `Overwrite` defaults true | |
| Chunked bodies bypass the size and time bounds: `body_left` never set, every read refreshes the progress timestamp. eight connections sending one byte every 29 s exhaust the connection table. Also skips the free-space precheck | `httpd.c:4342-4434` |
| COPY/MOVE to a name the filesystem would shorten, with nothing in the way — the check catches a collision, not the first create | |
| Document root with a trailing slash may resolve to its parent; `httpd_root` never normalised | unverified on hardware |
| Hard-linked directories walked through — `ST_LINKDIR` tests as a directory and `Lock()` follows it; no `O_NOFOLLOW` on AmigaOS | unverified on hardware |

**WebDAV** — remaining RFC 4918 gaps

PROPFIND ignores the request body (no `propname`, no named `prop`, no 404
propstat) · cross-host `Destination:` treated as local, answered 201 · LOCK on
an unmapped URL creates nothing (§7.3 wants a locked empty resource that then
survives its lock — conflicts with "nothing visible until whole") · Depth-0
collection lock does not protect members · `<D:owner>` truncated to 47 bytes and
stripped of non-ASCII · lock refresh accepted from outside the lock's scope ·
32-bit LCG lock tokens · 412/423 bodies carry HTML not `<D:error>` · UNLOCK with
no `Lock-Token` returns 409 not 400 · `Depth: 2` silently becomes 0 ·
`If-None-Match: *` unimplemented · XML skimmer has no nesting, entity decoding,
CDATA or comment handling · UTF-16 bodies not understood · PROPPATCH naming no
settable property emits a `<D:response>` with no propstat and no status
(invalid per §14.24, ~5 lines).

**Decided**

- **Dead properties: declined.** §9.2 SHOULD. AmigaOS offers a 79-byte filenote
  or a sidecar; a sidecar costs `Lock`+`Examine`+`Open`+`Read` per entry per
  PROPFIND, doubling listing cost on a 68000 and the file count of every drawer,
  and a `.props-<name>` scheme collides on OFS's 30-character truncation.
  Measured client cost of having none: Explorer's creation/access times and
  attribute bits, nothing else.
- **Bounded multistatus is deliberate.** 8 entries / 768 bytes, 8 properties.
  Honouring §9.8.3 unbounded needs a spill file or a streamed 207, and a
  streamed 207 cannot be retracted once the head is out.

**Concurrency** (all three already in `REENTRANCY.md`/`ALLOCATIONS.md`)

| Item | Cite |
|---|---|
| `ami_bpf_close_owner()` releases channels with no `ch->reading` check that `ami_bpf_close()` has | `bpf_channel.c:233-243`, `:586-593` |
| ~18 reads of `ami_ns` take no lock; every write does | `netstack.c:51`, `:1428` |
| Baton slot table: 16 slots keyed by `struct Task *`, nothing sweeps them; Exec recycles Task addresses | `netstack_baton.c:68` |

**Structural**

`src/mbuf` leaks every slab the day any `mbuf_*` LVO is implemented —
`ami_mbuf_cleanup()` has no production caller · `ami_sana2_lookup()`'s lock-free
read is correct only because attach writes `iface` last · `AMI_MDNS_PRIORITY`
defined outside the priority ladder's `#error` assertions
(`netstack_mdns.c:48`) · post-link pcrel check in 4 `CMakeLists`, absent from
~22 that link m68k executables including smoke/bracket/soak ·
`tests/perf/prof/` is a dead fork of `tools/profiler/` with an incompatible
sample record · a `cross`-stage build failure whose log has neither `error:` nor
`Error` dies on the diagnostic grep before anything is recorded.

**Tests**

Socket-option surface entirely untested — nothing exercises `SO_RCVBUF`,
`SO_SNDBUF`, `SO_LINGER`, `TCP_MAXSEG`, `TCP_NODELAY`, `IP_TTL`, `IP_TOS`,
`SIOCATMARK`, `FIOASYNC`, any `SIOCGIF*`, or the multicast width paths — which is where the API findings are concentrated · `sana2` has no dedicated suite · **httpd has no fuzzer** and is the newest network-facing parser; the chunked
state machine is the first target · `usergroup` functional tests were added 2026-08-02.

**Docs**

User guide COMMANDS node says seven Shell commands; the build produces 25 and
the installer copies all of them · `httpd` missing from `dist/ReadMe`'s C: list
and uninstall list · `SECURITY.md` trust-boundary table omits `httpd` · README
says seven build configurations, `ci.sh` has ten (twelve with host tiers) ·
`install/README.md` needs a line for `Guide.info` · `clients/dropbear/build.sh`
and `dist/make-dist.sh` are release-only steps CI never runs.

**Withdrawn** — do not re-raise

- The CI analyze stage does **not** skip cppcheck. `stage_analyze` is invoked as
  `stage_analyze || true`, and bash suppresses `set -e` inside a function called
  as part of an `||` list. Reproduced both directions.
- `ugl_crypt()` returning `"*"` is a hazard for third-party callers, not a live
  bypass: it also sets `UG_ENOSYS`, and nothing in the tree calls it.
`src/tools.h` are pre-refactor copies of the real files, each missing changes
the tracked versions have; `stage-developer.sh` and `aminetxduo_lib.sfd` at the
root are byte-identical to their tracked counterparts; `tmp_x/` holds a third,
older generation. None is referenced by any build file; all are `grep` traps.
Separately, a build failure in the `cross` stage whose log contains neither
`error:` nor `Error` dies on the diagnostic grep itself, before anything is
recorded and before the summary prints — CI still goes red, so it costs
diagnosis rather than correctness.

**Two audit claims that did not survive re-checking**, recorded so they are not
raised again. The CI analyze stage does **not** silently skip cppcheck: the
`grep '^NOT COVERED'` exiting non-zero under `set -euo pipefail` is a real
mechanism, but `stage_analyze` is only ever invoked as `stage_analyze || true`,
and bash suppresses `set -e` inside a function called as part of an `||` list.
Reproduced in both directions. And the `crypt()` stub is a hazard for
third-party callers rather than a live lockout bypass: `ugl_crypt()` does return
`"*"`, inverting the disabled-account convention for anyone using the canonical
`strcmp(crypt(pw, salt), pw_passwd)` idiom, but it also sets `UG_ENOSYS` which a
correct caller checks, and nothing in the tree calls it. Worth a documented
warning, not a defect in shipped code.

### Performance — measured positions

**Where transfer time goes.** Sampling profiler, `tools/profiler/`, 1 MB TCP,
A1200/68020, 1000 Hz, 4411 samples, 0.2% unattributed:

| category | wire | loopback |
|---|---|---|
| NetX Duo protocol | 25.6% | 16.7% |
| ThreadX + Amiga port | 23.3% | 16.2% |
| Kickstart (Exec) | 19.2% | 19.4% |
| copy (net68k asm) | 19.0% | 34.5% |
| checksum | 12.3% | 12.6% |

Supersedes the earlier "78% is inside NetX Duo protocol processing", which was
derived by subtracting measured primitives from a measured transfer.
**ThreadX + Exec is 42.5%, larger than NetX Duo's 25.6%.** Largest non-copy cost
is scheduling glue: `_tx_thread_interrupt_restore`/`_disable` 6.9%, Exec
`Reschedule`/`Switch`/`Dispatch`/`Supervisor` 10.5%. Top entries (wire):
`n68k_copy_bytes` 18.2%, `n68k_sum_longwords` 9.5%, `_tx_thread_interrupt_restore`
4.2%, `Supervisor` 3.4%, `Reschedule` 2.8%; top 24 = 73.0%.

**The 42.5% is a tight single-socket loop, not an application.** Same categories
against a real `fitz` run, Exec idle excluded:

| | NetX Duo | copy+checksum | Exec | ThreadX | ThreadX+Exec |
|---|---|---|---|---|---|
| bracket test, wire | 25.6% | 31.3% | 19.2% | 23.3% | **42.5%** |
| fitz, whole run | 33.2% | 32.3% | 20.7% | 13.8% | **34.5%** |
| fitz, read arm | 33.2% | 34.8% | 20.3% | 11.7% | **32.0%** |
| fitz, write arm | 33.1% | 30.0% | 21.0% | 15.8% | **36.8%** |

ThreadX's own share falls 23.3% → 13.8% because a real client spends much of a
read waiting, amortising the per-call bracket. Quote 42.5% only for a tight loop.

**Profiler implementation constraints.** A CIA timer cannot be used and fails
silently: `AddICRVector()` arbitrates the vector, not the hardware. CIA-B timer B
ran correctly at 1000 Hz until the first `ami_millis()` took it back via
timer.device MICROHZ; CIA-B timer A stopped with `ciaicr=$85`, an interrupt
raised and never acknowledged (Exec EXTER race). Source is now audio channel 3 at
level 4 — no latching chip in the acknowledge path, and level 4 sees inside the
level 2/3 handlers where a SANA-II receive runs. `prof_start()` measures each
candidate over eight windows and rejects any that does not hold rate.
Attribution: Exec keeps INLINE code in some jump-table slots rather than a `JMP`
(`Forbid`/`Permit` among them) — 7.2% was lost until a PC inside
`[base-negsize, base)` was attributed to the slot. `Disable()` masks INTENA so
those sections are unsampled; `Forbid()` is sampled normally, which is where the
bracket lives. Containment verified 97/98/96% on 68020, 100% on 68030. Not yet
run on 68000 — fs-uae aborts host-side on the A500 profiles before boot.

**Stack comparison, fixed rig.** `tests/perf/run-stackprof.sh`, bridged Amiberry
A3000, KS 3.1 40.68, one `a2065.device`, released `fitz`,
`FitzBench KB=4096 CHUNK=32768 REPS=3`. Both stacks took the same DHCP lease —
an earlier comparison ran its arms at different addresses.

| | AmiNetXDuo read | Roadshow read | AmiNetXDuo write | Roadshow write |
|---|---|---|---|---|
| throughput | 980, 982 KB/s | 1259, 1909 KB/s | 1929, 2058 KB/s | 1254, 1301 KB/s |
| Exec idle loop | 65.1, 65.3% | 23.9, 50.7% | 19.0, 20.2% | 10.9, 12.7% |
| CPU per MB | 363, 366 ms | 409, 417 ms | 399, 432 ms | 704, 716 ms |

We read at half the rate while spending less CPU per byte, so the read gap was
never CPU work. Cause identified 2026-08-02: duplicate-ACK suppression disabled
fast retransmit (fixed, `4b41379a`), and the lab rig's own 0.35% reordering was
rewarding that defect. The remaining ~15% is wake latency — 9.2 ms from response
header on the wire to first ACK against Roadshow's 7.0, machine idle throughout.

**bifat's four-stack benchmark**, real hardware, `timecmd copy` each way. We are
first or second on send everywhere and third or fourth on receive everywhere;
AmiTCP 4.6 is the exact mirror, so both sit at opposite ends of one trade rather
than one being slow. Receive, ours vs AmiTCP 4.6: A3000/060 + X-Surf-100
699/1103 · A500/1230-50 + X-Surf-500 389/569 · A1200/060 + CNet16 400/454 ·
A500 68000/14 MHz + X-Surf-500 115/175. Send, ours: 1044, 346, 580, 142. The
deficit widens with link speed (worst -37% on X-Surf-100 behind an 060, smallest
on CNet16 where the card dominates), which indicates a per-packet cost.

**net68k primitives, 68000**, WinUAE 6.0.3, cycle-exact A500 profile. The
byte-loop fallback costs **6.1x, not the 4x the source claims**: 5.4 µs/B against
0.89 for the `movem.l` path. `n68k_copy.S` takes it whenever `to` and `from`
disagree in bit 0, because a misaligned word access on a 68000 is an address
error rather than a slow path. It should never fire on receive — the alignment
census gives 0 mod 4 (application buffers, packet prepend pointers) and 2 mod 4
(eight of nine real drivers), both matching the destination's parity. Open
question: does any real driver hand over an odd buffer.

**Foreign stacks do not fold the checksum into the copy**, measured with
`tests/tapprobe/`. Roadshow reads the source exactly once (scribbling `0xEE` over
it the instant the hook returned left every reply intact at all four alignments)
and its hook costs 133 ns/B at best alignment against 158 for a plain `movem.l`
copy of the same data — no budget for per-longword arithmetic. AmiTCP_NG settled
from its GPL source.

**PARKED: melded copy-and-checksum.** Branches `meld` and `wiring` (`bfa9937`);
nothing on `main`, default build unaffected. `n68k_copy_sum_longwords()` measured
256.00 ns/B against 378.56 for the separate pair — 32.4% on a 68020. Both halves
have since improved and the melded routine has not (copy 159, checksum 149.8), so
the margin is now 17%. Wired into receive behind `AMINETXDUO_RX_COPY_SUM`
(default OFF) the receive pair went 389.33 → 312.92, about 20%; the stamp write,
prefix subtraction and acceptance checks eat a third of the primitive's gain.
**Parked because the device buffer is misaligned on 8 of 9 real drivers** —
ariadne, ariadne_ii, x-surf, x-surf-100 (Z2 and Z3), hydra, a2065 and cnet all
hand `S2_CopyToBuff` a pointer at 2 mod 4 and the fast path declines. Under a
400 pkt/s flood the a2065 gave 1266 consecutive misses and zero stamps. Only
`eb920.device` is aligned and could not complete a bulk transfer. Reviving this
means rewriting the melded loop around `movem.l` and the chained `addx` first.

### Harness

| Item | Status |
|---|---|
| `tcpdrill`'s device starts offline; Roadshow stops after `S2_ADDMULTICASTADDRESS`, arms `S2_ONEVENT` and waits, so its interface never comes up. Invisible to us because we send `S2_ONLINE` ourselves | Open. Blocks pointing tcpdrill at a foreign stack |
| `run-tcpdrill.sh` does not pass `-x` to `fsuae-run.sh` although the drills carry timing assertions. `AMINETXDUO_PERF=1` forces it | Open |
| The fs-uae lock is per-checkout (`$ROOT/build/.fsuae.lock`), so `-x` does not serialise against another checkout on the same host | Open |
| `tcpdrill` queued IPv6 ND frames as results | Fixed 2026-08-02 |
| `tcpdrill`'s `idle` counted 20 ms per `Delay(1)`; `pump()` between them made `idle 700` take ~1520 ms | Fixed 2026-08-02. `wait_frame()` has the same under-count, which makes `within=` looser than it reads — left alone, tightening it moves every timing case at once |

### AmiTCP_NG

Comes up on a Kickstart 3.1 directory boot once `rexxsyslib.library` and the
math libraries are in `LIBS:`. The earlier `errno 43` `EPROTONOSUPPORT` from
`AddNetInterface` was not a protocol-domain fault and not an OS-version
requirement. Measured 2026-08-02 as the third arm: read 1108 KB/s against our
983 and Roadshow's 1824.
## Decided against — do not "fix"

**Host-side cycle counting: Moira and Musashi both rejected**, 2026-08-01,
branch `agent/moira-eval`.

- Moira's 68000 timing is exact only in a configuration it does not ship.
  `MOIRA_PRECISE_TIMING` defaults false, making `SYNC(x)` a no-op, so
  data-dependent costs are discarded — `MULS.W` is charged its worst case 54
  whatever the operand. `MOIRA_MIMIC_MUSASHI` defaults true and its own comment
  says to turn it off. Both are unconditional `#define`s. Patched it matches
  M68000PRM 30/30 including `MOVEM.L (An)+` at 12+8n; unpatched 29/30.
- **Moira's 68020 has no instruction cache**, measured: the same loop body from
  64 to 640 bytes costs exactly 8.000 cycles per pair at every size, with no
  turn at 256 where FS-UAE's 020 shows one. **Must not be used to choose unroll
  depth.**
- Errors against real measurement do not share a sign: 68020 copy +23%, 68000
  checksum at 20 B −41%, alignment penalty +1.2% where the machine gives +31%.
  Structural — Moira is a CPU, not a machine: flat always-ready RAM, no chip-RAM
  contention, no prefetch overlap, no cache, no unaligned-access penalty, which
  is the effect `n68k_copy.S` aligns its destination to avoid.
- Musashi is worse: its 68020, 68030 and 68040 share one cycle table (verified
  by dumping it) and that table is the 020 best case, i.e. a permanent 100%
  I-cache hit. `USE_ALL_CYCLES()` charges a whole timeslice to a spin loop,
  inventing a hotspot on exactly the busy-waits a network stack does.
- **Nobody has a cycle model above the 68020.** WinUAE's own 020+ cores have
  cycle accumulation `#if 0`'d out in `gencpu.cpp`.

**The ThreadX tick rate stays at 50**, 2026-08-01. Swept 20/40/50/60/80/100 Hz,
8 runs per arm, interleaved, 48 runs all PASS. Between-arm span is not larger
than within-arm spread on any metric and is not monotone in rate (80 Hz reads
higher than 60).

Mechanism: **wakeups stay at ~850 whatever the rate**, because the source is
`timer.device UNIT_VBLANK` and the knob does not touch it. Below 50 Hz the extra
wakeups are empty (59% idle at 20 Hz); above it they deliver catch-up bursts
(841 of 848 wakeups deliver more than one tick at 100 Hz). Instantaneous skew
was 0 in all 48 runs; nothing was clipped, lost, deferred or over budget.

20 Hz delivers 339 ticks instead of 867 and skips 493 wheel walks for 0 KB/s.
The argument against it is not throughput: `src/bsdsocket/options.c:83` derives
`SO_RCVTIMEO`/`SO_SNDTIMEO` granularity from `1000000/NX_IP_PERIODIC_RATE`, so
20 Hz coarsens socket timeout resolution from 20 ms to 50 ms.

Two traps for anyone who tries. `-D` cannot set the knob:
`port/netxduo-amiga/inc/nx_user.h:34` hard-defines `NX_IP_PERIODIC_RATE 50`
unconditionally and is included before `nx_port.h`'s `#ifndef` fallback, so
overriding `TX_TIMER_TICKS_PER_SECOND` alone silently rescales every TCP timer
by the ratio — a sweep that looks plausible and measures nothing. Both headers
must move together. And there is no delayed-ACK confound to isolate:
`nx_tcp_enable.c:111` computes `_nx_tcp_ack_timer_rate` as
`ceil(NX_IP_PERIODIC_RATE / NX_TCP_ACK_TIMER_RATE)`, so the ACK interval
self-scales to a constant 200 ms at every rate.
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

| Item | Detail |
|---|---|
| A task can hold about five library bases that use a `WaitSelect()` timeout | Each base takes an event signal from the calling task (`library.c:244`) and the first timeout takes another for the timer (`select.c:475`), out of a Task's 32 bits. `OpenLibrary()` refuses once they run out, which is correct. Found 2026-07-31 when eight opens got five; `run-cycledrill.sh` now asks for four |
| `run-fitzbench.sh`'s write figure is not a rate | Stops timing when the write call returns, not when data drains. Guest-timed 1718 KB/s against a measured wire rate of 364. Reads agree between clocks; writes diverge ~4.7× |
| `run-fitzbench.sh` refuses a same-host virtual peer | Over the uncomputed TX checksums that `ethtool -K <iface> tx off` fixes. Can be relaxed. Query by full path — `/usr/sbin` is not on a non-login ssh PATH |
| cppcheck stage skips itself | Baseline from 2.20.0, gate hosts have 2.17.1. Install 2.20.0 or regenerate |
| FS-UAE cannot boot headless | `FATAL: [GLAD] …`. Harnesses take `-A` for Amiberry and `-a ARGS` to pass arguments |
| A pinned toolchain install can carry the argv bug in all eleven `crt0.o` | GCC 16.1.1b locally built reports `11 buggy` — has the compiler fix for the frame skew, not newlib's `120371e` for the argv declaration. Anything built there against `-lc` without `fix-toolchain-crt0.py` hands ported clients `&__argv` |

**Amiberry's A600 PCMCIA emulation does not work, for any stack.** On an A1200
with `-N ne2000_pcmcia` the emulated RTL8019 and `cnet.device` drive a full DHCP
lease — ours reports `eth0: online, address 10.0.2.15` and the Roadshow 1.15
demo reports the same address from the same card. Move identical staging to
`-m A600` and both fail: ours cannot open the device, Roadshow says
`Could not add interface "eth0" (Input/output error)`. **An A600 failure says
nothing about the code**; PCMCIA tests must run on the A1200 profile.

Not a memory limit, which was the confound: the failing A600 run had 4.6 MB
free, and an A1200 at `chipmem_size=2;bogomem_size=0;fastmem_size=0` — 1 MB of
chip and nothing else, the supported floor — brings the same card up and leases
an address with 374,760 bytes still free. That is also the first demonstration
of the README's 1 MB floor with a live interface rather than by inference;
`run-oommsg.sh` only proves the other end, that 512 KB cannot start the stack.

**Two interfaces: proved on a host, not provable on a guest.** TCP leaves from
the address `bind()` named (`nxd_tcp_client_socket_source_connect()` in the
fork); `tests/netstack/host/test_tcp_source_connect_host.c` asserts it against an
`NX_IP` with two `nx_ip_interface[]` entries, compiling the real connect, route
lookup and SYN build.

**Correction to an earlier note here**: `AddNetInterface eth0` does *not* hang
with a second card present. With `a2065` on Zorro and the NE2000 PC Card on
PCMCIA — different buses, Fast RAM held to 4 MB to stay clear of the credit-card
window at $600000 — `AddNetInterface eth0 eth1` returns 0, `eth0` leases, and
`ShowNetStatus` lists both with `eth1` offline. The hang belonged to two Zorro
cards. The real limit is that `cnet.device` will not open while the A2065 is
present although it opens alone, and Amiberry offers exactly two network boards,
so that is the only pair available. `SrcProbe` takes a second address and a
destination for a machine that can host one.

**The `sana2_rx.c` reader orphan does not reproduce under emulation.**
`ami_sana2_rx_stop()`'s last-resort path logs `reader N did not stop; leaking
its stack` and leaks 32 KB when a driver ignores `AbortIO`, which a2065.device
2.16 is documented to do. Thirteen full teardowns under Amiberry logged it zero
times. Nothing is outstanding in the code — the free sits outside the started
gate where the teardown owns it, and `run-cycledrill.sh` greps every run and
fails on `AMINETXDUO_CYCLE_ORPHAN_FATAL=1`. What is missing is a sighting, and
only real hardware can provide one.

**`tests/clients/run-argvexit.sh` removed 2026-07-31; it never completed a run.**
Under Amiberry the guest boots, `ToolsSmoke` starts, `DH0:tools.txt` reaches the
`===== SYS:ArgvExit =====` header and stops — `ArgvExit` never returns from
`SystemTagList()`. The toolchain was ruled out separately:
`fix-toolchain-crt0.py --check` reports `11 ok, 1 skipped` for the frame skew and
`2 call site(s) already push __argv by value`, the immune case. The 256 KB
per-invocation stack leak it was written for is covered by
`clients/dropbear/run-fsuae.sh -A`, which runs `dbclient` six times in one boot
with `AvailMem()` printed after each — that found the leak (266,368 bytes a run)
and proved the fix (0). **A harness that has never run looks like coverage and
is not.**