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
| `[143]` `-0x360` | `ObtainNetXDuoContext`, for `tls.library` | Compiled in only under `AMINETXDUO_TLS_CONTEXT`, otherwise `bsd_enosys` |
| `[144]` `-0x366`, `[145]` `-0x36c` | `NetStackQuery`, `NetStackControl` | Declared in the drawer's SFD from revision 2; the vector table still comments them PRIVATE. Published means frozen: `NetStatusHeader` and every `NETCTRL_*` request struct become ABI |
| `[146]`..`[149]`, `-0x372`..`-0x384` | RFC 3493 §4, `if_nametoindex`, `if_indextoname`, `if_nameindex`, `if_freenameindex`, revision 3 and up | `==bias 870` in the SFD is what places them. Fixed forever |
| `[150]` `-0x38a` | next free | Extensions continue past the end of the SFD, never inside it |
| runtime detection | `lib_Revision` | A header cannot tell a caller whether the library in memory has the vectors. Every addendum symbol carries the revision that introduced it |

## What the NDK has, and what is therefore ours to define

Latin-1 headers: `LC_ALL=C grep -a`, or a negative result means nothing.

| In the NDK | Absent, and published by us |
|---|---|
| `struct cmsghdr` (12 bytes, `socklen_t`+`LONG`+`LONG`), `CMSG_DATA`, `CMSG_FIRSTHDR`, `CMSG_NXTHDR` in `<sys/socket.h>`. `CMSG_NXTHDR` as shipped does not compile: it expands to an `ALIGN()` no NDK header defines, and `CMSG_FIRSTHDR` does not test `msg_controllen` | `CMSG_LEN`, `CMSG_SPACE`, `CMSG_ALIGN`, `CMSG_BUFFER`, and replacements for the two broken macros, in `include/aminetxduo/cmsg.h` |
| `IP_MULTICAST_IF`, `IP_MULTICAST_TTL`, `IP_MULTICAST_LOOP`, `IP_ADD_MEMBERSHIP`, `IP_DROP_MEMBERSHIP`, `struct ip_mreq` in `<netinet/in.h>`, so IPv4 multicast needs no header work | the IPv6 equivalents, in `include/aminetxduo/in6.h` |
| `BIOC*`, `struct bpf_hdr`/`bpf_program`/`bpf_stat`/`bpf_version`, `DLT_*`, the `BPF_*` opcodes and `BPF_WORDALIGN` in `<net/bpf.h>`; `FIONREAD` in `<sys/filio.h>`; `SIOCGIFADDR` in `<sys/sockio.h>`, so `bpf.h` has nothing to publish | nothing; everything `AMI_BPF_*` is implementation internals |
| `IP_RETOPTS` at 8, a 4.3BSD get/set of arriving IP options that no AmigaOS stack ever answered and this one refuses | `IP_PKTINFO` takes that 8. The one number the addendum reuses |
| | `sockaddr_storage`, `PF_INET6`, `IPPROTO_IPV6`, every `IPV6_*`, `INET6_ADDRSTRLEN`, `in6addr_any`, `IN6ADDR_*_INIT`, `IN6_IS_ADDR_*`, `AI_ADDRCONFIG`, `in6_pktinfo`, `icmp6_filter` |

## Behaviour a caller depends on

| Decision | Where | Effect |
|---|---|---|
| Both the BSD and the Linux `IPV6_*` numbers are accepted, and the numbering used to enable an option is the numbering returned as `cmsg_type` | `include/aminetxduo/in6.h`, `src/bsdsocket/in6.c:240` | `IPV6_RECVPKTINFO` 36 yields `IPV6_PKTINFO` 46; 49 yields 50. Mixing them on one socket reads as whichever was set last |
| `CMSG_ALIGN` is 4 bytes, and that is ABI | `include/aminetxduo/cmsg.h:83` | RFC 3542 leaves it to the implementation and every ancillary-data buffer a caller ever builds depends on the choice. It is consistent with the NDK's own 12-byte `struct cmsghdr` |
| Loopback is `lo0`, index = its NetX slot plus one | `src/bsdsocket/interfaces.c:224` | It appears in `if_nametoindex()`, `if_indextoname()`, `if_nameindex()` and in the `ipi6_ifindex` of a datagram that arrived over `::1` or `127.0.0.1`, and deliberately not in `ObtainInterfaceList()`, `QueryInterfaceTagList()` or `SIOCGIFCONF`, which are about interfaces a caller can configure and bring online |
| `sendmsg()` on a stream socket takes no `IPV6_PKTINFO` and no `IPV6_HOPLIMIT`, permanently | | A stream's source is fixed when the SYN goes out, so there is nothing per-write to name. Naming the source at `connect()` is `nxd_tcp_client_socket_source_connect()` in the fork, reached by binding before connecting |
| `AI_V4MAPPED` is not defined and `AI_ADDRCONFIG` is 0 | `include/aminetxduo/in6.h:357` | `getaddrinfo()` refuses any bit outside the NDK's `AI_MASK` with `EAI_BADFLAGS` and this library never synthesises `::ffff:a.b.c.d`, so leaving `AI_V4MAPPED` undefined makes it a compile error rather than a silent lie. `AI_ADDRCONFIG`'s behaviour is unconditional here, so 0 is a truthful no-op |
| `sockaddr_storage` has no `ss_family` | `include/aminetxduo/in6.h:333` | The family byte is at offset 1 for `AF_INET` and offset 0 for `AF_INET6` on this NDK, so a member at either offset is right for one family and silently wrong for the other. It is 128 aligned bytes and the returned length says what arrived |
| Headers are additive, never shadowed | `developer/include/aminetxduo/` | Shipping a `net/if.h` would let include order decide per translation unit whether ours or the NDK's wins |
