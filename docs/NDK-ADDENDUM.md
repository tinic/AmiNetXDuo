# Extending the NDK

The NDK's `bsdsocket` interface stopped moving in 2006. What we add past it now
has to reach third-party code, and today it does not: the archive ships **no
headers at all**, and `include/aminetxduo/` is internal. A caller can link
against the 0..143 range because the NDK declares it, and cannot reach anything
we have added since.

This is the plan for closing that. It covers what to ship, and, more
importantly, the three places where shipping it means *fixing* an ABI that
nobody else will fix for us.

## What exists to build on

The NDK generates its glue from one file:

```
NDK3.2/SANA+RoadshowTCP-IP/sfd/bsdsocket_lib.sfd
    ==base _SocketBase
    ==bias 30
    LONG socket(LONG domain,LONG type,LONG protocol) (d0,d1,d2)
    ...
```

`sfdc` turns that into `proto/bsdsocket.h`, `inline/bsdsocket.h` and
`pragmas/bsdsocket_pragmas.h`. The inline form is a statement-expression macro
that loads the argument registers and jumps through `SocketBase` at the LVO.

LVO arithmetic: entry *n* of the SFD is at `-(30 + 6n)`, and our vector array
carries the four standard library vectors (Open/Close/Expunge/Reserved) at
`-6..-24` before `socket`. Do not derive the mapping from the SFD by counting
`==reserve` directives, there are three of them (10, 2, 6) and they do not all
consume callable slots. `src/bsdsocket/bsdsocket_vectors.c` is the authority.

The layout that matters:

```
  -0x336 [136]  getnameinfo     last entry the NDK's SFD defines
  -0x33c [137] ..
  -0x35a [142]  the ==reserve 6 block, Commodore's "six reserved
                slots for future expansion", the last directive before ==end
  -0x360 [143]  ObtainNetXDuoContext   ours, past the end of the SFD
  -0x366 [144]  NetStackQuery          ours
  -0x36c [145]  NetStackControl        ours
  -0x372 [146]  first free
```

## Five decisions we are making for everyone

These are the reason this needs planning rather than just doing.

**1. LVO slots.** `[146]` onward, continuing past the end of the SFD as
`[143]`-`[145]` already do.

Do **not** take the `==reserve 6` block at `[137]`-`[142]`, tempting as six free
slots inside the published range look. That range is what Commodore set aside
for its own expansion; if Roadshow ever ships again and fills it, every binary
compiled against our meaning of `-0x33c` jumps into a different function, and
nothing diagnoses it. Past the end can only collide with another third party
doing the same thing, and defining that space is what being the reference means.

Once we hand out `-0x372` for `if_nametoindex`, it is that forever.

**2. `CMSG_ALIGN` for m68k.** SETTLED 2026-07-31: **4 bytes**, in
`include/aminetxduo/cmsg.h`. RFC 3542 leaves the alignment to the
implementation, so we pick it, and every ancillary-data buffer any caller ever
builds depends on the choice.

`struct cmsghdr` itself *is* in the NDK, and is 12 bytes, the assessment that
said otherwise was wrong. What is missing is `CMSG_LEN` and `CMSG_SPACE`; what
is present but broken is `CMSG_NXTHDR`, which expands to an `ALIGN()` no NDK
header defines, and `CMSG_FIRSTHDR`, which does not test `msg_controllen`. The
addendum header replaces those two and adds the other two, so a caller that
includes it after `<sys/socket.h>` gets one consistent set.

**3. `IPV6_*` option numbers.** Also not in the NDK. They must not collide with
anything Roadshow might assign. Same permanence.

SETTLED 2026-07-31, and the rule is not "pick one": both the BSD and the Linux
numbers are accepted, and **the numbering a caller enables an option with is the
numbering it gets back as `cmsg_type`**. `IPV6_RECVPKTINFO` 36 yields
`cmsg_type` `IPV6_PKTINFO` 46; 49 yields 50. Mixing them on one socket reads as
whichever was set last.

