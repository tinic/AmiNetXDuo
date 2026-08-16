# Where we do not conform

Open deviations only. Obligation levels are quoted from the cited text, not
paraphrased. Upstream = vendored NetX Duo, where a fix goes to the fork; ours =
`src/`, `port/`, `include/`. Work items are in `BACKLOG.md`.

Every row is self-contained. A row that stops being true is deleted, not
annotated — git has the history.

## Violations

| RFC | Requirement | Where | Effect |
|---|---|---|---|
| 3810 §6.2 / 2710 §3 | A received MLD message without a Hop-by-Hop Router Alert MUST be discarded | `nx_mld_packet_process.c` checks hop limit 1 and a link-local source, and nothing else. `_nx_ipv6_process_hop_by_hop_option()` skips the option and records nothing, and `NX_PACKET` has no field to carry the answer forward | A query forged from on-link without the option is answered. The hop-limit and source-scope checks are what keep off-link senders out |
| 5280 §4.2 | Unrecognized critical extension MUST be rejected | flag written at `nx_secure_x509_extension_find.c:191`, declared at `nx_secure_x509.h:611`, read nowhere | nameConstraints and every other critical extension silently ignored |
| 5280 §6.1.3 | Revocation | `nx_secure_x509_crl_revocation_check.c` is built (`nx_secure/CMakeLists.txt:207`) and called from nothing | A stolen key stays usable indefinitely |
| 7627 / 9325 §3.5 | Extended master secret MUST be supported | absent from the vendored tree; `src/tlslib/tls_resume.c:46-51` records that nx_secure does not implement it | Session resumption restores a master secret that was never bound to a handshake transcript |

## Accepted, and not detectable by the caller

| Interface | Behaviour | Where |
|---|---|---|
| `SBTC_CAN_SHARE_LIBRARY_BASES` | recorded into `sb_CanShareBases` and exposed SBT_RW, never read | `bsdsocket_internal.h:562-564`, `errno.c:417-418` |
| `SO_BROADCAST`, `SO_OOBINLINE`, and `SO_REUSEPORT`'s share-arrivals half | success, no effect. A broadcast `sendto()` without `SO_BROADCAST` succeeds where 4.4BSD returns `EACCES` | `options.c:221-228`, `:266-268` |
| `SO_RCVBUF` on TCP | recorded and answered, never applied: the arm is inside `#ifdef NX_ENABLE_LOW_WATERMARK`, which this port does not define. The advertised window is sized from the packet pool at create time and is not settable afterwards. UDP is applied | `options.c:319-331` |
| `DAV: 1,2` | class claim. §18.1 needs all Class 1 MUSTs (PROPFIND body gap) and §18.2 needs §6-§10 (LOCK on unmapped URL, Depth-0 collection). Advertising `DAV: 1` would be honest, but Finder reads that as read-only | `httpd.c:3449`, `:3536` |

Refused rather than silently ignored, which is correct: unknown ancillary types
(`cmsg.c:544-547`), RFC 3542 extension-header options (`in6.c:360`, `:392`),
out-of-mask `ai_flags` (`addrinfo.c:373`), sticky `IPV6_HOPLIMIT`
(`cmsg.c:866-867`), `ICMP6_FILTER` on a non-ICMPv6 socket (`cmsg.c:787-788`).

## Comments in the tree that claim more than the code does

| Location | Claim | Reality |
|---|---|---|
| `README.md:182-183` | certificates "properly checked" | overstated: revocation, critical-extension rejection, EKU and nameConstraints are all absent |
| `ami_random.c:564-577` | clock credit conditional on the seconds field being wall time | the guard at `:590` tests non-zero. On a no-RTC machine uptime is non-zero a second after boot, so 8 bits are credited in the case the comment excludes |
| `netstack_dns.c:719` | RFC 6762 §6.7 | §6.7 is Legacy Unicast Responses; the rule is §3, which `:757` and `:929` cite correctly |
| `sntp.c:62-63` | all RFC 4330 §5 checks present | `sntp_validate()` (`:485-530`) checks mode, version, LI, stratum, transmit and originate. §5 check 5, root delay and dispersion, is absent |

## Declined, with the cost of declining

