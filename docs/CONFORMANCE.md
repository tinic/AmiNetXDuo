# Standards conformance

Status against the RFCs that apply. Work items are in `BACKLOG.md`.

Surveyed 2026-08-02 against current documents: TCP against 9293, HTTP against
9110-9112, MLD against 9777, temporary addresses against 8981.

Obligation levels are quoted from the cited text, not paraphrased. "Absent"
means a search was run. Upstream = vendored NetX Duo (fixes go to the fork);
ours = `src/`, `port/`, `include/`.

## Violations

| RFC | Requirement | Cite | Effect |
|---|---|---|---|
| 8200 §5 | IPv6 requires a link MTU of at least 1280 | fixed 2026-08-04 | was: `MTU=576` accepted on an IPv6 interface. IPv6 is now not started on a link below 1280; IPv4, which has no floor, stays |
| 1122 §3.3.2 / 8504 §5.1 | MUST reassemble; EMTU_R ≥ 576 | fixed 2026-08-04, `netstack.c:687` | was: every inbound fragment dropped, both families. Enabling it alone was a denial of service, nothing bounded concurrent reassemblies, so 16 minimum-size fragments (~640 bytes of wire traffic) emptied the pool of a 1 MB machine for 60 s. A fragment is now refused once the pool is at or below half. +2,384 bytes |
| 9777 §6 | MLD reports MUST be sent for scope ≥ 2 | `nx_mld.h` is a 48-line stub; joins set a MAC filter only, `nx_ipv6_multicast_join.c:79-95` | Solicited-node groups are scope 2. Behind a snooping switch with an active querier, ND fails |
| 1122 §4.2.3.5 (MUST-23) | R2 for SYN ≥ 3 minutes | fixed 2026-08-04 | was: 127 s. SYN now has its own retry budget, `NX_TCP_SYN_MAXIMUM_RETRIES`, and R2 is 191 s, verified on the host. `connect()` to a dead host takes about three minutes to fail instead of about two |
| 5961 §3, §4, §3.2 | Challenge ACK required for in-window RST and SYN, **and rate-limited** | fixed 2026-08-04, `nx_tcp_socket_packet_process.c` RST step 2 and SYN step 3; budget on `NX_IP`, `NX_TCP_CHALLENGE_ACK_LIMIT` in `nx_tcp.h`, refilled in `nx_tcp_periodic_processing.c` | was: one in-window guess reset an established connection, and every out-of-window segment drew an unthrottled ACK. **§5 (blind data injection) is deliberately not done**, it needs MAX.SND.WND per socket and a second decision inside `_nx_tcp_socket_state_ack_check()`. `rfc5961.drill`, 7 cases, 62 checks |
| 1122 §4.2.3.10 (MUST-57) | SYN to broadcast/multicast MUST be discarded | fixed `1e0e80f0`, `nx_tcp_packet_process.c:151-177` | was: broadcast SYN answered. The half-open composition is also bounded, `bsd_listen_rearm()` tops the parked list up to `as_Backlog` |
| 1122 §4.2.3.4 (MUST-38) | Sender SWS avoidance | `nx_tcp_socket_send_internal.c:455-470`, no minimum-usable-window gate | Undersized segments are sent when the peer advertises small window increments. Nagle also absent |
| 2181 §5.4.1 | AUTHORITY data must not be returned as answers | fixed 2026-08-04, `nxd_dns.c:4916-4918`, `:5402-5404` | was: one response could insert an A record for a name never queried. Restricted to the ANSWER section; a CNAME chain's A record lives there, so no CNAME-hosted name is affected |
| 8504 §6.6 / 6724 §5 | RFC 6724 source selection MUST be implemented | `nxd_ipv6_interface_find.c:73-300` is a first-match walk | No candidate set, no policy table, none of Rules 1/2/3/6/7/8 |
| 5280 §4.2 | Unrecognized critical extension MUST be rejected | flag written `nx_secure_x509_extension_find.c:191`, read nowhere; lookup is by OID, never an enumeration | nameConstraints and any other critical extension silently ignored |
| 6125 §6.4.4 | MUST NOT match CN when a DNS-ID is present | fixed 2026-08-04 | was: CN compared first and returned, so a certificate whose CN matched and SAN did not was accepted. The subjectAltName walk was also mis-stepping past every entry that was not a dNSName, which is what kept the precedence bug from showing |
| 5280 §6.1.3 | Revocation | no call to the CRL code that exists | Stolen key usable indefinitely |
| 8017 §8.2.2 | PKCS#1 v1.5 verify re-encodes and compares the whole EM | fixed 2026-08-04, `nx_secure_x509_pkcs7_decode.c:85-137`, `:150-157`, `:206-211` | was: Bleichenbacher 2006 signature forgery against a small-exponent root. Byte 0, the padding bytes, the padding length and trailing data are all checked now. The discarded OID is not exploitable once the padding is exact, see BACKLOG *Withdrawn* |
| 5246 §6.2.3.2 | CBC IV MUST be unpredictable | fixed 2026-08-04, `src/tls/rfc7905/nx_secure_tls_record_payload_encrypt.c:684-700` + vendored copy | was: previous ciphertext block reused, the BEAST precondition |
| 9325 §4.5 | SHA-1/MD5 MUST NOT be used for signatures | fixed 2026-08-04, `ami_tls_crypto.c:1528-1552` | was: `0x0201/0x0203/0x0101` advertised and an MD5-signed cert accepted. **This can stop a connection that works today**, typically to an old device on a local network. Roots in `DEVS:Internet/certificates` are unaffected whatever they are signed with |
| 7627 / 9325 §3.5 | Extended master secret MUST be supported | absent | Session resumption restores a master secret that was never bound to a handshake transcript |
| 5280 §6.1 | Path termination | fixed 2026-08-04, `nx_secure_x509_certificate_chain_verify.c:93-100`, `:196` | was: two cross-signed certificates looped without bound, one signature verification per pass. Now `NX_SECURE_X509_CHAIN_TOO_LONG`, with a host test that did not return before the cap |
| 1122 §4.2.3.3 | Receiver SWS: announce a window only when it reopens by min(MSS, RCV.BUFF/2) | fixed 2026-08-04, `nx_tcp_socket_receive.c`, `nx_tcp_fast_periodic_processing.c` | was: only RCV.BUFF/2 applied, and only on the receive path. On the 33 KB window this stack advertises the smallest update it would send was ~16 KB, so a peer whose window had run down to a few hundred bytes stayed there until half the buffer was read. The floor went onto the receive path first and was worth nothing until the 200 ms delayed-ACK timer got it too, caught by `rwndupdate.drill` s02 |
| 9293 §3.10.7.4 | An unacceptable segment MUST draw an acknowledgment | fixed 2026-08-04, `nx_tcp_socket_packet_process.c` | was: a retransmission of already-acknowledged data drew nothing when the receive queue was non-empty, so a lost ACK on a request/response exchange deadlocked until R2. `d06_no_dsack_without_permission` asserted the opposite and was corrected with it |
| 9293 §3.10.7.1 | A RESET MUST NOT be answered with a RESET | fixed 2026-08-04, `nx_tcp_socket_packet_process.c` option branch and `nx_tcp_packet_process.c:479-489` | was: two malformed-option paths returned before the RST-bit test. The second was not in the row that led here |
| 6864 §4.2 | The IPv4 ID MUST NOT be reused on a non-atomic datagram | fixed 2026-08-04, `nx_tcp_socket_retransmit.c:371` | was: `nx_packet_identical_copy` was set and never cleared, so a packet that went out identical once reused the original ID on every later retransmission, where the ACK and window had moved, which is the ordinary case. The genuinely-identical case §4.1 permits is untouched |
| 8106 | RDNSS in a router advertisement | fixed 2026-08-04, `nx_icmpv6_process_ra.c`, `netstack_dns.c` | was: absent, so an IPv6-only link yielded addresses and no resolver |
| 7230 §3.3.3 | `Transfer-Encoding` with `Content-Length` MUST be rejected | fixed 2026-08-04, `httpframe.c:98`, `httpd.c:4480` | was: both accepted with correct precedence, which is the whole of request smuggling. `Transfer-Encoding` was also matched by a 7-character prefix, so `gzip, chunked` fell through to Content-Length framing and silently corrupted the upload while `chunkedX` matched. **This rejects requests some older clients send** |
| 4918 §9.10.6 | A lock token MUST be unguessable | fixed 2026-08-04, `httpd.c:2405` | was: a 32-bit LCG. Now 128-bit and non-repeating |
| 3542 §3.1 | `IPV6_CHECKSUM` on a non-raw socket must fail | fixed 2026-08-02, `4d073c4` | was: collided with `IPV6_V6ONLY_LINUX` at 26 |
| 3493 §5.2 | `0 ≤ x ≤ 255` uses x | fixed 2026-08-02, `4d073c4` | was: `IPV6_MULTICAST_HOPS 0` coerced to 1 and sent |
| 9110 §15.2 | Client must parse and discard 1xx | fixed 2026-08-02, `4d073c4` | was: interim response taken as final. Note `fetch` sends HTTP/1.0, which a conforming server must not send 1xx to |
| 3986 §5.2.2 | Relative reference resolution | fixed 2026-08-02, `4d073c4` | was: relative `Location` parsed as absolute |