`IP_PKTINFO` takes 8, which this NDK spells `IP_RETOPTS`, a 4.3BSD get/set of
arriving IP options that no AmigaOS stack ever answered and this one refuses.
That is the one place the addendum takes a number the NDK had already used.

**4. Loopback's index is its NetX slot plus one, and it is called `lo0`.** It
appears in `if_nametoindex()`, `if_indextoname()` and `if_nameindex()`, and in
the `ipi6_ifindex` of a datagram that arrived over `::1` or `127.0.0.1`, which
is what a server answering on the interface a query came in on needs.

It is deliberately *not* in `ObtainInterfaceList()`, `QueryInterfaceTagList()`
or `SIOCGIFCONF`. Those three are about SANA-II interfaces a caller can
configure, bring online and take down, and loopback is none of those things; a
tool that walks them to offer the user a choice would otherwise offer one that
cannot be chosen.

**5. A per-write source address or hop limit on TCP is refused, permanently.**
`sendmsg()` on a stream socket takes no `IPV6_PKTINFO` and no `IPV6_HOPLIMIT`.
A stream's source is fixed when the SYN goes out, so there is nothing per-write
to name and accepting one would mean either ignoring it or lying about it.

Naming the source at `connect()` was the real gap, and it is closed separately:
`nxd_tcp_client_socket_source_connect()` in the fork, reached by binding the
socket before connecting.

## Shape of the addendum

A `Developer/` drawer in the archive:

```
Developer/
    include/aminetxduo/     the new types and prototypes
    sfd/aminetxduo_lib.sfd  our additions only
    inline/  proto/  pragmas/    generated from that sfd
```

**A separate SFD, not a fork of Commodore's.** A fork has to be re-merged every
time the NDK moves and silently diverges when it is not. A small separate file
declaring the same `==base _SocketBase` generates its own inline header, and a
caller including both reaches all of it, the base variable is shared, so the
two sets mix with no glue.

**Additive header paths, not shadowed ones.** RFC 3493 says `if_nametoindex()`
comes from `<net/if.h>`, and a developer porting Unix code will expect that. But
shipping a `net/if.h` means include order decides whether ours or the NDK's
wins, silently, per translation unit. So: the headers live under
`aminetxduo/` and never replace an NDK header. If the standard spellings turn
out to matter, an optional overlay drawer can be documented as
put-this-first-and-here-is-the-risk, opt-in, not the default.

**Runtime detection is `lib_Revision`.** A header cannot tell a caller whether
the library actually in memory has the vectors; only the revision can, which is
the mechanism `tool_netstatus_open()` already uses and which we bumped to 2 on
2026-07-31. Every addendum symbol carries the revision that introduced it, and
the drawer ships the check as a documented three-liner rather than leaving each
caller to invent one.

## Contents, in dependency order

1. **RFC 3493 §4**, `struct if_nameindex`, `IF_NAMESIZE`, and the four
   prototypes. New LVOs `[146]`..`[149]`. Indices are 1-based, matching
   `rtm_index`; change neither alone.
2. **RFC 3542**, WRITTEN, `include/aminetxduo/cmsg.h`: the `CMSG_*` macros,
   `CMSG_BUFFER()` for the aligned control buffer a `char[]` cannot be,
   `struct in6_pktinfo`, `struct in_pktinfo`, `struct icmp6_filter` + its six
   macros, and the option numbers. No new LVOs: it rides `sendmsg`/`recvmsg`,
   and `struct msghdr` is already the 28-byte 4.4BSD shape with `msg_control` at
   offset 16. The header is on the include path of
   `tests/ipv6/ipv6_socket_test.c`, which links against none of our code, so it
   is already proved usable from outside, what is left is shipping it.
