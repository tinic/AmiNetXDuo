# Standards conformance

What this stack is measured against, where it stands, and what has been
declined on purpose. Five parallel surveys, 2026-08-02, one per layer, each
reading the source against the RFC text rather than against our own comments.

The work list lives in `BACKLOG.md`. This file is the map: what applies, what
the status is, and — the part a defect-driven process never produces — **what
was looked at and found fine**, so the next person knows where not to look.

## How to read it

**Status** is one of implemented, partial, absent, or N/A. Every "absent" here
came from a search that was actually run, not from an inference. **Obligation**
is the level in the cited text, and it is quoted rather than paraphrased,
because MUST and SHOULD carry different arguments and summaries blur them.

**N/A is stated with a reason**, never by omission. A survey that silently drops
what does not apply is indistinguishable from one that missed it.

**Upstream vs ours** is tracked throughout. Vendored NetX Duo behaviour is an
upstream contribution; ours is local work. Seven fork branches exist, so the
split is directly actionable rather than academic.

Use the current document. Where an RFC has been obsoleted the survey went
against the replacement — TCP against **9293** not 793, HTTP against
**9110-9112** not 723x, MLD against **9777** not 3810, temporary addresses
against **8981** not 4941.

## Where the stack stands, by layer

### Link and IPv4

Solid: ARP including the RFC 5227 §2.4 defence, martian-source filtering,
RFC 894 encapsulation, the ICMP echo server with broadcast echo correctly
discarded, IGMPv2 in full — Router Alert, TTL 1, report suppression, Leave.

Missing and it matters: **no fragment reassembly** (RFC 1122 §3.3.2 MUST,
EMTU_R ≥ 576), and **inbound ICMP errors reach nothing** — the dispatcher
handles echo request and echo reply and releases the rest, so RFC 1122 §3.2.2.1
and §4.1.3.3 are both plainly violated. A connected UDP socket to a closed port
blocks for its full timeout where BSD returns `ECONNREFUSED` at once.

Declined with reasons: IP source routing (RFC 7126/BCP 186 makes dropping it
the current recommendation), ICMP Redirect (a classic MITM vector; ignoring it
is the modern default), RFC 1042/802.3 receive (senders are extinct), IGMPv3.

### IPv6

Solid: SLAAC with DAD at three probes, the full five-state NUD machine, hop
limit 255 enforced on every ND message, RH0 refused per RFC 5095, the required
address set of RFC 4291 §2.8, Parameter Problem on an unrecognised Next Header.

**The sharpest finding in the whole survey**: RFC 8201 §1 says a node not
implementing Path MTU Discovery *must use the IPv6 minimum link MTU as the
maximum packet size*. **We do neither** — PMTUD is off by decision, ICMPv6
Packet Too Big is not even dispatched, and we send up to the full 1500. Any
path narrower than the local link is a black hole presenting as a hang. The RA
MTU option, which is free, is discarded on the same `#ifdef`.

Also open: no MLD (see the false-claims register), no fragment reassembly, no
RFC 7559 Router Solicitation backoff, no RFC 8106 RDNSS, and an A-bit test
nested inside the L-bit test so a prefix advertised A=1 L=0 forms no address at
all.

Declined: privacy addresses (RFC 8981) and opaque IIDs (RFC 7217), DHCPv6,
RFC 4191 route preferences, RFC 7371 multicast flags.

### TCP

Better than expected. RFC 9293 §3.10.7's four-case acceptability test is
correct and wraparound-safe; simultaneous open works; RST generation follows
§3.10.7.1; 2MSL is a conformant 240 s. RFC 5681 slow start and congestion
avoidance are faithful, fast retransmit and fast recovery are complete, and
**RFC 6582 NewReno is fully implemented** — which had been assumed absent.
RFC 6056 port randomisation is Algorithm 1 over the IANA dynamic range off a
SHA-256 DRBG. Delayed ACK is 200 ms, inside §4.2.3.2's half-second.