## Accepted and ignored

Not detectable by the caller.

| Interface | Behaviour | Cite |
|---|---|---|
| `SBTC_CAN_SHARE_LIBRARY_BASES` | never written, never read | `library.c:270` |
| `SO_REUSEADDR/REUSEPORT/BROADCAST/OOBINLINE/SNDBUF` | success, no effect | `options.c:105-122`, `:148-189` |
| `SO_RCVBUF` on TCP | recorded and answered, never applied: the only knob NetX Duo offers is `nx_tcp_socket_receive_queue_max_set()`, whose whole body is inside `NX_ENABLE_LOW_WATERMARK`, which this port does not define. The advertised window is sized from the packet pool at create time and is not settable afterwards | `options.c:313-344` |
| `IPV6_DSTOPTS` (BSD 50) | taken as `IPV6_PKTINFO`; reads `struct in6_pktinfo` from the caller's buffer | `cmsg.c` Linux aliases 49/50/51 collide with `IPV6_HOPOPTS`/`DSTOPTS`/`RTHDR`. Unfixed |
| `DAV: 1,2` | class claim; §18.1 needs all Class 1 MUSTs (PROPFIND body gap) and §18.2 needs §6-§10 (LOCK on unmapped URL, Depth-0 collection) | `httpd.c:3132`, `:3219`. Advertising `DAV: 1` is honest but Finder reads it as read-only |

