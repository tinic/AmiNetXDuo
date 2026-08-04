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
| 8200 §5 | IPv6 requires a link MTU of at least 1280 | `sana2_device.c:183-201` takes the device figure; `:624-634` applies `MTU=` downwards with no bound | `MTU=576` on an IPv6-enabled interface is accepted |
| 1122 §3.3.2 / 8504 §5.1 | MUST reassemble; EMTU_R ≥ 576 | `nx_ip_fragment_enable()` never called; drops at `nx_ipv4_packet_receive.c:640`, `nx_ipv6_process_fragment_option.c:95-99` | Inbound fragments dropped, both families |
| 9777 §6 | MLD reports MUST be sent for scope ≥ 2 | `nx_mld.h` is a 48-line stub; joins set a MAC filter only, `nx_ipv6_multicast_join.c:79-95` | Solicited-node groups are scope 2. Behind a snooping switch with an active querier, ND fails |
| 1122 §4.2.3.5 (MUST-23) | R2 for SYN ≥ 3 minutes | `nx_user.h:201,204` → ladder 1+2+4+…+64 = 127 s | 127 s < 180 s. Fix is `NX_TCP_MAXIMUM_RETRIES 7` → 255 s, at the cost of `connect()` blocking that long |
| 5961 §3, §4 | Challenge ACK required for in-window RST and SYN | `nx_tcp_socket_packet_process.c:234-248`, `:257-271`; RST at any sequence accepted when `RCV.WND==0` at `:165-167` | One in-window guess resets an established connection |
| 1122 §4.2.3.10 (MUST-57) | SYN to broadcast/multicast MUST be discarded | `nx_ipv4_packet_receive.c:529-550` → `nx_ip_dispatch_process.c:459-476`; only source is checked, `nx_tcp_packet_process.c:517-542` | Broadcast SYN answered. Composes with one half-open slot per listener (`socket.c:1729-1730`), pinning a port for 127 s |
| 1122 §4.2.3.4 (MUST-38) | Sender SWS avoidance | `nx_tcp_socket_send_internal.c:455-470`, no minimum-usable-window gate | Undersized segments are sent when the peer advertises small window increments. Nagle also absent |
| 1122 §4.1.3.5 | UDP demux MUST match the 4-tuple | `nx_udp_packet_receive.c:247` compares port only; no local-address field in `NX_UDP_SOCKET` | A `connect()`ed UDP socket accepts datagrams from any peer |
| 2181 §5.4.1 | AUTHORITY data must not be returned as answers | `nxd_dns.c:4803-4805`, cached under the RR's own owner at `:4866` | One response inserts an A record for a name never queried |
| 8504 §6.6 / 6724 §5 | RFC 6724 source selection MUST be implemented | `nxd_ipv6_interface_find.c:73-300` is a first-match walk | No candidate set, no policy table, none of Rules 1/2/3/6/7/8 |
| 5280 §4.2 | Unrecognized critical extension MUST be rejected | flag written `nx_secure_x509_extension_find.c:191`, read nowhere; lookup is by OID, never an enumeration | nameConstraints and any other critical extension silently ignored |
| 6125 §6.4.4 | MUST NOT match CN when a DNS-ID is present | `nx_secure_x509_common_name_dns_check.c:92-97` compares CN first and returns | Cert whose CN matches and SAN does not is accepted |
| 5280 §6.1.3 | Revocation | no call to the CRL code that exists | Stolen key usable indefinitely |
| 8017 §8.2.2 | PKCS#1 v1.5 verify re-encodes and compares the whole EM | `nx_secure_x509_pkcs7_decode.c:104` checks byte 1 only; `:113-121` accepts any nonzero padding; `:187-197` ignores trailing data; OID discarded by the caller | Signature forgery per Bleichenbacher 2006. The shipped CA bundle contains no small-exponent root (79 of 80 RSA roots use e=65537), so the attack has no target there; a privately added root is exposed |
| 5246 §6.2.3.2 | CBC IV MUST be unpredictable | `nx_secure_tls_record_payload_encrypt.c:186-188`, `:626-629`; our copy `src/tls/rfc7905/…:301-304`, `:691` | Previous ciphertext block reused. BEAST precondition |
| 9325 §4.5 | SHA-1/MD5 MUST NOT be used for signatures | `src/tls/ami_tls_crypto.c:1538-1540`, walked onto the wire at `nx_secure_tls_send_clienthello_extensions.c:341-345` | `0x0201/0x0203/0x0101` advertised; MD5-signed cert accepted. Ours, three lines |
| 7627 / 9325 §3.5 | Extended master secret MUST be supported | absent | Session resumption restores a master secret that was never bound to a handshake transcript |
| 5280 §6.1 | Path termination | `nx_secure_x509_certificate_chain_verify.c:86-160`, no counter, no visited set | Two cross-signed certs → unbounded loop, one signature verification per pass. Reasoned from code, not executed |
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
| `SO_RCVBUF` on TCP | calls a function compiled out without `NX_ENABLE_LOW_WATERMARK`; status discarded | `options.c:171-178` |
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
| `nx_user.h:636-645` | MLD unnecessary because link-local groups are "never forwarded" | snooping *filters*; RFC 9777 §6 requires reports for scope ≥ 2 |
| `nx_user.h:176-183` | ladder satisfies R2 | quotes the 100 s data rule, notes SYN uses the same counter, stops. MUST-23 is 180 s |
| `include/aminetxduo/in6.h:90-92` | "there are no raw IPv6 sockets here" | corrected `4d073c4` |
| `README.md:157-158` | certificates "properly checked" | no revocation, no critical-extension rejection, no EKU, no nameConstraints, RSA-MD5 in the table |
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