Landed 2026-08-02: RFC 2018 SACK (receive side), RFC 6298 RTO estimation with
Karn, and the duplicate-acknowledgment correctness fix.

Open and ranked in `BACKLOG.md`: the SACK **send** side (we advertise
SACK-Permitted and discard every block the peer sends), broadcast SYN
acceptance composing with a single half-open slot, the whole of RFC 5961, sender
silly-window avoidance, and restart-after-idle.

Declined: ECN, RFC 7413 Fast Open, RFC 8985 RACK-TLP (structurally
unsupported — retransmit re-headers packets in place, so there are no
per-segment send times), RFC 6928 IW10.

### UDP

RFC 768 is correct in both directions including the IPv4 zero-checksum rule.
RFC 8085's congestion guidance is N/A in practice — our UDP is DNS, DHCP and
mDNS, all small and low rate.

Open: **demultiplexing ignores the 4-tuple** (RFC 1122 §4.1.3.5), so a
`connect()`ed UDP socket accepts datagrams from any peer — which is a live
concern for the resolver. Checksums are verified at dequeue rather than
enqueue, so a corrupt datagram can evict a good one before it is ever checked.

### DHCP, DNS, mDNS, SNTP

Good: the RFC 2131 T1/T2 renewal state machine, an extended parameter request
list that is ours, mDNS source-address checking and the probe/conflict/goodbye
sequence, DNS-SD publication, transaction IDs and source ports giving the full
30 bits RFC 5452 asks of a blind attacker, and an SNTP client that exceeds
RFC 5905 §14 including the originate-timestamp echo check.

Open, and the security ones compose: no source-address validation on DNS
responses, a QDCOUNT-0 bypass that skips four of RFC 5452 §9.1's six MUST
checks, AUTHORITY records accepted as answers and cached under their own owner
name, and two independent routes to a permanent cache entry. **The cache is
ours** — enabling it is what turns upstream's ignored TC bit into an RFC 1123
§6.1.3.2 caching violation.

Also: no RFC 5227 probe on the DHCP path (the code is upstream and waiting
behind one define), and `.local` leaking to the unicast resolver on the IPv6
path, which is the one gap here a user meets by accident.

Declined with reasons: IDNA (AmigaOS has no Unicode path to a hostname —
a user types the `xn--` form and it passes through unchanged, which is the right
answer), DHCPv6, RFC 3396 long options, RFC 4361 client identifiers — that last
one **actively wrong for us**, since the whole point of our option 61 is to land
on the same lease as Roadshow on the same NIC, and a DUID would defeat it.

### TLS, PKIX, crypto

Good: RFC 7905 ChaCha20-Poly1305 correct; hostname verification mandatory and
refusing to verify without a hostname; the trust store failing closed and keying
on a hash of the full issuer DER rather than the CN; RFC 5746 secure
renegotiation; compression offered as `null` only, closing CRIME; peer public
key validation against the curve equation, closing invalid-curve attacks;
TLS 1.0/1.1 and SSLv2 and RC4 all correctly absent.

Landed 2026-08-02: the record-buffer fix that had been shadowed by our own
copies of the two files, unbiased DRBG bytes for the huge-number RBG, and
`basicConstraints` CA:TRUE with `pathLenConstraint` — **which had been sitting
on a branch outside the build**, along with three fuzz-found bounds fixes.

Open and ranked in `BACKLOG.md`: hostname verification checking CN before SAN,
an unbounded certificate chain walk, fatal alerts and bare TCP FINs both
reported to the application as a clean end of stream, no revocation of any kind,
and a permissive PKCS#1 v1.5 parser — measured as having no target in the
shipped CA bundle, since 79 of its 80 RSA roots use e=65537.

