# Extending the NDK

The NDK's `bsdsocket` interface stopped moving in 2006. Anything we add past it
reaches third-party code only through the `developer/` drawer:
`developer/sfd/aminetxduo_lib.sfd` is the source, the generated
`clib`/`inline`/`proto`/`pragmas`/`lvo` headers are committed under
`developer/include/`, `tools/gen-developer.sh --check` fails on drift, and
`tools/stage-developer.sh` assembles the drawer for `dist/make-dist.sh` and for
the example builds that compile against the staged drawer alone.

## LVO slots

`src/bsdsocket/bsdsocket_vectors.c` is the authority. Entry *n* of the NDK's SFD
is at `-(30 + 6n)`; the four standard vectors occupy `-6..-24`.

| Slots | What | Note |
|---|---|---|
| `[0]`..`[136]`, to `-0x336` | the NDK's SFD, ending at `getnameinfo` | |
| `[137]`..`[142]`, `-0x33c`..`-0x35a` | Commodore's `==reserve 6` block | Not ours to take. It is what a regenerated SFD fills first, so a binary compiled against our meaning of `-0x33c` would jump into a different function with nothing to diagnose it |
| `[143]` `-0x360` | reserved, was `ObtainNetXDuoContext` | `bsd_enosys` (`bsdsocket_vectors.c:173`). `tls.library` runs on any `bsdsocket.library` and no longer reaches into this one |
| `[144]` `-0x366`, `[145]` `-0x36c` | `NetStackQuery`, `NetStackControl` | Declared in the drawer's SFD from revision 2; the vector table still comments them PRIVATE. Published means frozen: `NetStatusHeader` and every `NETCTRL_*` request struct become ABI |
| `[146]`..`[149]`, `-0x372`..`-0x384` | RFC 3493 §4, `if_nametoindex`, `if_indextoname`, `if_nameindex`, `if_freenameindex`, revision 3 and up | `==bias 870` in the SFD is what places them. Fixed forever |
| `[150]` `-0x38a` | next free | Extensions continue past the end of the SFD, never inside it |
| runtime detection | release identity, revision and vector-table size | Query the established `SBTC_RELEASESTRPTR` tag first; see `aminetxduo/capabilities.h` and `developer/examples/IfNames.c` |

## What the NDK has, and what is therefore ours to define

Latin-1 headers: `LC_ALL=C grep -a`, or a negative result means nothing.

| In the NDK | Absent, and published by us |
|---|---|
| `struct cmsghdr` (12 bytes, `socklen_t`+`LONG`+`LONG`), `CMSG_DATA`, `CMSG_FIRSTHDR`, `CMSG_NXTHDR` in `<sys/socket.h>`. `CMSG_NXTHDR` as shipped does not compile: it expands to an `ALIGN()` no NDK header defines, and `CMSG_FIRSTHDR` does not test `msg_controllen` | `CMSG_LEN`, `CMSG_SPACE`, `CMSG_ALIGN`, `CMSG_BUFFER`, and replacements for the two broken macros, reached through `<sys/socket.h>`; only buffer convenience macros remain in `include/aminetxduo/cmsg.h` |
| `IP_MULTICAST_IF`, `IP_MULTICAST_TTL`, `IP_MULTICAST_LOOP`, `IP_ADD_MEMBERSHIP`, `IP_DROP_MEMBERSHIP`, `struct ip_mreq` in `<netinet/in.h>`, so IPv4 multicast needs no header work | the IPv6 equivalents, reached through `<netinet/in.h>` |
| `BIOC*`, `struct bpf_hdr`/`bpf_program`/`bpf_stat`/`bpf_version`, `DLT_*`, the `BPF_*` opcodes and `BPF_WORDALIGN` in `<net/bpf.h>`; `FIONREAD` in `<sys/filio.h>`; `SIOCGIFADDR` in `<sys/sockio.h>`, so `bpf.h` has nothing to publish | nothing; everything `AMI_BPF_*` is implementation internals |
| `IP_RETOPTS` is 8 and remains unsupported | `IP_PKTINFO` uses 26, with a distinct 12-byte payload |
| | `PF_INET6`, `IPPROTO_IPV6`, every `IPV6_*`, `INET6_ADDRSTRLEN`, `IN6ADDR_*_INIT`, `IN6_IS_ADDR_*`, `AI_ADDRCONFIG`, `in6_pktinfo`, `icmp6_filter` |

## Behaviour a caller depends on

| Decision | Where | Effect |
|---|---|---|
| One IPv6 option namespace, aligned with AROS except V6ONLY | `developer/netinclude/netinet6/in6.h` | TCLASS 36, RECVPKTINFO 61, PKTINFO 46. Old Linux numeric aliases are rejected; CHECKSUM 26 cannot alias V6ONLY 27 |
| `CMSG_ALIGN` is 4 bytes, and that is ABI | `developer/netinclude/sys/cmsg.h` | RFC 3542 leaves it to the implementation and every ancillary-data buffer a caller ever builds depends on the choice. It is consistent with the NDK's own 12-byte `struct cmsghdr` |
| Loopback is `lo0`, index = its NetX slot plus one | `src/bsdsocket/interfaces.c:224` | It appears in `if_nametoindex()`, `if_indextoname()`, `if_nameindex()` and in the `ipi6_ifindex` of a datagram that arrived over `::1` or `127.0.0.1`, and deliberately not in `ObtainInterfaceList()`, `QueryInterfaceTagList()` or `SIOCGIFCONF`, which are about interfaces a caller can configure and bring online |
| `sendmsg()` on a stream socket takes no `IPV6_PKTINFO` and no `IPV6_HOPLIMIT`, permanently | | A stream's source is fixed when the SYN goes out, so there is nothing per-write to name. Naming the source at `connect()` is `nxd_tcp_client_socket_source_connect()` in the fork, reached by binding before connecting |
| `AI_V4MAPPED` is undefined; `AI_ADDRCONFIG` is 0x0400 | `developer/netinclude/netdb.h`, `src/bsdsocket/addrinfo.c` | Filtering is opt-in, symmetric for both families, and counts assigned non-loopback addresses including ULA and link-local IPv6 |
| IPv6 uses the BSD length/family prefix from ABI 4.11 | `developer/netinclude/netinet/in.h`, `developer/netinclude/sys/socket.h` | IPv4 is unchanged. IPv6 stays 28 bytes, with length at 0 and family at 1. `sockaddr_storage.ss_family` shares that offset. Rebuild pre-release IPv6 applications; there is no legacy layout mode |
| The standard socket headers have one source-controlled overlay | `developer/netinclude/` | The stack and shipped applications compile against the same files staged into `Developer/include`; put that directory before NDK/netinclude. The baseline and changes are in its `ReadMe`. `tests/toolchain/check_ipv6_sdk.py` compiles them for every runtime, CPU and include order; `src/bsdsocket/in6.c` pins the layouts; `tests/ipv6/ipv6_socket_test.c` checks them at run time. The `ReadMe` also holds the AROS alignment, its exceptions and the open upstream decisions |
