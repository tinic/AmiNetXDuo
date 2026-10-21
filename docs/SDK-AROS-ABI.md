# Pre-1.0 SDK alignment with AROS

The common API uses AROS's IPv6 family number, address layout and nonconflicting
IPv6 option values. This is source-level alignment for the classic m68k AmigaOS
ABI, not a claim that an AROS binary or its library vector table can be used on
AmigaOS. All shipped pre-release IPv6 applications must be rebuilt together
with the library. There are no supported third-party IPv6 binaries to retain.

Reference: AROS commit `43e2e698c4075f8b9c19b05702ceca17b1ff5442`, specifically
[netinet/in.h](https://github.com/aros-development-team/AROS/blob/43e2e698c4075f8b9c19b05702ceca17b1ff5442/workbench/network/common/include/netinet/in.h),
[sys/socket.h](https://github.com/aros-development-team/AROS/blob/43e2e698c4075f8b9c19b05702ceca17b1ff5442/workbench/network/common/include/sys/socket.h),
[netdb.h](https://github.com/aros-development-team/AROS/blob/43e2e698c4075f8b9c19b05702ceca17b1ff5442/workbench/network/common/include/netdb.h)
and `net/if.h`. These references establish constants and interface shapes; no
AROS implementation or LGPL header is copied into this overlay.

| API | AmiNetXDuo decision |
| --- | --- |
| `AF_INET6`, `PF_INET6` | 28, matching AROS; 23 remains `AF_IPX` |
| `sockaddr_in6` | 28 bytes: length 0, family 1, port 2, flow 4, address 8, scope 24 |
| `sockaddr_storage` | 128 bytes, length/family prefix, natural m68k alignment; assertions cover IPv4, IPv6 and link-layer addresses |
| unicast hops, multicast interface/hops/loop, join/leave | 4, 9, 10, 11, 12, 13, matching AROS |
| `IPV6_TCLASS`, `IPV6_RECVPKTINFO` | 36, 61, matching AROS |
| `IPV6_PKTINFO`, `IPV6_HOPLIMIT`, `IPV6_RECVHOPLIMIT` | 46, 47, 37, matching AROS |
| `AI_ADDRCONFIG` | 0x0400, matching AROS; filters only when requested |
| `if_nameindex` | 32-bit unsigned index and pointer, matching AROS's m68k shape |

## Deliberate exceptions

**`IPV6_V6ONLY` is 27.** The referenced AROS header assigns 26 to both V6ONLY
and CHECKSUM. We reserve 26 for `IPV6_CHECKSUM` and use the BSD value 27 for
V6ONLY. CHECKSUM is declared but currently unsupported and returns
`ENOPROTOOPT`; it must never change a socket's V6ONLY setting. Old Linux
numeric aliases are no longer accepted, on any socket type. This exception
needs explicit upstream agreement before claiming a shared numeric ABI.

**Established resolver ABI stays unchanged.** AROS uses `AI_NUMERICSERV=8`
and positive `EAI_*` values. The Roadshow/AmiTCP interface uses 16 and negative
errors; 8 already means `AI_EXT`. Changing these would break existing IPv4
programs. The new ADDRCONFIG bit does not overlap either namespace.

**`IP_PKTINFO` is 26, not 8.** AROS does not publish this extension. We use
[Darwin's allocation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/netinet/in.h)
with the existing 12-byte payload (interface index, selected local address,
packet destination). The legacy BSD option `IP_RETOPTS=8` retains its identity
and remains unsupported. This pre-release extension changes even for IPv4
users of packet info; rebuild those applications. Ordinary IPv4 socket
layouts, established options and vectors retain their values.

## Headers and symbol ownership

Applications include `<sys/socket.h>`, `<netinet/in.h>`, `<netinet/icmp6.h>`,
`<net/if.h>` and `<netdb.h>`. The old `<aminetxduo/in6.h>` and `cmsg.h` paths
remain compatibility includes. CMSG buffer convenience macros and AmiNetXDuo
extension discovery remain in the project namespace. `netinet6/in6.h` and
`sys/cmsg.h` are implementation headers reached by the standard headers.

`in6addr_any` and `in6addr_loopback` have external C linkage. A runtime that
already supplies them owns the definitions. Otherwise compile the staged
`Developer/support/in6_addr.c` once into the application, or place that object
in a support archive. Do not add another definition when the runtime already
owns the symbols. The shared Amiga library does not pretend to export data
through its function vectors.

The overlay derives from AmigaPorts/amiga-netinclude commit
`c9fed00ba3186d9b02bc0de13eb0cf3c83f087e1`. Original headers retain their notices
and Latin-1 encoding. New supplemental headers and support code are MIT.
No user-installed compiler headers are modified. Upstreaming should split
common header/data-support changes from the stack implementation and private
extension glue; upstream maintainers do not need to adopt the latter.

## Runtime semantics and extension discovery

`AI_ADDRCONFIG` filters DNS queries, numeric addresses and null-node results
symmetrically. Only assigned non-loopback unicast addresses count; IPv6 ULA
and link-local addresses qualify, tentative/unknown addresses do not. Without
the flag, AF_UNSPEC hostname lookups preserve the existing policy: query AAAA
only when the machine has a 2000::/3 address. This avoids extra DNS queries
and IPv6-first results on IPv4-only LANs with automatic link-local IPv6.
Explicit AF_INET6 requests can still resolve names on ULA-only networks;
explicit AI_ADDRCONFIG uses configured families instead of that legacy gate.
Numeric and null-node requests retain their existing unflagged behavior.
Address configuration is a snapshot, not a promise that a destination is
reachable. `AI_V4MAPPED` remains undefined/unsupported.

No revision number identifies a stack. Query `SBTC_RELEASESTRPTR` through the
existing `SocketBaseTagList` vector, require success and an AmiNetXDuo release
identity, then check revision and vector-table size with the helpers in
`aminetxduo/capabilities.h`. Unknown stacks and failed queries must disable
private extensions. This allocates no new shared tag or vector. The IfNames
example demonstrates the complete sequence. IPv6 support itself is probed
with `socket(AF_INET6, ...)`, since an IPv4-only build shares the revision.

## Verification and remaining upstream decisions

The staged SDK gate checks C89/C11/C++98/C++17, libnix/newlib/clib2, 68000/68020,
both include orders, address layout/alignment, option collisions and C/C++
linkage of the optional constant definitions. Guest tests check actual library
calls, including old-family and option rejection, ancillary delivery, safe
extension discovery and ADDRCONFIG before/after interface configuration. The committed Amiberry harness can
select these modes with `tests/ipv6/run-socket.sh -b BUILD -a ipv4`, `-a ula`,
`-a linklocal`, or `-a ipv6` (the IPv6-only resolver test).
The `addrinfo_dns_policy` host test runs the real resolver against controlled
A/AAAA answers and checks query counts and result order, including unflagged
IPv4 LAN lookups with automatic link-local IPv6 and explicit family/flag use.
Language-mode checks on GCC 16 are not evidence of testing an old GCC, VBCC
or SAS/C executable; those compilers still need downstream validation.

Before upstream acceptance, agree on the V6ONLY exception and IP_PKTINFO
allocation, and on whether the optional data definitions belong in the CRT
or SDK support package. None of these values is claimed to have been approved
by AROS or AmigaPorts. The implementation and its exceptions are explicit so
maintainers can review them without reverse-engineering a mixed namespace.