Declined with the cost stated: TLS 1.3, because nx_secure's 1.3 defines only
AES-GCM suites and GHASH on a 68k is a bit-serial GF(2^128) multiply at
**344.6 ms/KB against CBC's 21.9** — roughly 2.9 KB/s, which is not "expensive"
but "a download nobody waits for". Adding a ChaCha20 suite to the vendored 1.3
tables is the concrete precondition. Certificate date checking is skipped when
the clock is implausible, because the alternative on a dead battery is a machine
that reaches no HTTPS site at all; it is reported through `TLSInfo()`.

### HTTP, WebDAV, sockets API, tools

The sockets API is in good shape against RFC 3493 and 3542: v4-mapped handling,
the interface-index functions, `IPV6_V6ONLY` enforced on `accept()`, `CMSG_*`
macros with the NDK's broken `CMSG_NXTHDR` replaced, `IPV6_PKTINFO` sticky and
ancillary, `ICMP6_FILTER` genuinely applied on receive. Unknown ancillary types
and the unimplemented RFC 3542 extension-header options are **refused rather
than ignored**, which is the right failure mode.

WebDAV is Class 2 read-write and all three desktop clients mount writable.

Open: the accepted-and-ignored cluster — `SBTC_FDCALLBACK`, several socket
options, `IPV6_UNICAST_HOPS` and `IPV6_TCLASS` on TCP — plus an `IPV6_CHECKSUM`
option-number collision, `fetch` treating a 1xx interim response as final, and
`fetch` resolving a relative `Location` as absolute.

Per tool: `telnet` implements RFC 854/855 largely correctly and is purely
reactive; `tftp` is honest RFC 1350 scoped to octet mode with Sorcerer's
Apprentice explicitly avoided; `ping` is RFC 792 in shape but never verifies the
reply checksum; `traceroute` names no RFC and implements none, using
hop-limited ICMP Echo by deliberate choice; `whois` is RFC 3912 in full; `nc`
claims nothing and owes nothing; `ssh` is vendored dropbear, unpatched, and
conformant to RFC 4250-4254.

## The false-claims register

A gap you know about is a backlog item. **A claim that is wrong is a trap for
whoever reads the code next**, so these rank above plain absences.

- **Three separate comments call `_nxd_ipv6_interface_find()` an RFC 6724
  selection routine.** It is a first-match walk with one on-link test and a
  `break` — no candidate set, no policy table, none of Rules 1, 2, 3, 6, 7 or 8.
  RFC 8504 §6.6 makes implementing RFC 6724 a MUST. Note the existing backlog
  note that "6724 does not apply here" is sound but scoped to **§6 destination
  ordering**; these comments are about **§5 source selection**. Do not conflate
  them.
- **The recorded rationale for omitting MLD rests on a false premise.** It
  argues that link-local groups are "never forwarded by anything". Forwarding is
  not the mechanism — MLD snooping *filters* scope-2 groups on the local
  segment, and RFC 9777 §6 requires reports for every group of scope ≥ 2 except
  ff02::1, which includes the solicited-node addresses neighbour discovery
  depends on. The decision may still be right; the reason is not.
- **The retransmit ladder is argued against the wrong clause.** The comment
  states it satisfies RFC 1122 §4.2.3.5's R2 of "at least 100 seconds" for data,
  notes that the same counter bounds SYN retransmission, and stops. MUST-23
  requires **180 seconds** for a SYN. 127 is not 180.
- **A published Developer header asserts a safety property the tree
  contradicts** — that there are no raw IPv6 sockets, which `raw.c` disproves,
  and on which an option-number aliasing decision rests.
- **`tlslib.h` claimed an impostor is refused** while `basicConstraints` was on
  an unmerged branch. True as of `05d41ee`; it was not before.
- **`README.md` says certificates are "properly" checked.** With no revocation,
  no critical-extension rejection, no EKU and no nameConstraints, "properly"
  carries more than the code does. The root-set half is accurate.
- **The randomness pool credits 8 bits of clock entropy in exactly the case its
  own comment says it should not** — the guard tests that the seconds field is
  non-zero, which on a machine with no RTC is uptime, and is non-zero a second
  after boot.