3. **What is already ours and private**, `netstatus.h`'s `NetStackQuery` /
   `NetStackControl` at `-0x366`/`-0x36c`. Publishing these is what lets someone
   else write a `netstat`. Decide deliberately: published means frozen.

Order matters because (1) sets the slot precedent and (2) sets the alignment
precedent, and (3) is the one that is purely a decision to publish what already
works.

## What shipped, 2026-07-31

Item 1 of the three. The drawer exists and carries RFC 3493 §4 only.

```
developer/sfd/aminetxduo_lib.sfd   the source of truth
developer/include/{clib,inline,proto,pragmas,lvo}/   generated, committed
developer/examples/IfNames.c       the example, and the test
developer/ReadMe                   the drawer's own documentation
tools/gen-developer.sh             regenerate; --check for drift
tools/stage-developer.sh           assemble the drawer into a destdir
```

`==bias 882` in the SFD is what puts the first entry at `-0x372`; sfdc emits
`LP1(0x372, ...)` and `_LVOif_nametoindex EQU -882`, which is the number
`tests/tools/ifprobe.c` reaches by hand.

The generated headers are committed so that packaging never needs sfdc.
`tools/gen-developer.sh --check` regenerates into a temp directory and diffs,
and `ci.sh`'s cross stage runs it wherever the resolved toolchain has an
`sfdc`, which the pinned one does.

`tools/stage-developer.sh` has one consumer too many to be inlined into
either: `dist/make-dist.sh` stages the drawer into the archive, and
`tests/tools/CMakeLists.txt` stages it into the build tree and compiles
`IfNames.c` against **that alone**, the staged include directory and the
NDK, and nothing else. A type that only exists in `include/aminetxduo/` and
never reaches the drawer is a compile error rather than a download.

Its `PUBLIC_HEADERS` list is deliberately a list and not a wildcard:
`include/aminetxduo/` holds the internal headers too, and being published is
a per-file decision that freezes the file.

One departure from the sketch above: `inline/`, `proto/` and `pragmas/` live
*under* `include/` rather than beside it, so the drawer needs one `-I` and
not two. The NDK's own `include_h/` is arranged the same way.

`tests/tools/ifprobe.c` keeps its hand-written vectors. It is written to
share nothing with the implementation, and now shares nothing with the
drawer either, so the two arriving at the same four offsets independently is
worth more than the deduplication.

Still open: item 2 (RFC 3542) is on another branch and the SFD carries a
marker where it does not go. Item 3 (`NetStackQuery`/`NetStackControl`) is
recorded as a recommendation in `BACKLOG.md`, not taken, publishing them
freezes `NetStatusHeader` and every `NETCTRL_*` request struct.

## The drawer is not only the vectors

Added the same day, after the item was widened: a constant is as much of a
wall as a vector. `include/aminetxduo/in6.h` is the second published header
and carries what the NDK leaves out of AF_INET6, `IPPROTO_IPV6`,
`PF_INET6`, `INET6_ADDRSTRLEN`, `IPV6_V6ONLY` / `IPV6_UNICAST_HOPS` /
`IPV6_TCLASS`, `IN6ADDR_*_INIT`, the `IN6_IS_ADDR_*` macros,
`struct sockaddr_storage` and `AI_ADDRCONFIG`. No vectors: the calls are
`setsockopt()`, `socket()` and `getaddrinfo()`, which the NDK declares
already, so there is no revision to check, what a caller wants to know is
whether `socket(AF_INET6, ...)` succeeds.

It is the SINGLE definition. `bsdsocket_internal.h` now includes it and its
`AMI_IPV6_*_BSD` names are aliases of the published ones rather than a second
copy of the numbers; the `_LINUX` alternates stay internal, because accepting
both numberings is behaviour and only one number should be published.

Two decisions worth writing down:

- **`AI_V4MAPPED` is deliberately not defined.** `getaddrinfo()` refuses any
  bit outside the NDK's `AI_MASK` with `EAI_BADFLAGS`, and this library never
  synthesises `::ffff:a.b.c.d`, so 0 would claim behaviour we do not have and
  a real bit would be refused. Undefined makes it a compile error to fix.
  `AI_ADDRCONFIG` is 0 for the mirror-image reason: the behaviour is
  unconditional, so passing the flag is a truthful no-op.
- **`sockaddr_storage` has no `ss_family`.** The family byte is at offset 1
  for `AF_INET` and offset 0 for `AF_INET6` on this NDK, so a member at
  either offset is right for one family and silently wrong for the other.
  It is 128 aligned bytes; the returned length says what arrived.

`developer/examples/V6Only.c` exercises all of it, compiled against the
staged drawer alone like `IfNames.c`.

## What the NDK actually has, audited 2026-07-31

Re-verified against `ndk-include` rather than taken from the comments, and
two of the comments were wrong. (`grep -r` reads those headers as binary,
they are Latin-1 and carry a `©`, so it silently finds nothing. Use
`LC_ALL=C grep -a`, or a negative result means nothing.)

Absent, and therefore ours to define: `sockaddr_storage`, `PF_INET6`,
`IPPROTO_IPV6`, every `IPV6_*`, `INET6_ADDRSTRLEN`, `in6addr_any`,
`IN6ADDR_*_INIT`, `IN6_IS_ADDR_*`, `AI_V4MAPPED`, `AI_ADDRCONFIG`,
`in6_pktinfo`, `icmp6_filter`, `CMSG_ALIGN`, `CMSG_BUFFER`.

**Present, contrary to what `BACKLOG.md` says about RFC 3542:**
`struct cmsghdr`, `CMSG_DATA`, `CMSG_FIRSTHDR` and `CMSG_NXTHDR` are all in
`<sys/socket.h>`, and `struct cmsghdr` is already the 12-byte
`socklen_t`+`LONG`+`LONG` shape that `CMSG_ALIGN` = 4 implies, so that
decision is consistent with what is there, which is worth knowing before
writing a second `struct cmsghdr`. What is genuinely missing is `CMSG_LEN`,
`CMSG_SPACE`, `CMSG_ALIGN`, and the bare `ALIGN()` the NDK's own
`CMSG_NXTHDR` expands to and which nothing in the NDK defines, so
`CMSG_NXTHDR` as shipped does not compile.

**Present, so IPv4 multicast needs no header work:** `IP_MULTICAST_IF`,
`IP_MULTICAST_TTL`, `IP_MULTICAST_LOOP`, `IP_ADD_MEMBERSHIP`,
`IP_DROP_MEMBERSHIP` and `struct ip_mreq` are in `<netinet/in.h>`. The IPv6
equivalents are absent, so `in6.h` publishes `IPV6_JOIN_GROUP`,
`IPV6_LEAVE_GROUP`, `IPV6_MULTICAST_IF`, `IPV6_MULTICAST_HOPS`,
`IPV6_MULTICAST_LOOP` and `struct ipv6_mreq`, BSD numbers published, Linux
numbers accepted, as the other three options are. A join sends no MLD report,
because there is none in the stack to send; link-local scope is what to rely
on.

**Present, so `bpf.h` has nothing to publish:** `BIOC*`, `struct bpf_hdr`,
`struct bpf_program`, `struct bpf_stat`, `struct bpf_version`, `DLT_*`, the
`BPF_*` opcodes and `BPF_WORDALIGN` are in `<net/bpf.h>`; `FIONREAD` is in
`<sys/filio.h>` and `SIOCGIFADDR` in `<sys/sockio.h>`. Everything `AMI_BPF_*`
in `include/aminetxduo/bpf.h` is implementation internals, segment views,
channel limits, internal error codes, the offsets the host-test replica
pins, and none of it is an application's business.

## What this does not cover

RFC 4007's `%zone` text form is behaviour, not header, it needs a parser and a
formatter, and no ABI. It is in `BACKLOG.md` on its own.