**TCP** — 9293 §3.10.7 acceptability (all four cases, wraparound-safe),
simultaneous open, RST generation §3.10.7.1, 2MSL 240 s, 5681 slow start and
congestion avoidance, fast retransmit and recovery, **6582 NewReno in full**
(`nx_tcp_socket_state_ack_check.c:472-509`), 6056 port randomisation
(Algorithm 1 over the IANA dynamic range off a SHA-256 DRBG), delayed ACK
200 ms, zero-window probe with backoff, 1122 §4.2.2.4 urgent-data receive,
MSS option handling. Landed 2026-08-02: 2018 SACK receive side, 6298 RTO with
Karn. Landed 2026-08-03: **2883 D-SACK receive side** — read 794 -> 985 KB/s
at 4 MB, wire retransmissions 234 -> 42, peer `TCPDSACKUndo` 0 -> 7. RFC 3708
(sender side, consuming D-SACK to undo a spurious retransmission) is absent and
applies to the write direction only. Landed 2026-08-04: 9293 §3.10.7.4
TIME-WAIT (a retransmitted FIN is acknowledged and 2MSL restarted) and 1337 §4
(a RST does not end TIME-WAIT).

**UDP** — 768 both directions including the IPv4 zero-checksum rule.

**IPv4/link** — 826 ARP with the 5227 §2.4 defence, martian-source filtering,
894 encapsulation, ICMP echo server with broadcast echo discarded,
**IGMPv2 in full** (Router Alert, TTL 1, report suppression, Leave).

**ICMP** — 1122 §3.2.2.1 and §4.1.3.3 (a Destination Unreachable reaches the
transport that drew it), 4443 §2.4(e.3) (no error for a packet sent to a
multicast group) and §2.4(f) (a token bucket bounds the rate errors are
originated at). Landed 2026-08-04.

**TCP initial window** — 5681 §3.1 (the RFC 3390 formula) is implemented on
both paths to ESTABLISHED: `nx_tcp_socket_state_syn_sent.c:157-165` and
`nx_tcp_socket_state_syn_received.c:119-133`. The `= mss` at
`nx_tcp_packet_process.c:762` is set when the SYN arrives and overwritten by
the second of those when the final ACK does, before the transition to
ESTABLISHED; the only segment sent in between is the SYN-ACK, which the
congestion window does not govern. Checked 2026-08-04 after a backlog row
claimed the listen path was out of step.

**IPv6** — 4291 §2.8 required address set, SLAAC with DAD at three probes,
NUD five-state machine, hop limit 255 on all ND, 5095 RH0 refusal, Parameter
Problem on unrecognised Next Header, 6980 and 8021 satisfied (we never
fragment). Landed 2026-08-03: **8201 Path MTU Discovery**, with §4's three
receive-side obligations — a report below 1280 is discarded rather than clamped
(`nx_icmpv6_process_packet_too_big.c`), the embedded source must be the address
the error arrived on per 4443 §2.4, and the estimate never increases in response
to a Packet Too Big (`nx_icmpv6_dest_table_find.c`). Retry interval 600 s
against a 5-minute minimum. Cost: +1,112 B of library, +36 B per `NX_IP`;
read throughput 368.0 -> 365.0 KB/s, inside a 2.4-3.0% within-arm spread.

**DNS/mDNS/DHCP/SNTP** — 5452 §9.1 (an answer is taken only from the server it
was asked of, carrying the question it answers), 2181 §8 (the TTL sign bit is
masked), 6762 §3 (.local and 254.169.in-addr.arpa never reach a unicast server,
in every build), 5227 §2.3 (the address is announced) and §2.4 (a conflict is
reported), 2131 T1/T2 renewal, 5452 §9.2 (16-bit ID + 14-bit port
= 30 bits, off the DRBG), compression-pointer handling with bounds checks and a
pointer cap, 6762 §11 source checks and §8/§9/§10.1 probe-conflict-goodbye,
6763 DNS-SD publication, 5905 §14 SNTP including the originate-timestamp echo.

**TLS** — 7905 ChaCha20-Poly1305, mandatory hostname verification, trust store
failing closed, 5746 secure renegotiation, `null` compression only (CRIME
closed), 8422 §5.11 peer key validation (invalid-curve closed), 8996/6176/7465
(1.0/1.1, SSLv2, RC4 all absent), 8017 §7.2.1 nonzero PS.

**Sockets** — 3493 v4-mapped handling, `if_nametoindex` family, `IPV6_V6ONLY`
enforced on `accept()`, `CMSG_*` with the NDK's broken `CMSG_NXTHDR` replaced,
`IPV6_PKTINFO` sticky and ancillary, `ICMP6_FILTER` applied on receive.

**Tools** — `telnet` 854/855 (reactive only, no BINARY, no Synch); `tftp` 1350
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
- CBC padding is not checked before the MAC; the MAC runs unconditionally. Timing signal is inverted relative to classic Lucky13 — valid padding is *faster*, by up to a record of SHA-256.
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