- **A `.local` guard cites RFC 6762 §6.7.** §6.7 is "Legacy Unicast Responses";
  the requirement is §3. The behaviour is right and the wrong citation has
  already propagated.

## What the surveys corrected

Recorded here because an audit that only adds findings is not calibrated.

- `AI_CANONNAME` **is** honoured, and pointing it at nodename is exactly
  RFC 3493 §6.1's stated fallback.
- Returning one address per family is a **quality** gap; 3493 requires only that
  "one or more results shall be returned".
- `getnameinfo` returning the numeric form is **explicitly permitted** by §6.4.
- `gethostbyname` being IPv4-only is **what 3493 asks for**.
- Oversize **UDP** sends now return `EMSGSIZE` before allocating; only raw keeps
  the silent-drop-after-success.
- A renumbering network does **not** leave a stale address forever — prefix
  expiry invalidates every SLAAC address under it. What is missing is the
  graceful half: no preferred lifetime, no DEPRECATED state.
- RFC 5227 DHCPDECLINE was recorded as present. **It is not** — the code is
  upstream behind a define set nowhere.
- Missing 424 in DELETE and COPY multistatus is **correct**; RFC 4918 §9.6.1 and
  §9.8.3 say it SHOULD NOT appear there. It belongs only in PROPPATCH.
- Nothing in RFC 4918 requires 422 at all.
- An absent `Depth` header on LOCK **means infinity**, so storing infinity is
  right.
- CBC padding is **not** checked before the MAC — the MAC is computed
  unconditionally. The timing signal is real but has the opposite shape to
  classic Lucky13.
- RFC 6582 NewReno **is** implemented.

## Citations this document got wrong

Corrected on 2026-08-02 by the work that implemented them, which is the only
review that reliably catches a misattributed section number.

- `getnameinfo` is RFC 3493 **§6.2**, not §6.4 — §6.4 is the address-testing
  macros. Both quoted sentences were verbatim correct; only the number was not.
- URI reference resolution is RFC 3986 **§5.2.2** (Transform References).
  §5.3 is Component Recomposition. §5.2.4 was right.
- "the fragment is not sent" is RFC 9110 **§7.1** — "The target URI excludes the
  reference's fragment component". RFC 3986 §3.5 is descriptive, not a MUST NOT.
- The Host-with-port obligation is RFC 9112 **§3.2**. RFC 9110 §7.2 gives the
  ABNF but its MUST covers only generating the field.

And one framing correction: `fetch` sends `HTTP/1.0`, and RFC 9110 §15.2 says a
server **MUST NOT** send a 1xx to an HTTP/1.0 client — so a conforming CDN will
not send `103` to it. The client-side obligation to parse and discard interim
responses is unconditional, so the fix stands, but the exposure was smaller than
"live on major CDNs" implied.

## Traps for whoever works on this next

- **Adding a DNS bailiwick check without CNAME chain following will break every
  CDN-hosted name.** CNAME processing is compiled out, and the A record that
  follows is accepted *precisely because* no owner-name check exists. One piece
  of work, not two.
- **RFC 4086 contains no RFC 2119 keywords.** "Are we conformant" is not a
  well-formed question about it. The normative obligation is RFC 5246 §D.1, and
  it is the seeding half that fails, not the construction.
- **RFC 8659 forbids using CAA in validation** — §1.1, "Relying Parties MUST NOT
  use CAA records as part of certificate validation". Having no CAA code is
  correct, not a gap.
- **RFC 4193 ULAs need no special handling at the internet layer.** The ULA
  problem is a 6724 policy-table problem.
- **`ndk-include` is Latin-1.** A plain `grep -r` reads those files as binary and
  silently finds nothing. Use `LC_ALL=C grep -a`. A whole RFC 3542 assessment
  was once written on one of those empty results.