Refused rather than ignored, which is correct: unknown ancillary types
(`cmsg.c:545-550`), RFC 3542 extension-header options (`in6.c:310`), out-of-mask
`ai_flags` (`addrinfo.c:319`), sticky `IPV6_HOPLIMIT` (`cmsg.c:885-887`),
`ICMP6_FILTER` on a non-ICMPv6 socket (`cmsg.c:805-807`).

## Incorrect in-tree claims

| Location | Claim | Reality |
|---|---|---|
| `netstack_ipv6.c:565-568`, `raw.c:455-456`, `options.c:613-614` | "RFC 6724 selection routine" | first-match walk, one on-link test, `break` |
| `in6.h:152-154`, `mcast.c:18-20` | MLD unnecessary because link-local groups are "never forwarded" | snooping *filters*; RFC 9777 §6 requires reports for scope ≥ 2. Corrected in place 2026-08-04. The claim was never in `nx_user.h:636-645`, where the row said it was |
| `nx_user.h:176-183` | ladder satisfies R2 | quotes the 100 s data rule, notes SYN uses the same counter, stops. MUST-23 is 180 s |
| `include/aminetxduo/in6.h:90-92` | "there are no raw IPv6 sockets here" | corrected `4d073c4` |
| `README.md:157-158` | certificates "properly checked" | still overstated, but less so since 2026-08-04: RSA-MD5/SHA-1 are gone, the chain terminates, the signature encoding is strict and a minimum modulus applies. Revocation, critical-extension rejection, EKU and nameConstraints remain absent |
| `ami_random.c:566-590` | clock credit conditional on the seconds field being wall time | guard tests non-zero; on a no-RTC machine uptime is non-zero a second after boot, so 8 bits are credited in the case the comment excludes |
| `netstack_dns.c:228` | RFC 6762 §6.7 | §6.7 is Legacy Unicast Responses; the rule is §3 |
| `nx_user.h:307` | "no SACK in the vendored tree" | receive side landed 2026-08-02 |
| `nx_user.h:503-505` | honours the server's TTL | not under repeated lookup; no sign-bit rule |
| `sntp.c:59` | all RFC 4330 §5 checks present | §5 check 5 (root delay/dispersion) absent |
| `tls_conn.c:717-721` | declines to defend against truncation | the `CLOSE_NOTIFY_RECEIVED` arm is unreachable in a non-DTLS build, so the cases cannot be told apart |