| Item | Reason |
|---|---|
| AES-GCM in TLS 1.2 | GHASH on 68k is a bit-serial GF(2^128) multiply: 344.6 ms/KB against AES-CBC's 21.9. No server takes GCM but neither ChaCha20-Poly1305 nor CBC |
| Certificate date checking when the clock is implausible | `tls_time.c:57`, `:62`, `:111-112`; window is 2026-01-01 + 50 years, outside which dates are not checked. Without it a machine with a discharged clock battery reaches no HTTPS site. Reported through `TLSInfo()`. Cost: no bound on the useful lifetime of a leaked key |
| Parsed root set | 119 roots would need ~30 KB parsed plus 125 KB DER and 119 ASN.1 walks per page load. The lazy store is keyed on FNV-1a of the full subject Name DER (`tls_store.c:23`) because four Mozilla roots share the CN "GlobalSign" |
| Nagle | RFC 1122 §4.2.3.4 says a TCP SHOULD implement it. The vendored tree has none and none is added: it buys a frame count with a round trip added to every small write that follows an unacknowledged one, on a machine whose small writes are a Shell session. `TCP_NODELAY` refuses 0 rather than report a coalescing that does not happen (`options.c:439`). The sender silly-window rule beside it is a different rule and does not stand in for this one: it withholds on the peer's WINDOW, never on the size of the write, which `tests/netstack/host/test_tcp_sws_host.c` asserts with one byte against 512 unacknowledged | `_nx_tcp_socket_sws_send_permitted` |
| RFC 6724 Rule 5.5 | Prefer a source in a prefix the next hop advertised. `NX_IPV6_PREFIX_ENTRY` holds the prefix, its length, its lifetime and its L bit, and `nx_icmpv6_process_ra.c` does not record which router put it there, so the information is never written rather than merely unread. Cost: on a link with two advertising routers and a prefix from each, the source may be the one the chosen next hop will not accept. The table holds two routers | `src/ipv6/ipv6_srcsel.c` |
| `SetProtection` on the session file | `src/tlslib/tls_resume.c` writes it with default protection bits. Master secrets and tickets sit in the clear on disk, so anyone taking the disk can decrypt captured traffic for resumed sessions |
| IDNA | AmigaOS provides no Unicode input path to a hostname; the `xn--` form passes through unchanged |
| `ChangeRouteTagList`'s metrics and flags | `RTM_CHANGE` on the routing socket changes "Metrics, Flags, or Gateway". The five `RTA_*` tags express addresses only, so two thirds of that is unreachable and is not refused by name because no tag can carry one. The vector itself has no published autodoc anywhere; ours is derived from the sfd prototype, the tag set and the `-route-` page, and is documented as such in `src/bsdsocket/routing.c` |
| `ChangeRouteTagList` across interfaces | A next hop moving to a different interface is delete-then-add, not atomic. `nx_ip_static_route_add.c:126` leaves `nx_ip_routing_entry_ip_interface` alone on the update path while `nx_ip_route_find.c:157` reads it, so an in-place change would keep leaving by the old card. Only reachable on a multi-interface machine |
| `S2EVENT_SOFTWARE` | SANA-II defines it; `anxnet.device` refuses it at `S2_ONEVENT` with `S2WERR_BAD_EVENT` rather than accepting a request it would never complete. `cnet.device` refuses the same one (`cnetdevice.asm:2157`) and every stack in the field works with cnet, so the accepted set is deliberately cnet's seven. Untested against Roadshow, AmiTCP and Miami |
| RFC 4361 client identifier | DHCP option 61 is emitted from the MAC (`netstack.c:1443-1450`) so the machine gets the same lease Roadshow would on the same NIC. A DUID would prevent that |
| RFC 8985 RACK-TLP | retransmission re-headers packets in place, so per-segment send times do not exist |
| RFC 6928 IW10 | a 14 KB initial window against a packet pool bounded at 256 packets |
| IP source routing | RFC 7126 / BCP 186 makes dropping it the recommendation |
| ICMP Redirect | an established man-in-the-middle vector; ignoring it is current practice |
| RFC 1042 / 802.3 receive | no remaining senders on Ethernet |
| IGMPv3, RFC 4191, RFC 7371, DHCPv6, RFC 3396 | see `BACKLOG.md` |

## Constraints on anything built next

- A DNS bailiwick check must land together with CNAME chain following. CNAME
  processing is compiled out, and the following A record is accepted only
  because no owner-name check exists; adding the check alone would fail every
  CNAME-hosted name.
- RFC 4086 contains no RFC 2119 keywords. The normative obligation is RFC 5246
  §D.1. The DRBG construction meets it; the seeding does not.
- RFC 8659 §1.1 forbids using CAA in validation. Having no CAA code is correct.