## Declined, with cost

| Item | Reason |
|---|---|
| TLS 1.3 | nx_secure's 1.3 defines only AES-GCM suites. GHASH on 68k is a bit-serial GF(2^128) multiply: 344.6 ms/KB against CBC's 21.9, approximately 2.9 KB/s. Precondition for reconsidering: a ChaCha20 suite in the vendored 1.3 tables |
| AES-GCM in TLS 1.2 | same cost; no server takes GCM but neither ChaCha20-Poly1305 nor CBC |
| Certificate date checking when the clock is implausible | `tls_time.c:103-104`, window 2026+50y. Without it, a machine with a discharged clock battery reaches no HTTPS site. Reported via `TLSInfo()`. Cost: no bound on the useful lifetime of a leaked key |
| Parsed root set | 120 roots require approximately 30 KB parsed plus 125 KB DER and 120 ASN.1 walks per page load. The lazy store is keyed on FNV-1a of the full issuer DER because four Mozilla roots share the CN "GlobalSign" |
| Session resumption in cleartext | against 7 s (RSA) / 23 s (ECDSA) full handshakes. Disabled with `TLSA_NoResume` or `TLSA_SessionFile ""`. Not accounted for in that trade: no `SetProtection` (`tls_resume.c:718`), 24 h cap never fires without an RTC (`:439`) |
| IDNA | AmigaOS provides no Unicode input path to a hostname; the `xn--` form passes through unchanged |
| RFC 4361 client identifier | option 61 exists to obtain the same DHCP lease as Roadshow on the same NIC; a DUID would prevent that |
| RFC 8985 RACK-TLP | retransmission re-headers packets in place, so per-segment send times do not exist |
| RFC 6928 IW10 | 14 KB initial window against a packet pool bounded at 256 packets |
| IP source routing | RFC 7126/BCP 186 makes dropping it the recommendation |
| ICMP Redirect | Established man-in-the-middle vector; ignoring it is current practice |
| RFC 1042/802.3 receive | no remaining senders on Ethernet |
| IGMPv3, RFC 4191, RFC 7371, DHCPv6, RFC 3396 | see `BACKLOG.md` |

## Verified conformant

**TCP**, 9293 §3.10.7 acceptability (all four cases, wraparound-safe),
simultaneous open, RST generation §3.10.7.1, 2MSL 240 s, 5681 slow start and
congestion avoidance, fast retransmit and recovery, **6582 NewReno in full**
(`nx_tcp_socket_state_ack_check.c:472-509`), 6056 port randomisation
(Algorithm 1 over the IANA dynamic range off a SHA-256 DRBG), delayed ACK
200 ms, zero-window probe with backoff, 1122 §4.2.2.4 urgent-data receive,
MSS option handling. Landed 2026-08-02: 2018 SACK receive side, 6298 RTO with
Karn. Landed 2026-08-03: **2883 D-SACK receive side**, read 794 -> 985 KB/s
at 4 MB, wire retransmissions 234 -> 42, peer `TCPDSACKUndo` 0 -> 7. RFC 3708
(sender side, consuming D-SACK to undo a spurious retransmission) is absent and
applies to the write direction only. Landed 2026-08-04: 9293 §3.10.7.4
TIME-WAIT 2MSL restart, 1337 §4 (a RST does not end TIME-WAIT), and 1122
§4.2.2.17 persist (see below). Landed 2026-08-04 in the sweep: 5961 §3/§4/§3.2
challenge acknowledgments, 9293 §3.10.7.4 and §3.10.7.1, 1122 §4.2.3.3
receiver SWS, MUST-23 (R2 191 s), and 6864 §4.2.

"Zero-window probe with backoff" on this line was **half false** until
2026-08-04, and read as conformant for as long as it was here. The probe byte
was armed by both paths that refuse a send and no timer was ever started to
carry it: `_nx_tcp_fast_periodic_processing()` only examines a socket whose
`nx_tcp_socket_timeout` is running, and `nx_tcp_socket_state_ack_check.c:651`
clears that timeout when the transmit queue drains under a non-zero window. A
receiver acknowledging everything before advertising zero therefore left no
timer, and the ACK carrying the zero window releases no packet, so it returned
at `:581` without starting one. The connection then waited on a window update
that one lost segment means never arrives, the exact failure §4.2.2.17 exists
to prevent. It looked conformant because the case usually exercised is the one
where the ACK that drains the queue is also the one advertising zero, which
leaves the timer running. Fixed in `nx_tcp_socket_send_internal.c` by starting
the timer where the probe is armed; `zerowindow.drill` is 7/7, 87 checks.

Corrected 2026-08-04 by a derived packetimpact case: the *acknowledgment* half
of §3.10.7.4 was never missing. A retransmitted FIN is out of window by then,
so `nx_tcp_socket_packet_process.c:233` answers it on the unacceptable-segment
path and returns before the state switch is reached. The commit that added the
TIME-WAIT arm said the peer got nothing back; it did. What that commit
genuinely adds is the 2MSL restart, for a FIN still inside the window.

**UDP**, 768 both directions including the IPv4 zero-checksum rule.

**IPv4/link**, 826 ARP with the 5227 §2.4 defence, martian-source filtering
(1122 §3.2.1.3; the check existed but its define was set nowhere, so it was
dead code in every shipped build until 2026-08-04, the claim on this line was
false for as long as it has been here),
894 encapsulation, ICMP echo server with broadcast echo discarded,
**IGMPv2 in full** (Router Alert, TTL 1, report suppression, Leave).

**ICMP**, 1122 §3.2.2.1 and §4.1.3.3 (a Destination Unreachable reaches the
transport that drew it), 4443 §2.4(e.3) (no error for a packet sent to a
multicast group) and §2.4(f) (a token bucket bounds the rate errors are
originated at). Landed 2026-08-04.

**TCP initial window**, 5681 §3.1 (the RFC 3390 formula) is implemented on
both paths to ESTABLISHED: `nx_tcp_socket_state_syn_sent.c:157-165` and
`nx_tcp_socket_state_syn_received.c:119-133`. The `= mss` at
`nx_tcp_packet_process.c:762` is set when the SYN arrives and overwritten by
the second of those when the final ACK does, before the transition to
ESTABLISHED; the only segment sent in between is the SYN-ACK, which the
congestion window does not govern. Checked 2026-08-04 after a backlog row
claimed the listen path was out of step.

**IPv6**, 4291 §2.8 required address set, SLAAC with DAD at three probes,
NUD five-state machine, hop limit 255 on all ND, 5095 RH0 refusal, Parameter
Problem on unrecognised Next Header, 6980 and 8021 satisfied (we never
fragment). Landed 2026-08-03: **8201 Path MTU Discovery**, with §4's three
receive-side obligations, a report below 1280 is discarded rather than clamped
(`nx_icmpv6_process_packet_too_big.c`), the embedded source must be the address
the error arrived on per 4443 §2.4, and the estimate never increases in response
to a Packet Too Big (`nx_icmpv6_dest_table_find.c`). Retry interval 600 s
against a 5-minute minimum. Cost: +1,112 B of library, +36 B per `NX_IP`;
read throughput 368.0 -> 365.0 KB/s, inside a 2.4-3.0% within-arm spread.

**DNS/mDNS/DHCP/SNTP**, 5452 §9.1 (an answer is taken only from the server it
was asked of, carrying the question it answers), 2181 §8 (the TTL sign bit is
masked), 6762 §3 (.local and 254.169.in-addr.arpa never reach a unicast server,
in every build), 5227 §2.3 (the address is announced) and §2.4 (a conflict is
reported), 2131 T1/T2 renewal, 5452 §9.2 (16-bit ID + 14-bit port
= 30 bits, off the DRBG), compression-pointer handling with bounds checks and a
pointer cap, 6762 §11 source checks and §8/§9/§10.1 probe-conflict-goodbye,
6763 DNS-SD publication, 5905 §14 SNTP including the originate-timestamp echo.

**TLS**, 7905 ChaCha20-Poly1305, mandatory hostname verification, trust store
failing closed, 5746 secure renegotiation, `null` compression only (CRIME
closed), 8422 §5.11 peer key validation (invalid-curve closed), 8996/6176/7465
(1.0/1.1, SSLv2, RC4 all absent), 8017 §7.2.1 nonzero PS.

**Sockets**, 3493 v4-mapped handling, `if_nametoindex` family, `IPV6_V6ONLY`
enforced on `accept()`, `CMSG_*` with the NDK's broken `CMSG_NXTHDR` replaced,
`IPV6_PKTINFO` sticky and ancillary, `ICMP6_FILTER` applied on receive.

**Tools**, `telnet` 854/855 (reactive only, no BINARY, no Synch); `tftp` 1350
octet mode, Sorcerer's Apprentice avoided; `whois` 3912 complete; `ssh`
vendored dropbear unpatched, 4250-4254; `traceroute` names no RFC and
implements none by choice.

## Updates post survey

- `AI_CANONNAME` is honoured; pointing it at nodename is 3493 §6.1's fallback.
- One address per family is a quality gap; 3493 requires "one or more results".
- `getnameinfo` numeric fallback is permitted by §6.2.
- `gethostbyname` IPv4-only is what 3493 asks for.
- Oversize UDP returns `EMSGSIZE` (`transfer.c:570-572`); only raw drops silently.
- Prefix expiry invalidates SLAAC addresses (`nx_ipv6_prefix_list_delete_entry.c:105-140`). Missing is the preferred lifetime and DEPRECATED state.
- 5227 DHCPDECLINE is **not** present.
- 424 in DELETE/COPY multistatus is wrong; §9.6.1 and §9.8.3 say SHOULD NOT. It belongs in PROPPATCH only.
- Nothing in 4918 requires 422.
- Absent `Depth` on LOCK means infinity; storing infinity is correct.
- CBC padding is not checked before the MAC; the MAC runs unconditionally. Timing signal is inverted relative to classic Lucky13, valid padding is *faster*, by up to a record of SHA-256.
- 6582 NewReno is implemented.

Section numbers corrected 2026-08-02: `getnameinfo` is 3493 **§6.2**; reference
resolution is 3986 **§5.2.2**; the fragment rule is 9110 **§7.1**; the Host-port
MUST is 9112 **§3.2**.

## Constraints on future work

- A DNS bailiwick check must be implemented together with CNAME chain following.
  CNAME processing is compiled out, and the following A record is currently
  accepted only because no owner-name check exists; adding the check alone would
  fail every CNAME-hosted name.
- RFC 4086 contains no RFC 2119 keywords. The normative obligation is RFC 5246
  §D.1. The DRBG construction meets it; the seeding does not.
- RFC 8659 §1.1 forbids using CAA in validation. Having no CAA code is correct.
- RFC 4193 ULAs need no internet-layer handling. The ULA problem is 6724's
  policy table.
- `ndk-include` is Latin-1. `grep -r` reads those files as binary and returns no
  matches. Use `LC_ALL=C grep -a`; an empty result is otherwise indistinguishable
  from absence.
