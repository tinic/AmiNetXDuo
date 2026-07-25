# AmiNetXDuo — Feasibility Research

*An AmiTCP/Roadshow-compatible `bsdsocket.library` for AmigaOS built on Eclipse ThreadX NetX Duo.*

Status: research only. No implementation decisions are final. Empirical results in
[§5.4](#54-empirical-build-spike-m68k) were produced on 2026-07-24 with the local
`m68k-amigaos-gcc 15.2.0` toolchain.

---

## 1. Executive summary

**Nobody has done this.** There is no NetX Duo or ThreadX port to AmigaOS or to m68k in
any public repository (GitHub search for `netxduo amiga` / `threadx amiga`: **0 results**;
`threadx` upstream has no 68k port among its 46 architectures). The idea is original.

**It is technically feasible, and the risky part is not the one you'd expect.** The
NetX Duo core is portable, endian-clean C, and it compiles for 68020 out of the box:

| Component | Files compiled | Failures | `.text` (m68020, `-O2`) |
|---|---|---|---|
| ThreadX core | 185 | **0** | 27.7 KB |
| NetX Duo core (all, IPv4+IPv6) | 511 | **0** | 111.9 KB |
| NetX Duo IPv4 subset (tcp/udp/ip/arp/icmp/packet) | — | — | ~73 KB |
| DHCP client | 1 | 0 | 15.2 KB |
| DNS client | 1 | 0 | 9.5 KB |
| PPP | 1 | 0 | 22.2 KB |

A shippable IPv4 + DHCP + DNS `bsdsocket.library` therefore lands in the **100–140 KB**
range — the same order as Roadshow's. That is not the problem.

**The problem is ThreadX.** NetX Duo does not merely call the ThreadX API — it reaches
into ThreadX *internals* to implement its own socket suspension (`_tx_thread_system_suspend`,
`_tx_thread_system_resume`, `_tx_thread_preempt_disable`, `tx_thread_suspend_cleanup`,
and direct manipulation of `TX_THREAD` suspension-list fields, across **~40 core files**).
There is no `NX_STANDALONE`/no-RTOS mode — the string `STANDALONE` does not appear
anywhere in `common/`, `addons/`, or `ports/`. So AmiNetXDuo must ship a real ThreadX
kernel running on top of Exec, not a thin `tx_*` shim. See [§6.2](#62-threadx-on-exec-the-central-problem).

**The competitive picture changed two weeks ago.** Two independent modern-stack projects
appeared in July 2026 ([§2](#2-prior-art)): `lwip-amiga` (lwIP + `bsdsocket.library`, but
**no SANA-II** — PiStorm/Emu68 `genet.device` only) and `AmiTCP_NG` (GPL fork of AmiTCP
3.0b2 with a clean-room Roadshow ABI, SANA-II, DHCP, BPF). Neither is a blocker, but they
change what "new" means. AmiNetXDuo's defensible ground is: **MIT-licensed, no BSD/GPL
lineage, IPv4 + IPv6 dual stack, SANA-II compatible, with NetX Duo's protocol add-on
catalogue (DHCP/DNS/PPP/PPPoE/SNTP/mDNS/NAT/TLS) available for free.** IPv6 in particular
would be an Amiga first — no classic stack has ever had it.

---

## 2. Prior art

### 2.1 NetX Duo / ThreadX on Amiga or 68k — none

- GitHub repository search, `netxduo amiga OR threadx amiga`: **0 results**.
- `eclipse-threadx/threadx` `ports/`, `ports_arch/`, `ports_module/`, `ports_smp/`: ARC,
  ARM (11/9/Cortex-A/M/R), C667x, Linux, MIPS, RISC-V, RXv1–3, Win32/64, Xtensa. **No
  m68k, no ColdFire.**
- NetX Duo `ports/`: 24 targets, same story.

The two hosted ports (`linux`, `win32`) are the useful precedent: they run ThreadX threads
on host threads with a global scheduler mutex. That is the template for an Exec port
([§6.2](#62-threadx-on-exec-the-central-problem)).

### 2.2 `rondoval/lwip-amiga` — the closest analogue (created 2026-07-10)

The direct architectural precedent: a modern embedded TCP/IP core (lwIP 2.2.1) plus an
AmigaOS port layer plus a `bsdsocket.library` front end, for AmigaOS 3.2. BSD-3-Clause.

What it proves works:
- **Per-opener child library bases + an in-library stack task**, with the whole thing
  self-starting on first `OpenLibrary`.
- **Core-locking direct path**: application tasks execute stack code *in their own
  context* under a single core semaphore, using Exec signals as the blocking primitive.
  (This works for lwIP because lwIP's core-lock mode has no notion of "the calling
  thread is suspended by the stack". NetX Duo does — see [§6.3](#63-who-runs-the-stack-code).)
- **A generated LVO vector table** — 139 slots emitted from the NDK `bsdsocket` SFD by a
  Python script. Same trick applies here.
- **76 of 121 LVOs implemented** is enough to be genuinely useful; it scores **138/142 on
  the `bsdsocktest` conformance suite** (4 skipped, 0 failed).
- Real numbers on real silicon: 944 Mb/s TCP down / 558 Mb/s up on Pi4/PiStorm.

What it deliberately does *not* do — and this is AmiNetXDuo's opening:
- **No SANA-II.** It defines its own `netdev` ABI (direct-call, zero-copy, batched,
  checksum offload). Only `genet.device` on Pi4/CM4 under PiStorm/Emu68 implements it.
  Every real Amiga NIC — A2065, Ariadne, X-Surf, PCMCIA, the PPP/SLIP drivers — is
  excluded by design.
- No IPv6, no TLS.
- Not implemented: Roadshow interface-config/routing/monitor calls, `mbuf_*`, `bpf_*`, `ipf_*`.

### 2.3 `MW0MWZ/AmiTCP_NG` — the incumbent-killer (created 2026-07-19, GPL-2.0)

A modernised GPL fork of AmiTCP/IP 3.0b2, positioned as a **drop-in Roadshow replacement
with no time limit**. Claims: full `bsdsocket.library` API over SANA-II + loopback, DHCP
client, RFC 3927 link-local, DNS with cache, RFC 1323 window scaling/timestamps, RFC 6528
randomised ISNs, a BPF subsystem, Roadshow-compatible command set (`Online`, `Offline`,
`AddNetInterface`, `ShowNetStatus`, `ping`), self-starting `LIBS:bsdsocket.library`.
Validated on emulated A2065/SLIRP and on PiStorm + `wifipi.device`.

Explicitly clean-room on the Roadshow extensions: Olaf Barthel's open Roadshow SDK used
as an ABI reference only.

Relevance: it removes "there is no free, modern, SANA-II TCP stack" as a motivation. It
does **not** remove: GPL-2 lineage (a problem if you want MIT), 4.4BSD-derived core code,
no IPv6, no TLS.

### 2.4 The 2017 NetBSD-stack proposal — never delivered

Swift Griggs, `port-amiga` list, Dec 2017: proposed adapting NetBSD's `src/netinet` to
AmigaOS 3.x as a `bsdsocket.library` replacement, citing dissatisfaction with all existing
stacks. Obstacles they identified — no kernel threads (strip fine-grained locking), no
kernel-only allocators, SANA-II vs BSD ifnet mismatch, no memory protection to debug with
— are exactly the obstacles here, minus locking (ThreadX brings its own). No code shipped.

### 2.5 AROSTCP

AROS's stack, in `workbench/network/stacks/AROSTCP`. Descends from the **AmiTCP/IP Group
(HUT) 1993–94 sources**, plus Neil Cafferkey (2005) and Pavel Fedin (2005–06); ISC DHCP
suite bundled. **GPL-2** for the stack, **LGPL-2** for `netinclude/` and `netlib/`, over a
4.4BSD base. The most complete open `bsdsocket.library` implementation in existence and
the best *behavioural* reference — but its licence rules it out as a code source for an
MIT project.

### 2.6 Others worth knowing

| Project | What it is | Why it matters |
|---|---|---|
| `tbdye/bsdsocktest` (GPL-3, Feb 2026) | **142-test conformance suite** for `bsdsocket.library`, 12 categories; `docs/COMPATIBILITY.md` documents per-stack bugs; `docs/AMITCP_API.md` documents the Amiga-vs-BSD deltas | **Adopt as the acceptance gate.** lwip-amiga's 138/142 is the number to beat. |
| `cnvogelg/aminisocket` | "Minimal Amiga BSDSocket Library Implementation" | Smallest reference for the library-base plumbing |
| `jbilander/catalyst` (LGPL-2.1) | Offloads TCP/IP **and TLS** to a hardware coprocessor over a mailbox | The "don't run a stack on the 68k at all" school |
| `lainejones/a314bsd`, `AmiSSL-Tunnel` | `bsdsocket.library` over an a314 adapter; TLS offloaded to a LAN daemon | Same school |
| WinUAE / FS-UAE / Amiberry | Native `bsdsocket.library` *emulation* (host sockets) | Not a stack — but it means "works under emulation" ≠ "works". Must be disabled when testing. |
| `obarthel/amiga-smbfs` | SMB filesystem written against the AmiTCP V3 API | A real-world client to validate against |

---

## 3. The compatibility contract

### 3.1 The library model

AmigaOS has no kernel networking. A TCP/IP stack is a **shared library** —
`LIBS:bsdsocket.library` — running in user space in the same address space as every
application, reached by `JSR` through a negative-offset LVO table. There is no syscall,
no fd table, no memory protection.

Consequences that shape the whole design:

- **Per-opener library base.** Every task calls `OpenLibrary("bsdsocket.library", 4)` and
  gets *its own* base with a private descriptor table, errno pointer and tag state.
  `SocketBase` must never be shared between tasks. This is the single most important
  structural requirement — the classic implementation is a child-base clone per opener.
- **`Errno()` / `SetErrnoPtr()`** — errno is per-opener and optionally written through a
  caller-supplied pointer of caller-chosen width (1/2/4 bytes), configured via
  `SBTC_ERRNOPTR` in `SocketBaseTagList`.
- **`CloseSocket()`, not `close()`** — sockets are not DOS file handles.
- **`WaitSelect()` is the core primitive**, not `select()`: it waits on socket readiness
  **and Exec signal bits** simultaneously, so one loop serves network + Intuition + ARexx
  + timers, with break signals surfacing as `EINTR`. Getting its documented edge cases
  right (sets unmodified on error, break repost, user signal mask) is most of the
  compatibility work.
- **`SetSocketSignals()` / `GetSocketEvents()` / `SO_EVENTMASK`** — the async, signal-driven
  alternative (AmiTCP V4 event API).
- **`ObtainSocket()` / `ReleaseSocket()` / `ReleaseCopyOfSocket()` / `ObtainServerSocket()`**
  — descriptor hand-off between tasks (how `inetd`-style servers and launched children get
  a socket). Reference-counted, cross-base.
- **`Dup2Socket()`** for descriptor cloning.

### 3.2 The ABI: 121 LVOs

Taken from the Roadshow NDK `pragmas/bsdsocket_pragmas.h` in the local toolchain
(`amigaos/tools/m68k-amigaos-gcc/.../ndk-include`). Offsets are the `amicall` byte offsets;
LVO = −offset. AmiTCP V3 stops around `0x126`; V4 adds through `~0x16e`; the rest is
Roadshow's extension set. The gap `0x132`–`0x168` is reserved/private.

```
0x01e socket          0x0d2 gethostbyname     0x186 bpf_set_notify_mask   0x228 getnetent            0x2b8 ObtainServerSocket
0x024 bind            0x0d8 gethostbyaddr     0x18c bpf_set_interrupt_mask 0x22e setprotoent         0x2be GetDefaultDomainName
0x02a listen          0x0de getnetbyname      0x192 bpf_ioctl             0x234 endprotoent         0x2c4 SetDefaultDomainName
0x030 accept          0x0e4 getnetbyaddr      0x198 bpf_data_waiting      0x23a getprotoent         0x2ca ObtainRoadshowData
0x036 connect         0x0ea getservbyname     0x19e AddRouteTagList       0x240 setservent          0x2d0 ReleaseRoadshowData
0x03c sendto          0x0f0 getservbyport     0x1a4 DeleteRouteTagList    0x246 endservent          0x2d6 ChangeRoadshowData
0x042 send            0x0f6 getprotobyname    0x1aa ChangeRouteTagList    0x24c getservent          0x2dc RemoveInterface
0x048 recvfrom        0x0fc getprotobynumber  0x1b0 FreeRouteInfo         0x252 inet_aton           0x2e2 gethostbyname_r
0x04e recv            0x102 vsyslog           0x1b6 GetRouteInfo          0x258 inet_ntop           0x2e8 gethostbyaddr_r
0x054 shutdown        0x108 Dup2Socket        0x1bc AddInterfaceTagList   0x25e inet_pton           0x2fa ipf_open
0x05a setsockopt      0x10e sendmsg           0x1c2 ConfigureInterfaceTagList 0x264 In_LocalAddr    0x300 ipf_close
0x060 getsockopt      0x114 recvmsg           0x1c8 ReleaseInterfaceList  0x26a In_CanForward       0x306 ipf_ioctl
0x066 getsockname     0x11a gethostname       0x1ce ObtainInterfaceList   0x270 mbuf_copym          0x30c ipf_log_read
0x06c getpeername     0x120 gethostid         0x1d4 QueryInterfaceTagList 0x276 mbuf_copyback       0x312 ipf_log_data_waiting
0x072 IoctlSocket     0x126 SocketBaseTagList 0x1da CreateAddrAllocMessageA 0x27c mbuf_copydata     0x318 ipf_set_notify_mask
0x078 CloseSocket     0x12c GetSocketEvents   0x1e0 DeleteAddrAllocMessage 0x282 mbuf_free          0x31e ipf_set_interrupt_mask
0x07e WaitSelect      0x16e bpf_open          0x1e6 BeginInterfaceConfig  0x288 mbuf_freem          0x324 freeaddrinfo
0x084 SetSocketSignals 0x174 bpf_close        0x1ec AbortInterfaceConfig  0x28e mbuf_get            0x32a getaddrinfo
0x08a getdtablesize   0x17a bpf_read          0x1f2 AddNetMonitorHookTagList 0x294 mbuf_gethdr      0x330 gai_strerror
0x090 ObtainSocket    0x180 bpf_write         0x1f8 RemoveNetMonitorHook  0x29a mbuf_prepend        0x336 getnameinfo
0x096 ReleaseSocket   0x0ae Inet_NtoA         0x1fe GetNetworkStatistics  0x2a0 mbuf_cat
0x09c ReleaseCopyOfSocket 0x0b4 inet_addr     0x204 AddDomainNameServer   0x2a6 mbuf_adj
0x0a2 Errno           0x0ba Inet_LnaOf        0x20a RemoveDomainNameServer 0x2ac mbuf_pullup
0x0a8 SetErrnoPtr     0x0c0 Inet_NetOf        0x210 ReleaseDomainNameServerList 0x2b2 ProcessIsServer
                      0x0c6 Inet_MakeAddr     0x216 ObtainDomainNameServerList
                      0x0cc inet_network      0x21c setnetent
                                              0x222 endnetent
```

Practical tiering for implementation:

- **Tier 1 (must, ~45 vectors)** — socket core, data transfer, `WaitSelect`, `IoctlSocket`
  (`FIONBIO`/`FIONREAD`), errno family, `SocketBaseTagList`, `getdtablesize`, the `inet_*`
  conversions, `gethostbyname`/`_r`, `gethostbyaddr`/`_r`, `gethostname`.
- **Tier 2 (should, ~25)** — `getaddrinfo`/`getnameinfo`/`freeaddrinfo`/`gai_strerror`,
  `sendmsg`/`recvmsg`, `Dup2Socket`, `ObtainSocket` family, `GetSocketEvents` +
  `SO_EVENTMASK`, the `get{serv,proto,net}*` netdb iterators, `vsyslog`.
- **Tier 3 (Roadshow parity, ~35)** — interface config/query, routing, DNS-server
  management, `GetNetworkStatistics`, `*RoadshowData`. Needed for `ShowNetStatus`,
  `AddNetInterface`, `netinfo`-style tools to work unchanged.
- **Tier 4 (out of scope)** — `ipf_*`, Roadshow's private packet filter. lwip-amiga skips
  it by design; nothing outside Roadshow's own tools calls it.

`mbuf_*` and `bpf_*` were **promoted from Tier 4 to Tier 3** by the §9 decisions.
`mbuf_*` exposes 4.4BSD mbuf chains to applications; NetX Duo's `NX_PACKET` is a different
shape (single header + payload, chained via `nx_packet_next`), so this is an emulation
layer written from scratch. `bpf_*` needs a raw packet path plus a BPF filter VM. Both are
milestone 7 (§8) — real work, not trim.

### 3.3 Everything else a stack is expected to provide

`bsdsocket.library` alone is not a usable stack. Roadshow/AmiTCP-era software also expects:

- **`usergroup.library`** — `getuid`/`getpwent`/`getgrent` etc.; AmiTCP's companion, shipped
  by Roadshow too. Many ported Unix tools link it.

  ABI **confirmed 2026-07-24** from two primary sources that agree function-for-function
  and register-for-register: AmiTCP's `fd/usergroup_lib.fd` (© 1993 AmiTCP/IP Group, HUT)
  and Roadshow's `sfd/usergroup_lib.sfd` (v1.4, 2004, Barthel). Both give
  `##base _UserGroupBase`, `##bias 30`, **39 public vectors, LVO −30 … −258** — which the
  toolchain's own `pragmas/usergroup_pragmas.h` corroborates exactly (0x01e … 0x102).
  No `usergroup.doc` autodoc could be found anywhere, so the vector *table* is settled but
  documented per-call *semantics* are not; the AmiTCP 4.x SDK would close that gap.

  > **Trap, found the hard way.** The local toolchain's `ndk-include/pwd.h` is **not** the
  > usergroup ABI — it is newlib's 10-field 4.4BSD `struct passwd` (with `pw_change`,
  > `pw_class`, `pw_expire`) substituted over Roadshow's. The real usergroup `struct passwd`
  > has **7 fields**. Anything returning a `struct passwd` built from that header is wrong
  > by 12 bytes and misaligned from `pw_gecos` onward. This is why bebbo's AmiTCP inline
  > header calls the type `struct TCP_passwd`. Use private tag names and pin the layout
  > with `_Static_assert`.
- **The `AMITCP` public message port** — `WaitForPort AMITCP` in `S:User-Startup` is the
  canonical "is the stack up" barrier. Cheap to honour, and worth honouring.
- **Config files** — Roadshow's `DEVS:NetInterfaces/<name>`, `DEVS:Internet/name_resolution`,
  `DEVS:Internet/routes`; AmiTCP's `AmiTCP:db/{interfaces,netdb-myhost,static-routes}`;
  the standard `/etc`-style netdb (`services`, `protocols`, `hosts`, `networks`).
  lwip-amiga instead uses a single `ENV:netstack.prefs` — simpler, but breaks tools.

  > **Corrected 2026-07-24**, against the Roadshow 1.15 manual and real in-the-wild
  > interface files. Two errors in the earlier draft, both inherited from secondary
  > sources:
  > - The address mode keyword is **`CONFIGURE=DHCP|AUTO|FASTAUTO`** (plus the literal
  >   `ADDRESS=DHCP`/`NETMASK=DHCP`), *not* `IPTYPE=DHCP`. In real Roadshow `IPTYPE` is
  >   numeric and means the **SANA-II packet type** (default 2048). `IPTYPE=DHCP` is
  >   AmiTCP/Genesis-era vocabulary. A confirmed real file reads:
  >   `device=ariadne.device`, `unit=0`, `configure=dhcp`, `address=…`, `netmask=…`,
  >   `iprequests=64`, `writerequests=64`, `copymode=fast`, `alias=…`,
  >   `hardwareaddress=…`, `id=…`, `filter=…`.
  > - **There is no `DEVS:Internet/default_gateway` in Roadshow 1.15.** The default route
  >   lives in `DEVS:Internet/routes` as `DEFAULT=`/`DEFAULTGATEWAY=`, alongside
  >   `DST`/`HOSTDST`/`NETDST` + `VIA` for specific routes.
  >
  > The parser in `src/config/` accepts both spellings (numeric `IPTYPE` = packet type,
  > alphabetic = address mode) and reads both `routes` and a `default_gateway` file if
  > present, so nothing is lost either way.
- **The command set** — `AddNetInterface`, `Online`, `Offline`, `ShowNetStatus`, `ping`,
  `netstat`, `route`, `nslookup`/`host`, `traceroute`.
- **Self-starting** — modern practice (both AmiTCP_NG and lwip-amiga) is that the library
  brings the whole stack up on first open, rather than requiring a separate daemon.

### 3.4 SANA-II — the driver contract

SANA-II is an `exec.device` protocol; the stack is the client. Any SANA-II driver works
with any SANA-II stack, which is why supporting it buys the entire installed base
(`a2065`, `ariadne`, `ariadne2`, `amiganet`, `xsurf`, `xsurf100`, `cnet`, `ppp`, `slip`,
`uaenet`, `mister_eth`, `wifipi`, …).

Shape of the interface:

| Code | Command | Use |
|---|---|---|
| 2 | `CMD_READ` | one outstanding request **per packet type**; completes when a matching frame arrives |
| 3 | `CMD_WRITE` | transmit one frame |
| 9 | `S2_DEVICEQUERY` | hardware type, MTU, BPS, address field size |
| 10 | `S2_GETSTATIONADDRESS` | MAC |
| 11 | `S2_CONFIGINTERFACE` | set MAC |
| 14/15 | `S2_ONLINE` / `S2_OFFLINE` | link up/down |
| 16/17 | `S2_ADDMULTICASTADDRESS` / `S2_DELMULTICASTADDRESS` | multicast |
| 21/22 | `S2_GETGLOBALSTATS` / `S2_GETSPECIALSTATS` | counters |
| — | `SANA2IOF_RAW` in `io_Flags` | full frames including link header (optional, not universally reliable) |

> **Corrected 2026-07-24.** Raw mode is a **flag on `CMD_READ`/`CMD_WRITE`**, not a pair of
> `S2_RAWREAD`/`S2_RAWWRITE` commands — those names appear in no version of the spec or any
> header on this machine. It also cannot be capability-probed: SANA-II offers no way to ask
> whether a device implements the flag, and a device that *accepts* it and then ignores it
> is indistinguishable from one that honours it — while silently mis-framing every packet.
> The shim therefore ships with raw **off**, requiring an explicit opt-in.

Two properties drive the port layer:

1. **Cooked framing.** In normal mode the driver owns the link-layer header. On TX the
   stack supplies `ios2_PacketType` (EtherType) + `ios2_DstAddr` + *payload*; on RX the
   driver hands back `ios2_PacketType`, `ios2_SrcAddr`, `ios2_DstAddr` + *payload*.

   > **Corrected 2026-07-24.** An earlier draft of this section claimed NetX Duo prepends
   > the Ethernet header itself and that the shim must strip it on TX. That is wrong.
   > NetX Duo reserves `NX_PHYSICAL_HEADER` (16) bytes of headroom and leaves the link
   > header entirely to the driver: `nx_ram_network_driver.c:388` moves `prepend_ptr`
   > back by `NX_ETHERNET_SIZE` and writes all 14 bytes itself, taking the destination
   > from `nx_ip_driver_physical_address_msw/lsw`. So in cooked mode the shim **never
   > builds an Ethernet header at all** — payload goes straight from `prepend_ptr`, and
   > retransmitting the same `NX_PACKET` works for free. Only RX needs synthesis, and
   > only so the receive path can demux EtherType.

   RX still **synthesises** a header from `ios2_SrcAddr`/`ios2_DstAddr`/`ios2_PacketType`
   — mechanical, but it must be exactly right for ARP. Note that delivery must go through
   `_nx_ip_packet_deferred_receive` / `_nx_arp_packet_deferred_receive` /
   `_nx_rarp_packet_deferred_receive` rather than `_nx_ip_packet_receive`: the reader is
   not the IP thread, and `_nx_ip_packet_receive` does not demux EtherType, so handing it
   an ARP frame silently breaks ARP.
2. **Buffer-management hooks.** `OpenDevice` is passed `ios2_BufferManagement` tags
   `S2_CopyToBuff` / `S2_CopyFromBuff` (plus optional `S2_PacketFilter`,
   `S2_CopyToBuff16`/`S2_CopyFromBuff16`). The driver calls *our* hooks in m68k register
   convention (`a0`=dst, `a1`=src, `d0`=len) to move packet data, because it may be using
   DMA or chip RAM. This is a natural place to copy straight into an `NX_PACKET` payload
   — one copy, no bounce buffer.

`CMD_READ` is per-packet-type, so the stack keeps a pool of outstanding reads for
`0x0800` (IPv4), `0x0806` (ARP), and — for IPv6 — `0x86DD`, each with its own reader.

---

## 4. The existing-stack landscape (and why "AmiTCP is outdated" is true but incomplete)

| Stack | Licence / availability | API level | Notes |
|---|---|---|---|
| **AmiTCP/IP 3.0b2** | Free, **source on Aminet**; the origin of `bsdsocket.library` | v3 | 4.4BSD-derived; no DHCP (BOOTP only); unfinished installer |
| **AmiTCP/IP 4.x** (4.1/4.2/4.3) | **Proprietary** (NSDi, Finland); needs a serial/key file; SDK on Aminet under `LICENSE.SDK` | v4 | Introduced `GetSocketEvents`, `sendmsg`/`recvmsg`, the V4 event API |
| **AROSTCP** | GPL-2 (LGPL-2 for `netinclude`/`netlib`) | v4-ish | AmiTCP HUT sources + Cafferkey/Fedin; ISC DHCP |
| **Genesis / NetConnect** | Free-ish AmiTCP fork, MUI/ReAction GUI; shipped in OS 3.9 | v4 | Configuration widely reported as fiddly |
| **Miami / MiamiDx** | Commercial, **effectively unobtainable** (author lost the key generator) | v4 | Best GUI of its era; slower than AmiTCP/Roadshow |
| **Roadshow** | Commercial, €25; demo nags/disconnects. Olaf Barthel, still maintained (last release 2024). Ships with OS4. **SDK/headers are freely distributable** | v4 + extensions | The de-facto modern reference; defines the extended ABI in §3.2 |
| **EasyNet / EasyNet Pro** | Sold by AmigaKit; free or commercial AmiTCP core | v3/v4 | No DHCP as of 2025 |
| **AS225 / I-Net 225** | Commodore's 1990 stack / its successor | pre-SANA-II | `socket.library`, not `bsdsocket.library`; historical only |
| **AmiTCP_NG** | GPL-2, July 2026 | v4 + Roadshow ABI | See §2.3 |
| **lwip-amiga** | BSD-3, July 2026 | v4 subset (76/121) | See §2.2; no SANA-II |

The licensing situation for an MIT project is clean but narrow: **AmiTCP 3.0b2, AROSTCP
and AmiTCP_NG are all GPL or BSD-with-advertising-clause 4.4BSD lineage, and the AmiTCP 4.x
SDK is proprietary.** None of their *code* can be used. The Roadshow SDK's headers and
autodocs are freely distributable and are the correct **ABI reference** — same posture
AmiTCP_NG documents (reference only, no code, no disassembly). The local NDK in
`amigaos/tools/m68k-amigaos-gcc/.../ndk-include` already contains those headers.

---

## 5. NetX Duo assessment

### 5.1 What it gives us

- **MIT licence** (Microsoft → Eclipse Foundation). Same as this repo. No lineage problem.
- **Dual IPv4 / IPv6** in one stack — an Amiga first.
- **Add-ons already written**: `dhcp` (client+server, v4 and v6), `dns`, `ppp`, `pppoe`,
  `sntp`, `mdns`, `nat`, `auto_ip` (RFC 3927), `telnet`, `ftp`, `http`, `mqtt`, `smtp`,
  `pop3`, `tftp`, `web`, `websocket`, `rtp`/`rtsp`, `snmp`, `ptp`.
- **`nx_secure` TLS 1.2/1.3 + crypto** — an MIT-licensed TLS that could underpin an
  `amissl.library`-alternative, subject to 68k performance reality.
- **`addons/BSD/nxd_bsd.c`** — an existing BSD-sockets veneer (~657 KB of C) with
  `socket`/`bind`/`listen`/`accept`/`connect`/`send`/`recv`/`select`/`poll`/`fcntl`/
  `ioctl`/`getaddrinfo`/`recvmsg`/… Useful as a semantic reference, but see §6.4 — it is
  probably the *wrong* layer to build `bsdsocket.library` on.
- Industrial pedigree: a large regression suite lives in `test/`.

### 5.2 What it costs — the ThreadX dependency

Measured over `common/src` + `common/inc`:

| Category | Symbols |
|---|---|
| Public API used | `tx_mutex_get`/`put` (660 call sites), `tx_event_flags_*`, `tx_thread_create`/`delete`/`identify`/`sleep`/`preemption_change`, `tx_timer_create`/`delete`/`deactivate`, `tx_time_get` |
| **ThreadX internals used** | `_tx_thread_current_ptr`, `_tx_thread_system_suspend`, `_tx_thread_system_resume`, `_tx_thread_preempt_disable`, `_tx_thread_system_preempt_check`, `_tx_thread_timeout`, `_tx_thread_terminate`, `_tx_thread_system_state`, and direct writes to `TX_THREAD` fields `tx_thread_suspend_cleanup`, `tx_thread_suspended_next/previous`, `tx_thread_suspend_status`, `tx_thread_suspend_control_block`, `tx_thread_additional_suspend_info` |

~40 core files build their own suspension lists on top of ThreadX's thread control block —
that is how `nx_tcp_socket_receive(..., wait_option)` blocks a caller. A shim that maps
`tx_mutex_*` onto `SignalSemaphore` will not satisfy this. **Real ThreadX is required.**

Confirmed absent: no `NX_STANDALONE`, no bare-metal mode, no OS abstraction layer
(open upstream issue #182 asks for exactly this; no maintainer answer).

### 5.3 68k-specific concerns, resolved

- **Endianness** — NetX Duo is endian-parameterised: leaving `NX_LITTLE_ENDIAN` undefined
  compiles the big-endian path, where `NX_CHANGE_ULONG_ENDIAN`/`NX_CHANGE_USHORT_ENDIAN`
  become no-ops. m68k is the *native* wire order; header swapping disappears entirely.
  This is a real performance win over x86-class targets.
- **Alignment** — NetX Duo does 32-bit accesses into packet headers. On m68k, longword
  access at any **even** address is legal (only odd addresses raise an address error), and
  the Ethernet header is 14 bytes, so the IP header lands at an even offset. Unlike ARMv5
  or SPARC, no alignment fixup layer is needed. (68020+ handles it without penalty beyond
  an extra bus cycle.)
- **Compiler** — the codebase is clean C89/C99 with no GCC-specific extensions; GCC 15.2
  for m68k accepts it unmodified (§5.4).
- **`ULONG`/`LONG` = 32-bit** on m68k, matching NetX Duo's assumption exactly (the Linux
  port needs a special case for LP64; we don't).

### 5.4 Empirical build spike (m68k)

Method: hand-written minimal `tx_port.h` (types, `TX_MAX_PRIORITIES`, extension macros,
placeholder `TX_DISABLE`/`TX_RESTORE`) + `nx_port.h` derived from the Linux port with
`NX_LITTLE_ENDIAN` removed. Compiler:
`m68k-amigaos-gcc 15.2.0 -c -O2 -m68020 -fomit-frame-pointer`.

```
ThreadX  common/src   : 185/185 compiled,   0 failures   27,708 bytes .text
NetX Duo common/src   : 511/511 compiled,   0 failures  111,856 bytes .text
addons/dhcp           :   4/4   compiled,   0 failures   59,584 bytes (client alone: 15,184)
addons/dns            :   1/1   compiled,   0 failures    9,492 bytes
addons/ppp            :   1/1   compiled,   0 failures   22,168 bytes
addons/sntp           :   1/1   compiled,   0 failures   14,468 bytes
addons/BSD            :   0/1 — one clash: nxd_bsd.h redefines struct timeval vs toolchain headers
addons/tftp           :   1/2 — server needs FileX (fx_api.h); client fine
```

Per-subsystem `.text` (whole-object; the linker pulls only what's referenced, so a real
link is smaller):

```
nx_tcp 23,208   nx_ip 23,276   nx_icmp 12,952   nx_arp 5,884   nx_udp 4,592
nx_packet 3,108   nx_igmp 2,352   nx_rarp 1,088   nx_system 100
IPv6 add-on: nxd* 13,408 + nx_icmpv6 9,532 + nx_ipv6 5,436 + nx_nd 1,472  ≈ 30 KB
```

Notes: `-noixemul` is not usable with this (newlib-based) toolchain — it breaks
`sys/reent.h`. The library build will want `-fbaserel`/`-msmall-code` experiments and a
no-libc discipline anyway (a shared library cannot drag in newlib's stdio); NetX Duo's
only libc uses are `memset`/`memcpy`/`memcmp`, which we supply.

The three biggest single objects are `nx_tcp_socket_send_internal` (2,884), `nx_md5`
(2,716 — droppable) and `nx_tcp_packet_process` (2,308). Nothing pathological.

**Conclusion: the port is a plumbing problem, not a portability problem.**

---

## 6. Proposed architecture

```
   Application task                Application task
        │  OpenLibrary("bsdsocket.library", 4)
        ▼
 ┌──────────────────────────────────────────────┐
 │ bsdsocket.library  (per-opener child bases)  │  ← the compatibility surface (§3.2)
 │  descriptor table · errno ptr · WaitSelect   │
 │  · SocketBaseTagList · socket event signals  │
 └───────────────┬──────────────────────────────┘
                 │  native NetX Duo API (nx_tcp_socket_*, nx_udp_socket_*, nxd_*)
 ┌───────────────▼──────────────────────────────┐
 │ NetX Duo core (MIT, unmodified)              │
 │  IPv4/IPv6 · TCP · UDP · ICMP · ARP · IGMP   │
 │  + addons: DHCP, DNS, (PPP, SNTP, mDNS…)     │
 └───────────────┬──────────────────────────────┘
                 │  NX_IP_DRIVER (NX_LINK_* commands)
 ┌───────────────▼──────────────────────────────┐
 │ sana2 driver shim                            │
 │  cooked↔Ethernet header · CopyTo/FromBuff    │
 │  · CMD_READ pipeline per packet type         │
 └───────────────┬──────────────────────────────┘
                 │  exec IORequests
          SANA-II device (a2065 / ariadne / xsurf / ppp / uaenet / …)

 ┌──────────────────────────────────────────────┐
 │ ThreadX (MIT, unmodified core) + Exec port   │  ← the hard part (§6.2)
 │  TX_THREAD ↔ struct Task · timer tick        │
 └──────────────────────────────────────────────┘
```

Four new components, in dependency order.

### 6.1 `port/amiga/` — ThreadX Exec port

Model it on `threadx/ports/linux/gnu` (2,594 lines total, 8 files) — the only
non-bare-metal precedent:

| File | Amiga realisation |
|---|---|
| `inc/tx_port.h` | types (already proven), `TX_THREAD_EXTENSION_0` carries `struct Task *` + run-signal mask, `TX_DISABLE`/`TX_RESTORE` → `Forbid()`/`Permit()` (task-level; no ISR touches ThreadX state — see §6.2) |
| `tx_thread_schedule.c` | pick `_tx_thread_execute_ptr`, `Signal()` its task's run bit, master waits |
| `tx_thread_system_return.c` | current thread yields back to the scheduler |
| `tx_thread_context_save/restore.c` | no-ops in a hosted port (Exec does the real switching) |
| `tx_thread_stack_build.c` | trivial — stacks belong to Exec tasks |
| `tx_timer_interrupt.c` | driven by a dedicated tick task on `timer.device` (or a VBlank server); `NX_IP_PERIODIC_RATE` must equal `TX_TIMER_TICKS_PER_SECOND`. **See the tick-rate finding below — 100 Hz on `UNIT_MICROHZ` is the wrong choice for this platform.** |

#### Tick rate: 100 Hz on `UNIT_MICROHZ` is wrong for AmigaOS

The port currently runs `TX_TIMER_TICKS_PER_SECOND` = 100 off `timer.device`
`UNIT_MICROHZ`, re-arming a one-shot request every tick. Measured under load in the
soak test: **the clock runs 4–5% slow** (3008 ticks in 31.8 s wall), because each tick
pays the scheduling latency of a fresh IORequest round trip through a Task.

What the incumbent stack does, from the AmiTCP-derived AROSTCP sources:

- `bsdsocket/sys/kernel.h`: **`#define hz (50)`** — a 50 Hz computational clock.
- `bsdsocket/kern/amiga_time.c:111`: `OpenDevice(TIMERNAME, **UNIT_VBLANK**, …)` — the
  tick comes from the vertical-blank interrupt, not the microsecond timer.
- `bsdsocket/kern/uipc_domain.c`: `timeout(pfslowtimo, 0, **hz / 2**)` and
  `timeout(pffasttimo, 0, **hz / 5**)` — the classic 4.4BSD protocol timers at
  **2 Hz (500 ms)** and **5 Hz (200 ms)**.

So TCP itself needs nothing finer than 200 ms. A 50 Hz tick gives 20 ms granularity —
an order of magnitude more than the protocol timers require — at half our current
wakeup rate, from a hardware interrupt rather than a device round trip. `UNIT_VBLANK`
is also documented as having "very low overhead" and being *more* accurate than
`UNIT_MICROHZ` over long periods.

#### What was implemented, and why it is not simply "use VBlank"

**Superseding the recommendation above.** An earlier draft suggested 50 Hz on
`UNIT_VBLANK` with the rate taken from `SysBase->VBlankFrequency`. That is not what
shipped, because the tick source cannot be trusted as the *time base*:

- VBlank is 50 Hz PAL but **60 Hz NTSC**. AROSTCP hardcodes 50, so its clock runs 20%
  fast on an NTSC machine — a bug to avoid inheriting, not a precedent.
- Under **RTG** (Picasso96/CyberGraphX) and on PiStorm/Emu68-class systems, the rate,
  the regularity, and in exotic configurations the existence of a well-behaved chipset
  VERTB are all outside our control.

So the port **separates the two concerns: the tick source provides *wakeups*, `ReadEClock()`
provides *time*.** Each wakeup computes how many 50 Hz periods have actually elapsed and
delivers exactly that many `_tx_timer_interrupt()` calls — catching up after a late or
coalesced wakeup, delivering none after an early one. E-Clock is CIA-derived, independent
of the display, and reports its own frequency, so this is correct on PAL, NTSC, RTG and
accelerated systems alike. Catch-up is capped (8 ticks) and resyncs beyond that rather
than firing a burst under the core lock. The source is validated against E-Clock at
startup and falls back to `UNIT_MICROHZ` if it is missing or out of band.

Measured on the emulated 68020 floor, before → after:

| | 100 Hz `UNIT_MICROHZ` | 50 Hz VBlank + E-Clock |
|---|---|---|
| clock drift | **−5.40%** | **−0.04%** (self-measured 49.98 Hz) |
| wakeups/s actually delivered | 94.7 (5% silently lost) | 49.5 |
| µs per wakeup | 463 | 390 |
| **CPU spent on the tick** | **4.38%** | **1.93%** |

Two controls isolate the cause: the same code at 100 Hz on `UNIT_MICROHZ` gives 3000
ticks and at 50 Hz gives 1507 — **the drift fix is the E-Clock catch-up, not the unit
change**. The unit change is what buys the CPU saving.

Caveats that emulation cannot settle: RTG itself is untestable under FS-UAE (native PAL
chipset only), NTSC is untested, and the per-wakeup cost deserves confirmation on real
iron. Also found on the way: **a `timer.device` request that has been `AbortIO`'d does
not complete again when re-armed** — recycling one silently killed the ThreadX clock.
The port no longer aborts and reuses a request.
| `tx_initialize_low_level.c` | create the tick task, the scheduler lock, adopt the caller |

### 6.2 ThreadX-on-Exec: the central problem

The Linux port's model is *one global mutex* — only one ThreadX thread runs at a time,
and `tx_thread_schedule` hands the baton to the highest-priority ready thread. Exec is a
preemptive priority scheduler already, so there are two options:

- **(A) Baton model (port the Linux design).** A single "ThreadX core lock"; the port
  signals the chosen task and blocks the others. Faithful to ThreadX semantics
  (priority, preemption-threshold, `TX_DISABLE` regions all behave), and it makes ThreadX's
  internal data structures safe without touching Exec's scheduler. Cost: every stack
  operation serialises, and each ThreadX-level context switch costs a `Signal`+`Wait` pair.
  At the 14 MHz 68020 floor that is measurable but bounded — this is exactly what lwip-amiga's
  "single core semaphore" does, and it reaches 944 Mb/s on fast hardware.
- **(B) Let Exec schedule, protect ThreadX state with `Forbid`/`Permit`.** Cheaper, but
  ThreadX's ready-list/preemption logic then models a world it does not control, and any
  divergence is a heisenbug in a system with no memory protection.

**Recommendation: (A).** Predictability beats throughput here, and (A) is the only variant
with an upstream precedent to crib from.

Interrupt safety: with (A), nothing in ThreadX or NetX Duo runs at interrupt level — the
SANA-II reader is a Task, packet arrival is an IORequest completion, and the timer tick is
a task. So `TX_DISABLE`/`TX_RESTORE` can be `Forbid()`/`Permit()` rather than
`Disable()`/`Enable()`, which matters a lot for Amiga system health (long `Disable()`
regions break serial/floppy/audio).

### 6.3 Who runs the stack code?

NetX Duo suspends **the calling thread** inside `nx_tcp_socket_receive`, `nx_tcp_socket_send`,
`nx_packet_allocate`, etc. Callers on AmigaOS are pre-existing Exec Tasks that we did not
create. Two ways out:

- **(A) Thread adoption** — when a task opens `bsdsocket.library`, allocate a `TX_THREAD`
  control block for it and register it with the port so ThreadX can suspend/resume it via
  Exec signals. `tx_thread_create`'s stack-build step is skipped for adopted threads.
  Direct-path, no marshalling, matches lwip-amiga's "app tasks execute stack code in their
  own context" and preserves blocking-socket semantics for free.
- **(B) Worker pool** — `bsdsocket` marshals every call to NetX Duo worker threads.
  Simplest port, but a blocking socket ties up a worker, and `WaitSelect` across many
  sockets becomes an N-worker problem. Adds a round-trip per call.

**Decided (§9): prototype both in milestone 1 and pick on evidence**, with (A) as the
preferred outcome and (B) as the documented fallback. This determines the whole port's
shape, so no socket code is written until it is settled.

### 6.4 `bsdsocket.library` on native NetX Duo APIs, not on `nxd_bsd.c`

Tempting to wrap `addons/BSD`, but:

- `WaitSelect` must wait on **sockets *and* Exec signal bits** simultaneously and return
  `EINTR` on break — `nx_bsd_select` has no such concept, so it would have to be
  reimplemented anyway.
- `SetSocketSignals`/`GetSocketEvents` map onto NetX Duo's native receive/connect/disconnect
  **callbacks**, which `nxd_bsd` consumes for its own purposes.
- `ObtainSocket`/`ReleaseCopyOfSocket` need descriptor ownership transfer across library
  bases — outside `nxd_bsd`'s per-`tx_thread` fd model.
- Per-opener errno with caller-chosen width doesn't fit `nx_bsd_set_errno`.
- It is 657 KB of C carrying a POSIX-ish model we then fight.

Use `nxd_bsd.c` as a **semantic reference** (its option/ioctl/`getaddrinfo` handling is
worth mining) and build the library directly on `nx_tcp_socket_*` / `nx_udp_socket_*` /
`nxd_*` / `nx_packet_*`. That is what lwip-amiga did with lwIP's raw API, and it is why it
scores 138/142.

Generate the LVO vector table mechanically from the NDK SFD/FD (`fd2sfd`/`fd2pragma` are
already in the local toolchain), rather than hand-writing 121 stubs.

### 6.5 `sana2/` — driver shim

- One `NX_IP` driver entry handling `NX_LINK_INITIALIZE`, `ENABLE`, `DISABLE`,
  `PACKET_SEND`, `PACKET_BROADCAST`, `ARP_SEND`, `ARP_RESPONSE_SEND`, `MULTICAST_JOIN/LEAVE`,
  `GET_STATUS/SPEED/DUPLEX/ERROR_COUNT`, `SET_PHYSICAL_ADDRESS`, `INTERFACE_ATTACH/DETACH`,
  `UNINITIALIZE`, `DEFERRED_PROCESSING`.
- TX: no header construction at all in cooked mode (see the correction in §3.4) —
  `ios2_PacketType` from the driver command or `nx_packet_ip_version`, `ios2_DstAddr` from
  `nx_ip_driver_physical_address_msw/lsw`, payload straight from `prepend_ptr` →
  `CMD_WRITE`. A pool of write requests, `SendIO`, never `DoIO`: the driver entry is
  reachable from arbitrary threads inside `nx_tcp_socket_send`, so it must never block on
  the wire.
- RX: reader task per packet type (`0x0800`, `0x0806`, optionally `0x86DD`) with N
  outstanding `CMD_READ`s; `S2_CopyToBuff` writes **straight into the `NX_PACKET`
  payload**; synthesise an Ethernet header from `ios2_SrcAddr`/`ios2_DstAddr`/
  `ios2_PacketType` so the receive path can demux; deliver via
  `_nx_ip_packet_deferred_receive`/`_nx_arp_packet_deferred_receive`.

  **Consequence discovered during implementation:** `S2_CopyToBuff` is called at
  interrupt level, so `nx_packet_allocate` cannot happen inside it. Every outstanding
  `CMD_READ` must therefore **pin its `NX_PACKET` in advance**. That fixes the pipeline
  depth as a hard packet-pool cost (currently 4 IPv4 + 2 ARP + 2 IPv6 = up to 8 packets
  permanently pinned, against an `AMI_POOL_MIN_PACKETS` floor of 16) and is what the
  `alloc_failures` counter tracks.
- `S2_DEVICEQUERY` → MTU/BPS/`AddrFieldSize`; `S2_GETSTATIONADDRESS` → MAC;
  `S2_ONLINE`/`S2_OFFLINE` → link enable/disable; `S2_GETGLOBALSTATS` →
  `GetNetworkStatistics`/`netstat`.
- Optional fast path: probe for `S2_RAWREAD`/`S2_RAWWRITE` and use full-frame mode when
  the driver supports it (skips header synthesis), falling back to cooked mode.
- The copy hooks are called in m68k register convention — small asm trampolines.

### 6.6 Configuration, tools, and the "is it up?" contract

**Decided (§9): Roadshow's file layout**, rather than inventing one — this is where
AmiTCP_NG is right and lwip-amiga is convenient-but-incompatible. `DEVS:NetInterfaces/*`,
`DEVS:Internet/name_resolution`, `DEVS:Internet/default_gateway`, standard netdb files.
Create the `AMITCP` public port. Self-start on first `OpenLibrary`. Ship `AddNetInterface`,
`Online`, `Offline`, `ShowNetStatus`, `ping`, `netstat`, `host`, plus `usergroup.library`
alongside (§9, scope).

---

## 7. Risks, ranked

| # | Risk | Severity | Mitigation |
|---|---|---|---|
| 1 | ThreadX Exec port + **thread adoption** (§6.3) proves fragile — races in `_tx_thread_system_suspend` against Exec's own scheduling | **High** | Prototype this *first*, before any socket code. Fall back to the worker-pool model. |
| 2 | No memory protection: a bug anywhere corrupts the whole machine; debugging is Enforcer/Mungwall + emulator | High | Develop under WinUAE/FS-UAE with Enforcer; keep the stack's state in one allocation; assert aggressively in debug builds |
| 3 | `WaitSelect` fidelity — the single most-used call, with subtle documented behaviour | High | `bsdsocktest` from day one; target ≥138/142 |
| 4 | Footprint/perf at the 68020/4 MB floor — NetX Duo's packet pools are not tuned for this, and IPv6 + TLS + `bpf_*` are all in scope | Medium | Size pools from available RAM; keep IPv6, TLS and `bpf_*` behind build options so the floor build stays small; measure with `sockbench`-style tooling |
| 4b | **Scope breadth** — v1 carries IPv6, `usergroup.library`, TLS and `mbuf_*`/`bpf_*`, none of which lwip-amiga or AmiTCP_NG attempt in full | Medium | Milestones 7–9 are strictly after a passing `bsdsocktest`; TLS ships as its own library so it can slip without blocking the stack |
| 5 | Two fresh competitors (§2.2, §2.3) may reach "good enough" first | Medium | Differentiate on SANA-II + IPv6 + MIT + TLS; treat their public ABI notes as free validation |
| 6 | Licence hygiene — no AmiTCP/AROSTCP/Roadshow code may be copied | Medium | Reference the freely-distributable Roadshow SDK headers/autodocs for the **ABI only**; document the clean-room posture in the README as AmiTCP_NG does |
| 7 | `mbuf_*` / `bpf_*` have no NetX Duo analogue, and both are now in scope (§9) | Medium | Milestone 7, after the socket surface is proven. `mbuf_*` emulates 4.4BSD chains over `NX_PACKET`; `bpf_*` needs a raw path + filter VM. `ipf_*` stays stubbed |
| 8 | Shared-library build discipline (no newlib stdio, base-relative data, per-opener bases) | Low | Solved problem; the local toolchain and NDK have the machinery |

---

## 8. Suggested milestone order

1. **Spike: ThreadX on Exec.** Port the 8 Linux port files; run ThreadX's own
   `sample_threadx.c` (two threads, a queue, a semaphore, a timer) under FS-UAE. Nothing
   else matters until this is solid. Then build **both** §6.3 candidates against it —
   thread adoption (a pre-existing Exec task calling `tx_mutex_get`/`tx_thread_sleep`
   and being suspended/resumed by ThreadX) and a minimal worker pool — and pick on
   evidence. Exit criterion: a soak test with 4 adopted tasks contending on a mutex and a
   timer, clean under Enforcer.
2. **NetX Duo on the RAM driver.** `nx_ram_network_driver.c` already exists in
   `common/src`; bring up two `NX_IP` instances talking TCP to each other in one Amiga
   process. Proves the core + packet pools + timers on 68k.
3. **SANA-II shim.** Point it at `uaenet.device`/A2065 under WinUAE with SLIRP. Success =
   DHCP lease + `ping` to the gateway.
4. **`bsdsocket.library` Tier 1.** Library skeleton, per-opener bases, generated LVO table,
   `WaitSelect`. Success = `bsdsocktest` loopback categories passing.
5. **Tier 2 + DNS/DHCP integration**, then `bsdsocktest` network tier against the host helper.
6. **Tier 3 Roadshow parity + tools + `usergroup.library`.** Success = stock Roadshow-era
   software (`AmiTCP`-linked clients, `smbfs`, a browser) running unchanged against the
   Roadshow config layout.
7. **`mbuf_*` over `NX_PACKET` and the `bpf_*` raw path + filter VM.** Own milestone —
   no upstream support to lean on. Success = a `tcpdump`-shaped tool capturing on a real
   interface.
8. **IPv6 build option** — third SANA-II reader for `0x86DD`, `nxd_*` socket paths,
   DHCPv6/RA. Success = `bsdsocktest` passing with IPv6 enabled and an IPv6 `ping`.
9. **`nx_secure` TLS**, as a separate library on the same core. Benchmark a TLS 1.2
   handshake on a real 68020 *before* committing to an API.

Adopt **`bsdsocktest` (142 tests)** as the acceptance gate throughout; lwip-amiga's
138/142 is the public bar.

---

## 9. Decisions (2026-07-24)

| # | Question | Decision |
|---|---|---|
| 1 | Minimum target | **68020 + OS 3.1, 4 MB.** Build `-m68020`; size packet pools for ~4 MB Fast RAM; IPv6 is a build option, not a stretch. |
| 2 | Thread adoption vs worker pool (§6.3) | **Prototype both in milestone 1, then decide.** Adoption is the preferred outcome; the worker pool is the documented fallback. No socket code until this is settled. |
| 3 | Configuration model | **Roadshow layout.** `DEVS:NetInterfaces/*`, `DEVS:Internet/name_resolution`, `DEVS:Internet/default_gateway`, standard netdb files, `AMITCP` public port. No `ENV:`-only mode. |
| 4 | Optional subsystems in scope for v1 | **All four**: IPv6 (build option), `usergroup.library`, `nx_secure` TLS, and the `mbuf_*` / `bpf_*` vectors. |

Consequences of decision 4 worth stating plainly, since it is the widest of the options:

- **`mbuf_*` / `bpf_*` move from Tier 4 to Tier 3** in §3.2. `mbuf_*` means emulating 4.4BSD
  mbuf chains over `NX_PACKET` (NetX Duo has no equivalent shape); `bpf_*` means a raw
  packet path plus a BPF filter VM. Neither has upstream support to lean on — lwip-amiga
  skips both, AmiTCP_NG inherits `mbuf_*` from its 4.4BSD core and wrote `bpf_*` itself.
  Budget these as their own milestone, not as trim on the socket work.
- **`nx_secure` TLS on a 68020 is the one item where the target decision fights the scope
  decision.** TLS 1.2 handshake RSA/ECDHE on a 14 MHz 68020 is seconds, not milliseconds.
  Recommend it ships as a separate library (an `amissl.library`-shaped thing) built on the
  same core, gated behind its own build option, and that it is benchmarked on 68020 before
  any API is promised. Keeping it in the v1 *plan* is fine; keeping it on the v1 *critical
  path* is not.
- `ipf_*` (Roadshow's private packet filter) remains out of scope — it is undocumented
  beyond its vector offsets and nothing outside Roadshow's own tools calls it.

### M9 gate result (2026-07-25): TLS is NOT viable on the 68020 floor

The §9 decision made TLS conditional on a 68020 benchmark. It has been run, on-Amiga,
with a **real TLS 1.2 handshake** completing between an `nx_secure` client and server
(`TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256`, 38/38 checks):

| | 68020 |
|---|---|
| **Full handshake, connect → first encrypted record** | **185.5 s** |
| RSA-2048 private, no CRT / CRT | 158.0 s / 44.4 s |
| RSA-2048 public | 1.98 s |
| ECDHE P-256 shared secret | 5.18 s |
| ECDSA P-256 verify | 6.97 s |
| SHA-256 / AES-128-CBC, 1 KB | 23.2 ms / 21.9 ms |
| AES-128-**GCM**, 1 KB | 344.6 ms |

`nx_secure` compiles clean for m68k (361/361 files, ~208 KB linked) and works — it is
simply too slow as a transparent socket layer. **Offload (`catalyst`, `AmiSSL-Tunnel`)
is the realistic path for the floor target.** Note the 185 s figure has client *and*
server on one CPU; a **client-only** handshake derives to **~13–20 s**, since a client
never performs the private-key operation. That is inside a typical server timeout, so a
deliberate "fetch this URL" operation is defensible where a transparent socket is not.

Findings worth keeping regardless of whether TLS ships:

- **`nx_secure` uses RSA CRT on only one path** — `NX_CRYPTO_SET_PRIME_P` appears solely
  in `nx_secure_process_client_key_exchange.c`; the ECDHE_RSA ServerKeyExchange signature
  and `send_certificate_verify` pass the full 2048-bit private exponent. A measured
  **3.6×** penalty, on a server path we would only hit if we ever act as a server.
- **AES-GCM is 20× slower than AES-CBC**, because `nx_crypto_gcm.c`'s GHASH is a
  bit-serial GF(2¹²⁸) multiply. TLS 1.3 mandates AEAD, so **TLS 1.3 is impractical as
  shipped**. A 4-bit table-driven GHASH would be ~20–30×.
- **`nx_secure` has no ChaCha20-Poly1305** (verified: no source files, no ciphersuite
  entries). That is the AEAD a 68k would want — ChaCha20 is pure ARX with no multiplies —
  so making TLS 1.3 practical means *writing* it, not just tuning GHASH.
- **ECDSA P-256 verify is 3.5× slower than RSA-2048 verify** here, inverting the usual
  modern advice: prefer RSA certificate chains on this hardware. The industry's drift
  toward ECDSA certificates therefore works against us.
  **→ Superseded by the P-256 work below**: verify is now 1.961 s against RSA's 0.681 s,
  so the gap is 2.9× on a sub-second operation rather than 3.5× on a seven-second one.
  ECDSA chains are fine; RSA is merely still cheaper.
- **`NX_RAND` is undefined**, so `nx_api.h` falls back to newlib `rand()` — a 32-bit LCG —
  to generate ECDHE private keys and the client random. **A shipping blocker for any real
  TLS use**, independent of speed.
  **→ Half-addressed**; see "The `NX_RAND` problem" below. The generator is now a SHA-256
  hash DRBG instead of an LCG, which fixes the expansion. The *entropy* remains the
  blocker, and the module now says so in its own API rather than in a comment.

Toolchain landmine found here, relevant project-wide: this toolchain ships a **zero-byte
`libgcc.a`** and nothing exports `__udivdi3`, so any link pulling in 64-bit division
fails. `src/common/ami_udivdi3.c` supplies it with a 68020 `divu.l` fast path.
It began life as `src/tls/tls_udivdi3.c` with a note to move it to `src/common/` when a
second component needed it; by the time anyone looked there were three copies (the TLS
one, `tests/conformance/compat/libgcc64.c` for newlib's `printf`, and a second CMake
target compiling the first file again for `src/crypto68k/`). One copy now, built as
`aminetxduo_m68k_rt`, and `tests/conformance/build.sh` compiles that same file rather
than keeping its own.

#### The `NX_RAND` problem (2026-07-25): an entropy pool that says it is not enough

`NX_RAND` now points at `src/common/ami_random.c` — a SHA-256 hash DRBG over an entropy
pool — instead of newlib's `rand()`. That closes the *expansion* hole: with an LCG, one
32-bit output is the entire state, and a TLS client random goes on the wire in clear, so
an observer who sees it can compute the ECDHE private key that follows. The DRBG is
counter-mode `SHA-256(key ‖ counter)` with a forward ratchet after every 32-byte block,
so a disclosed block reveals neither its predecessors nor the key that made it.

**The entropy is still the blocker, and the module now says so in its API rather than in
a comment.** `ami_random_is_seeded()` reports FALSE until the pool has been credited 64
bits, and the internal collection *cannot reach that by construction* — the four sources
credited anything at all cap at 8 + 4 + 2 + 12 = 26 bits. So the answer to "may I run a
TLS handshake?" on an unattended Amiga is **no**, and the caller has to supply a seed
through `ami_random_add_entropy()` to change that.

What the sources are actually worth, measured by `tools/smoke/randtest.c` across three
cold boots of an emulated 68020 (FS-UAE, identical boot image):

| source | varies across cold boots? | credited |
|---|---|---|
| `GetSysTime()` wall clock | **yes** — `1532507776.382025` / `…783.606435` / `…798.617969` | 8, and 0 when `tv_secs == 0` (no battery clock) |
| E-Clock interval jitter | **yes** — 22–27 distinct deltas out of 256 samples, 52–263 ticks | 7–9 measured, capped at 12 |
| `IdleCount` / `DispCount` | **yes** — 44/66/57 and 282/276/283 | 4 |
| task-list walk | not measurably | 2 (charity; a real Workbench would earn it) |
| `AvailMem()` ×4 | **no** — identical to the byte, all three runs | 0 |
| `AllocVec()` address | **no** — identical, all three runs | 0 |
| uninitialised `MEMF_ANY` residue | **no** — identical, all three runs, *and non-zero* | 0 |
| `AttnFlags`, `VBlankFrequency`, E-Clock rate, `FindTask(NULL)`, `LastAlert` | no | 0 |

Two findings worth keeping:

- **FS-UAE is not the metronome the plan assumed.** The E-Clock interval genuinely varies
  under emulation, so the jitter source is not dead there — but the variation comes from
  the *host's* scheduler, and how much of that an attacker can see or influence is exactly
  what nobody has analysed. Hence the cap at 12 bits rather than the ~4.6 bits/sample that
  24 distinct values would nominally support.
- **"Non-zero therefore unpredictable" is a trap, and this code fell into it.** The first
  version credited 8 bits for uninitialised `MEMF_ANY` residue whenever any byte came back
  non-zero. It always came back non-zero — and always the same sixteen bytes, because the
  allocator hands the same block to the same caller at the same point in the same boot
  sequence. The credit was 31 bits before that was measured and ~21 after. Everything
  still goes *into* the pool; it no longer goes into the accounting.

**Three cold boots produced three different output streams** (`b0bfd079…`, `259ea84d…`,
`1862a377…`), which is the good outcome and is *not* the same claim as unpredictability —
the difference comes from the host wall clock and the E-Clock jitter, and an attacker who
knows when the machine was switched on has most of the first of those.

The SHA-256 is verified: the empty string, `"abc"`, both FIPS 180-4 Appendix B multi-block
examples and the one-million-`a` vector, run through the implementation lifted verbatim
into a host harness. Worth recording that the *first* harness reported all five as
failures — it typed `ULONG` as `unsigned long`, which is 64 bits on a modern host and 32 on
m68k-amigaos. The code was right and the test was wrong, which is the more dangerous way
round; `src/common/ami_random.c` now carries a compile-time assertion on the width.

**What an attacker-facing assessment would say:** this is a well-conditioned DRBG on a
badly-sourced seed. Against an off-path attacker guessing a TCP initial sequence number or
a DNS query id it is a large improvement on the LCG and is fine. Against anyone attacking a
TLS session key it is **not adequate** — around 20 bits of credited entropy, over a source
set that is unaudited, on a machine whose boot time an adversary on the same LAN can
observe. **Do not enable `AMINETXDUO_TLS` for adversarial use without supplying a seed.**

What would actually improve it, roughly in order of value:

1. **A persisted seed file.** Read `DEVS:Internet/random_seed` at startup, mix it, write 32
   fresh bytes back immediately. This is the single change that breaks the "every boot
   starts from the same place" property, and it is how every Unix has solved this since
   the 1990s. Not implemented — it needs `dos.library` in a path that is currently
   `exec`-only, and the decision of where to put it belongs with whoever ships TLS.
2. **User input timing.** A `Process` can sample `IECLASS_RAWKEY` / mouse timings from
   `input.device`. Slow to accumulate, and genuinely unpredictable.
3. **An operator seed.** A passphrase or a file the human supplies, credited by the human.
   `ami_random_add_entropy()` already takes it; nothing calls it yet.
4. **A proper analysis of the E-Clock jitter** on real hardware, of the kind the Linux
   jitter RNG has had. Until that exists, the 12-bit cap is a guess dressed as a number.

Cost, measured: `ami_random_init()` takes **21–22 ms** on the emulated 68020 (nearly all of
it jitter sampling), called once from `bsd_runtime_open()`. Steady state is one SHA-256 per
eight `NX_RAND()` calls — a 200 packet/s TCP stream spends well under 1% of the CPU there.
`bsdsocket.library` grew **5,224 bytes** of text: `ami_random.o` is 5,328 (SHA-256 plus the
collection), less the 104 saved by deleting the xorshift in `library_runtime.c` that it
replaces. `Forbid()` is taken per 32-byte block rather than per request, so the longest
uninterruptible stretch is one SHA-256 pair and not the 200 ms a 16 KB draw would
otherwise hold the scheduler off for.

#### Update: `src/crypto68k/` makes RSA 8× faster — the blocker moves to EC

A follow-on optimised the bignum arithmetic (`src/crypto68k/`, behind
`AMINETXDUO_CRYPTO68K_ASM`). Measured on the emulated 68020, pairs run back-to-back:

| | reference | crypto68k | ratio |
|---|---|---|---|
| RSA-2048 public, e=65537 | 2.011 s | **0.681 s** | 2.9× |
| RSA-2048 private, CRT | 44.75 s | **20.05 s** | 2.2× |
| RSA-2048 private, plain | 160.83 s | 66.40 s | 2.4× |
| Montgomery square, 2048-bit | 52.17 ms | 26.90 ms | 1.9× |

**Stacked with enabling CRT (3.6×, which is orthogonal): 160.83 s → 20.05 s = 8.0×** on
an RSA private operation. Correctness: 4964 checks, 0 failures, against Python-derived
known answers *and* differentially against the unmodified vendored code.

**The biggest single lever was not assembly.** The vendored exponentiation walks all 32
bits of the top exponent limb, so `e = 65537` (`0x00010001`) costs 32 squarings where 16
are needed; sliding-window plus leading-zero skipping nearly doubles every RSA *public*
operation on its own. Karatsuba was costed and **rejected** (~5% — Montgomery reduction
is not Karatsuba-able, so only half the work is eligible). SOS was chosen over the
textbook CIOS recommendation because this machine's fast path is `ADD.L Dn,(An)+`, which
requires destination == source — an addressing-mode argument, not an operation count.

**Consequence for viability: RSA is no longer the blocker for a TLS client.** Three
RSA-2048 public operations per handshake go from 5.95 s to **2.04 s**. What now dominates
is elliptic-curve arithmetic, untouched by that work: ECDHE P-256 shared secret 5.18 s
and ECDSA P-256 verify 6.97 s, so an ECDHE_ECDSA handshake is still ~30 s.

**→ Also superseded.** The follow-on took the EC arithmetic 3.6–3.9× (table below), so
that ~30 s of asymmetric work is now **3.71 s**. The expectation stated here — that the
~1.4× limb-loop win would carry over — turned out to be the *wrong* prediction: the limb
loop was not the bottleneck, the field *representation* was. See below.

#### Update: `src/crypto68k/c68k_p256.*` makes P-256 3.7× faster — EC is no longer the blocker either

The follow-on to the follow-on. Measured on the emulated 68020,
`tests/crypto68k/crypto68k_ec_bench`, every pair run back to back in one process
with **nothing changed but one function pointer** in a copy of the curve struct
(`nx_crypto_ec_multiple`), so the ratios are what a handshake would actually see:

| | reference | crypto68k | ratio |
|---|---|---|---|
| **ECDSA P-256 verify** | 7.028 s | **1.961 s** | 3.6× |
| **ECDHE P-256 shared secret** | 5.245 s | **1.368 s** | 3.8× |
| **ECDHE P-256 keygen** | 1.475 s | **0.381 s** | 3.9× |
| generic scalar multiply `k·Q` | 5.184 s | 1.334 s | 3.9× |
| fixed-base scalar multiply `k·G` | 1.475 s | 0.380 s | 3.9× |
| Jacobian point doubling | 14.97 ms | 3.88 ms | 3.9× |
| Jacobian + affine addition | 16.62 ms | 5.19 ms | 3.2× |
| field multiply + reduce | 1.466 ms | 0.449 ms | 3.3× |
| **P-256 Solinas reduction alone** | 1.019 ms | **0.084 ms** | **12.1×** |

The three vendored absolutes reproduce the M9 gate figures above (1.52 / 5.18 /
6.97 s) to within 1%, which is the cross-check that the benchmark is measuring
the same computation.

**Only the 68020 column is meaningful.** The same binary under FS-UAE's 68030
model reports 4.5–5.0× — and an ECDSA verify of 196 ms against the 68020 model's
7028 ms, which is 36× and is not a clock ratio. The 68030 model does not charge
for `MULU.L`, so it flatters exactly the work this change moves *out* of the
multiply and into carry chains. Quoted here so nobody re-measures it and
believes it.

**None of the textbook algorithmic levers were missing, and that is the finding.**
`nx_crypto_ec.c` already has NAF point multiplication, Solinas reduction rather
than Barrett or Montgomery, a real Yang squaring, Jacobian coordinates with a
single final inversion, and a fixed-base comb table for `G` — and the comb **is**
reached on the paths that matter (verified: `_nx_crypto_ec_fp_projective_multiple`
dispatches on pointer identity with `curve->nx_crypto_ec_g`, and both ECDSA
verify's `u1·G` half and ECDH key generation pass exactly that pointer; the
measured 1.48 s keygen against 5.18 s shared secret *is* the comb working).

What was slow was the layer underneath. **`_nx_crypto_ec_secp256r1_reduce()` is
67% of a field multiply** — measured, not estimated — because it does not work on
limbs at all: it serialises the value into a 64-byte big-endian byte stream one
byte at a time, memmoves it, parses 32 bytes back into limbs one byte at a time,
then builds nine 8-limb terms with 64 more per-word byte swaps and adds them as
sign-carrying variable-length huge numbers. Collapsing that to one pass over
eight limb positions with a signed carry — the same mathematics, no byte touched
— is **12.1×** on the reduction and most of the 3.8× overall.

**Hand-written assembly was worth 1.13×, and only for the carry chains.**
`c68k_p256.S` covers the eight-limb add and subtract and the reduction's 63-term
pass, because C has no carry flag and GCC therefore spends five instructions and
a branch where `ADD.L`/`ADDX.L` needs two (both plausible C spellings were
compiled and disassembled first; the 64-bit-accumulator form emits a `CLR.L` per
term instead of hoisting one zero register). The **multiply is deliberately left
to the compiler** — it is already within ~25% of the 68020's `MULU.L` floor, which
is the same conclusion the RSA work reached.

**Shamir's trick for ECDSA verify was costed and rejected.** The usual argument —
verify is `u1·G + u2·Q`, two scalar multiplications, interleave them and halve the
doublings — does not apply here, because `u1·G` is not a generic scalar
multiplication. It is a comb, and a comb needs 26 doublings, not 256; interleaving
would drag the `G` half up to 256 shared doublings to save the 26 it already needs.
Priced at the measured point-operation costs: 284 doublings + 100 additions
(separate) against 258 + 93 (Shamir) = 1.62 s against 1.48 s, about **8%** of a
verify, for a second scalar routine and a second static table. Available if
anyone needs it; not free, and not the 1.5–1.8× the textbook promises.

Correctness: **1730 checks, 0 failures** (`tests/crypto68k/crypto68k_ec_test`) —
RFC 6979 A.2.5 published signatures verified through the real
`_nx_crypto_ecdsa_verify`, 1600 field operations and 70 scalar multiplications
differentially against the unmodified vendored code including k = 0, 1, 2, n−1,
n, n+1, 2²⁵⁶−1 and the small scalars where the accumulator meets a table entry,
a known-answer ECDH secret computed from both sides, and **ten invalid
signatures that must be and are rejected** — because an "optimisation" that made
verify always succeed would pass every positive test in the file.

**Consequence for viability: a client ECDHE_ECDSA handshake's asymmetric
arithmetic goes from 13.7 s to 3.7 s.** Combined with the RSA work, that
removes the last of the "tens of seconds" from a TLS client's public-key cost;
what remains is one keygen, one ECDH and one verify per certificate. ECDHE_RSA
is no longer obviously preferable — an RSA-2048 verify is 0.681 s against
1.961 s for ECDSA, so an RSA chain is still cheaper per certificate, but the gap
is now 3× on a sub-second operation rather than 3.5× on a seven-second one.

Prior art, worth knowing before anyone re-treads it:

- **Howard Chu wrote a complete 68020 OpenSSL bignum assembly in 2002** (`bn_m68k.s`,
  1604 lines, all ten BN primitives with unrolled Comba kernels). It was never merged
  upstream — OpenSSL has no m68k bignum asm at any tag — but **it survives in AmiSSL** and
  is built for `amiga-os3-68020`. GMP, libgcrypt and mbedTLS all ship m68k `MULADDC`
  variants; libtommath, wolfSSL and nettle have nothing.
- **Chu's "over 4× faster than gcc" does not transfer.** It was measured against GCC 2.95,
  which called a helper. **GCC 15.2 already emits `MULU.L`** — verified in the
  disassembly — so the realistic ceiling is ~1.4× on the limb loop, and expecting 4×
  would be chasing a number that no longer exists.
- **`MULU.L` 32×32→64 is NOT implemented on the 68060** — it traps to vector 61 and is
  emulated. AmiSSL disabled Chu's assembly for 68060 for exactly this reason. Our floor
  is 68020 so this is fine, but `AMINETXDUO_CRYPTO68K_ASM` must never be enabled for a
  68060 target.
- **Real-world yardstick:** AmiSSL issue #67 instruments a TLS 1.3 `SSL_connect` on an
  **A3000 68030@25 MHz at 2.99 s** (AmiSSL 4.12) / 5.21 s (5.4), and issue #11 records a
  68060@50 taking 18.8 s against a 60 s server timeout. Our figures sit in the same
  universe, which is the best cross-check available without real hardware.

> **objdump trap, cost someone an afternoon's worth of wrong conclusions if unnoticed:**
> the toolchain's default objdump architecture is plain 68000, which cannot decode
> `MULU.L` and prints it as `.short 0x4c06`. Without `-m m68k:68020` you conclude the
> compiler emits no 32×32 multiply at all. Verified here: `.short 0x4c06` becomes
> `mulul d6,d2,d3` with the right `-m`.

### M8 result (2026-07-25): the IPv6 dual stack works, over a real wire

`-DAMINETXDUO_IPV6=ON` builds a dual stack that has been run on an emulated 68020 and
68030 and does, as far as can be established, something no classic Amiga TCP/IP stack
has done before: **it speaks IPv6.**

**FS-UAE's SLIRP DOES carry IPv6** — the assumption that it would not is wrong, and
finding that out changed what "verified" means for this milestone. FS-UAE 3.2.35's
`qemu-uae` plugin links a libslirp with the v6 half compiled in (`ip6_input`,
`ip6_output`, `icmp6_input`, `ndp_send_ra`, `ndp_send_ns` are all in the binary), and it
is *enabled*. Measured, not inferred, by `tests/ipv6/ipv6_link_test`:

```
interface 0 IPv6 addresses:
    addr fd00::280:10ff:fe32:3334      prefix /64  state 1 (TENTATIVE, DAD running)
    addr fe80::280:10ff:fe32:3334      prefix /10  state 4 (VALID)
  ok   ICMPv6 echo to ::1
  ok   ICMPv6 echo to our own link-local address
  ---> an IPv6 router advertised itself: YES     default router fe80::2
  ping6 default router: reply, 10 bytes
  ---> SLIRP answered fe80::2: YES
```

So the highest-value test route in the plan was available after all: **real ICMPv6
packets, EtherType 0x86DD, across an emulated Commodore A2065 through the SANA-II shim
to the host** — plus a router advertisement, stateless autoconfiguration of the global
`fd00::/64` address, duplicate address detection, and neighbour discovery resolving the
router's MAC. `ff02::2` (all-routers) and `fec0::2` are not answered; SLIRP's IPv6 router
lives at `fe80::2` and its prefix is `fd00::/64`, not libslirp's stock `fec0::`.

All three test routes in the plan were taken, and they prove different things:

| | `ipv6_test` (RAM driver) | `ipv6_socket_test` (`::1` via the LVOs) | `ipv6_link_test` (A2065 + SLIRP) |
|---|---|---|---|
| ICMPv6 echo | ✅ `::1` and peer link-local | — | ✅ `::1`, self, and the router |
| TCP over IPv6 | ✅ handshake + data both ways | ✅ `bind`/`listen`/`connect`/`accept` over `::1` | — |
| UDP over IPv6 | ✅ + `nxd_udp_source_extract` | ✅ `sendto`/`recvfrom` over `::1` | — |
| `sockaddr_in6` in and out | — | ✅ `getsockname`/`getpeername`/`accept`/`recvfrom` | — |
| `IPV6_V6ONLY`, `inet_ntop`/`pton`, `getaddrinfo` | — | ✅ | — |
| Duplicate address detection | ✅ between two real peers | — | ✅ against whatever is on the wire |
| Router advertisement / SLAAC | — (no router on a two-node wire) | — | ✅ |
| SANA-II `0x86DD` reader | — | — | ✅ |
| Goes through `bsdsocket.library`'s ABI | no | **yes**, linked against none of our code | no |
| Result, 68020 and 68030 | **78 checks, 0 failures** | **54 checks, 0 failures** | **6 checks, 0 failures** + findings |

```sh
cmake -S . -B build/v6 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
      -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_IPV6=ON
cmake --build build/v6 --parallel

AMINETXDUO_RUN_TAG=v6a ./tools/fsuae-run.sh -t 300 build/v6/tests/ipv6/ipv6_test
AMINETXDUO_RUN_TAG=v6b ./tests/ipv6/run-fsuae.sh        -b build/v6   # link/SLIRP
AMINETXDUO_RUN_TAG=v6c ./tests/ipv6/run-socket-fsuae.sh -b build/v6   # AF_INET6 LVOs
```

What is **not** proven: nothing has been run on real Amiga hardware with a real Ethernet
card, so the `0x86DD` `CMD_READ` path is verified against `a2065.device` under emulation
only. And no IPv6 traffic has crossed SLIRP's NAT to the outside world — SLIRP answers
for itself, but whether the host had global IPv6 to forward to was not tested.

#### The ABI, verified rather than assumed

The Roadshow NDK already defines the IPv6 socket ABI. Two things in it are traps, and
both are pinned with `_Static_assert` in `src/bsdsocket/in6.c`:

1. **`struct sockaddr_in6` has no `sin6_len`.** `netinet/in.h:170` gives `sockaddr_in`
   the 4.4BSD shape (`sin_len` at offset 0, `sin_family` at offset 1); `netinet/in.h:182`
   right below it is the **Linux** `sockaddr_in6`, pasted in verbatim, with `sin6_family`
   at offset **0** and a compiler pad at offset 1. The two are therefore *not*
   interchangeable through `struct sockaddr *`: reading `sa->sa_family` from a
   `sockaddr_in6` reads the pad. `bsd_sa_family()` decides the family from the bytes plus
   the caller's declared length instead of from a struct member, and nothing writes a
   length byte into a `sockaddr_in6` — on this NDK that byte *is* the family.
   Confirmed layout: 28 bytes, offsets 0 / 2 / 4 / 8 / 24, `sa_family_t` 1 byte,
   `in_port_t` 2.
2. **`gai_strerror()` takes its argument in `a0`, not `d0`** —
   `pragmas/bsdsocket_pragmas.h:141` says `gai_strerror(a0)` and the `libcall` form on
   line 264 agrees. Same class of surprise as `bpf_set_notify_mask` taking `(d1,d0)`.
   Caught because every prototype is generated from the pragma table by
   `tools/gen_vectors.py` rather than written by hand.

`AF_INET6` is 23 (`sys/socket.h:196`) and collides with `AF_IPX`, which nothing here uses.

**Not in the NDK, so chosen and documented here:** `IPPROTO_IPV6` (41),
`INET6_ADDRSTRLEN` (46), every `IPV6_*` socket option, `in6addr_any`, `IN6_IS_ADDR_*`,
`sockaddr_storage`, `PF_INET6`. `IPV6_V6ONLY` is 27 in the BSDs and 26 in Linux and the
NDK picks neither, so **both are accepted** — 26 is `IPV6_CHECKSUM` in BSD and 27 is
`IPV6_JOIN_ANYCAST` in Linux, both raw-socket/multicast options this library does not
offer, so the collision risk is nil. `IPV6_UNICAST_HOPS` (4 / 16) is treated the same way
and maps onto the same per-socket hop limit as `IP_TTL`.

**`IPV6_V6ONLY` defaults to OFF (dual-stack).** Not copied from anyone — it follows from
NetX Duo. Its port tables are family-agnostic: `nx_tcp_server_socket_listen()` registers a
listen against a *port*, and the SYN that arrives may be v4 or v6. There is no
arrangement under which an `AF_INET6` and an `AF_INET` socket both hold port 80 here, so
the usual argument for defaulting it *on* does not apply. Setting it to 1 is honoured:
`connect()`/`sendto()` refuse a v4-mapped destination, and `accept()` disconnects an IPv4
peer and returns `EWOULDBLOCK` — enforcement has to happen at accept time because by then
NetX Duo's TCP state machine has already completed the handshake.

#### `getaddrinfo(AF_UNSPEC)`: what it returns and why

`netdb.h:176` defines `AI_MASK` as only `AI_PASSIVE|AI_CANONNAME|AI_NUMERICHOST|
AI_NUMERICSERV` — there is **no `AI_V4MAPPED` and no `AI_ADDRCONFIG`**, so a caller
compiled against this header cannot express either hint. The behaviour is therefore fixed
and documented rather than selectable:

- **IPv6 results first, then IPv4.** A caller that walks the list and takes the first
  address that connects prefers IPv6 for free.
- **`AI_ADDRCONFIG` is implied and cannot be turned off.** AAAA is queried only when the
  stack has IPv6 running; A only when it has IPv4. Returning a family the machine cannot
  use has no honest purpose and the caller cannot ask for it.
- **`AI_V4MAPPED` is not implied and not available.** An `AF_INET6` query returns AAAA
  records only and never synthesises `::ffff:a.b.c.d` from an A record; a dotted quad with
  an `AF_INET6` hint is `EAI_ADDRFAMILY`. A caller that wants both asks for `AF_UNSPEC`.
- **At most one address per family.** NetX Duo's DNS client answers with a single address,
  not an RRset; presenting one as if it were the whole set would be a lie about
  round-robin DNS.

`getaddrinfo`/`getnameinfo`/`freeaddrinfo`/`gai_strerror` ship in **both** build
configurations (66 → 70 implemented vectors); only their `AF_INET6` answers depend on the
option. `getnameinfo()` has no reverse lookup for IPv6: it would be an `ip6.arpa` PTR
query, and a name that is wrong is worse than no name, so `NI_NAMEREQD` gets `EAI_NONAME`
and everyone else gets the numeric form.

#### Address configuration

Three modes, all of which configure the `fe80::/64` link-local address first — that one
needs no router, no server and no configuration file, and RFC 4291 requires it anyway.
Selected by a new `CONFIGURE6` keyword in `DEVS:NetInterfaces/<name>`, named by appending
`6` to the IPv4 keyword it mirrors (Roadshow has no keyword ending in a digit, so nothing
can collide):

```
DEVICE     = a2065.device
UNIT       = 0
CONFIGURE  = DHCP              ; IPv4, unchanged
CONFIGURE6 = AUTO              ; OFF | LINKLOCAL | AUTO | STATIC
ADDRESS6   = 2001:db8::10/64   ; STATIC only; /64 if the length is omitted
GATEWAY6   = fe80::1
```

**This settles the last open question in §9: the default is `AUTO`** — link-local always,
plus RFC 4862 stateless autoconfiguration from router advertisements. On a link with no
IPv6 router that is indistinguishable from `LINKLOCAL`: one router solicitation goes out
and nothing answers, so the cost of defaulting to it is three ICMPv6 packets. On a link
that has one, IPv6 works with nobody editing a file — which is what happened above.
`ADDRESS6` with no `CONFIGURE6` implies `STATIC`, exactly as `ADDRESS` implies a static
IPv4 interface. In the floor build the three keywords are recognised and ignored, so one
config file works in both builds without "unknown keyword" warnings.

`LINKLOCAL` and `STATIC` genuinely suppress autoconfiguration, which took
`NX_IPV6_STATELESS_AUTOCONFIG_CONTROL` in `nx_user.h`. Without it,
`nx_icmpv6_process_ra.c` forms a global address from any advertised prefix and consults no
flag, and `nxd_ipv6_stateless_address_autoconfig_{enable,disable}()` are stubs returning
`NX_NOT_SUPPORTED` — so both modes would have been lies, and the first `netstack_test` run
of the IPv6 build said so out loud (`stateless autoconfiguration failed (75)` = 0x4B =
`NX_NOT_SUPPORTED`). Cost: one `ULONG` per `NX_INTERFACE` and one comparison per prefix
option received.

**DHCPv6 is deliberately not used.** NetX Duo ships a client
(`addons/dhcp/nxd_dhcpv6_client.c`), but it is ~40 KB before its own IANA/IAID option
handling, needs its own thread and UDP socket, and answers a question SLAAC has already
answered on every network an Amiga is likely to be on. The floor target is a 68020 with
4 MB. If a stateful-only network turns up, the addon is there.

#### Code size

Measured with `m68k-amigaos-size` on `-m68020 -O2` Release builds, `.text` only:

| | before M8 | `IPV6=OFF` | `IPV6=ON` | IPv6 delta |
|---|---|---|---|---|
| `bsdsocket.library` | 190,208 | 193,636 | 237,252 | **+43,616** |
| `netstack_test` | 162,936 | 162,964 | 201,840 | +38,876 |
| `ram_driver_test` (no bsdsocket/netstack) | 80,244 | 80,244 | 95,968 | +15,724 |
| `soak_test`, `usergroup.library`, `ping`(v4) | — | byte-identical | — | 0 |
| `sizeof(NX_IP)` | 2,128 | 2,128 | 3,164 | +1,036 |

The **floor build is unchanged by IPv6**: +28 bytes in `netstack_test` for five extra
entries in the interface-keyword table, and nothing at all elsewhere. The +3,428 in
`bsdsocket.library` is the four new `getaddrinfo` vectors, which are a feature the floor
build gained, not IPv6 it does not use. (An earlier revision leaked 3.4 KB of IPv6 text
conversion into the floor build because `config_text.c` is one object and the linker pulls
it whole — caught by measuring rather than reasoning, and now `#ifdef`-guarded.)

The ~44 KB full-stack delta is larger than §5.4's ~30 KB estimate for the `nxd*`/
`nx_icmpv6`/`nx_ipv6`/`nx_nd` objects, and the difference is not those files: it is
`FEATURE_NX_IPV6` widening the *IPv4* objects (`nx_ip`, `nx_tcp*`, `nx_udp*`,
`nx_packet`, `nx_icmp` all grow dual-stack paths) plus this project's own
`src/bsdsocket/in6.c`, `src/netstack/netstack_ipv6.c` and the IPv6 text conversions.
The IPv6 tables in `NX_IP` are tuned down from NetX Duo's defaults in
`port/netxduo-amiga/inc/nx_user.h` (neighbour cache 16 → 8, destination table 8 → 4,
default routers 8 → 2, prefix list 8 → 4).

#### Gaps recorded honestly

- **`sin6_scope_id` is preserved but not honoured.** `bind()`/`connect()` record it and
  `getsockname()`/`getpeername()` report it back, but it does not select an outgoing
  interface: NetX Duo's dual-stack `connect` and `send` entry points take no interface
  parameter and pick the source themselves through `_nxd_ipv6_interface_find()`. With one
  Ethernet interface the two answers are identical. The `fe80::1%eth0` text form is not
  parsed at all.
- **`bind()` to a specific local address still only binds a port**, for IPv6 exactly as
  for IPv4 — NetX Duo has no `nx_*_socket_bind` that takes an address.
- **`DEVS:Internet/hosts` cannot hold an IPv6 address.** `src/config/netdb.c` parses an
  entry's address with `ami_config_parse_ip()`, which understands dotted quads only, so
  `netstack_resolve6()` goes straight to DNS. Fixing it is a netdb schema change that
  touches `get{host,net}by*` and belongs with that work.
- **IPv6 multicast membership is not exposed.** `IPV6_JOIN_GROUP`/`LEAVE_GROUP` return
  `ENOPROTOOPT` because `NX_ENABLE_IPV6_MULTICAST` is off in the floor-target tuning.
  Neighbour discovery does not need it — solicited-node joins go through
  `_nx_ipv6_multicast_join()` and reach the driver as `NX_LINK_MULTICAST_JOIN` either way.
- **A link-local address reports `prefix /64` as `/10`.** That is NetX Duo's own
  convention (`nxd_ipv6_address_set()` takes 10 to mean "derive `fe80::/64` from the
  MAC" and stores the 10), and it is harmless — `fe80::/10` and `fe80::/64` both make
  every other link-local address on-link.
- **`sana2: reader N did not stop` at shutdown is pre-existing**, not an IPv6 regression:
  the floor build logs it for readers 0 and 1, and the IPv6 build logs the same thing for
  the third reader as well.
- **The tools do not report IPv6.** `netstat`, `ping` and `ShowNetStatus` in `src/tools/`
  are IPv4-only and were out of this milestone's scope; they build and behave identically
  in both configurations.

#### A trap worth writing down, found in this milestone's own test code

`tests/ipv6/ipv6_socket_test.c` calls the LVOs through hand-written `asm` stubs, and the
first version listed `d1`/`a0`/`a1` only as *inputs*. On AmigaOS those are **scratch**:
the callee may destroy them and need not say so. GCC believed they survived and reused
the "still valid" copies, so `send()` returned 18 and `rc == sizeof(message)` compared
false on the next line — three checks failing with the correct number printed beside
them, which is about as misleading as a bug gets. The NDK's own `inline/bsdsocket.h`
solves it by declaring `register int _d1 __asm("d1")` (and `_a0`, `_a1`) as `"=r"`
outputs; adopting that idiom took the test from 46/49 to **54/54**. Nothing in the
library was wrong — but anyone writing a bare-metal LVO caller against this stack will
meet the same thing.

#### One latent bug found and fixed on the way

`AMINETXDUO_IPV6` was set as a **`PUBLIC` compile definition on `aminetxduo_sana2` only**.
`nx_user.h` turns it into `NX_DISABLE_IPV6`, which decides `FEATURE_NX_IPV6`, which
changes the layout of `NX_IP`, `NX_INTERFACE`, `NX_PACKET` and `NX_TCP_SOCKET` — so with
`IPV6=ON` the SANA-II shim saw a different `NX_IP` from the NetX Duo core it was driving.
It does not fail to link; it reads the wrong offsets. The definition is now global, in the
root `CMakeLists.txt`, with a comment saying why it must stay that way.

### Loose ends closed (2026-07-25)

Three tidy-ups that were left behind by the milestone work, alongside the `NX_RAND`
entropy pool documented under the M9 gate above.

- **The 64-bit division helpers are one copy again.** `__udivdi3` and friends had grown
  to three (see the toolchain landmine note under the M9 gate). `src/common/ami_udivdi3.c`
  is now the only one, built as `aminetxduo_m68k_rt`, keeping the 68020 `divu.l` fast path
  and gaining `__udivmoddi4`/`__divdi3`/`__moddi3` from the conformance copy — newlib's
  `printf` calls all five. `tests/conformance/build.sh` compiles that file out of
  `src/common/` rather than keeping its own; `src/crypto68k/` no longer builds a second
  copy of the TLS one. Verified by building the default tree, `-DAMINETXDUO_IPV6=ON`,
  `-DAMINETXDUO_TLS=ON` and the conformance suite.
- **`tools/smoke/` has CMake targets** (`smoke_probes`, or `smoke_<name>` individually).
  Six diagnostic probes that were hand-built and referenced only from shell history, so
  they were on their way to rotting. They build with everything else and are deliberately
  **not** registered with `ctest`: `crashtest` jumps to `0x2` and `gurutest` double-frees,
  both on purpose, and `KernelStop` re-execs itself and fills free memory with `ILLEGAL`.
  `KernelStop`'s output name is load-bearing — its parent process runs
  `SYS:KernelStop child`.
- **The netdb file parser now runs on the Amiga.** `tests/netstack/devs/Internet/` held
  only `hosts` and `name_resolution`, so every on-Amiga `get{serv,proto,net}by*` test was
  hitting `src/config/netdb.c`'s built-in fallback tables — the file parser had 157/157 on
  the host and had never executed on the target. Representative `services`, `protocols`
  and `networks` files in the standard `/etc` format are staged now. Proof that the files
  and not the fallbacks are in use, from a debug-logging run: **`services: 92 entries`,
  `protocols: 42`, `networks: 7`**, against built-in tables of 30, 7 and 1. Conformance
  `dns` stays 15/15. **No defect found** — the parser handled tab/space mixtures, trailing
  `#` comments, multi-alias rows and the truncated dotted network numbers (`10.0.2`,
  `169.254`) exactly as the host test said it would.

### Still open (lower stakes, decide during implementation)

- Does `usergroup.library` ship as a real user database or as the usual single-user stub
  (`root`/uid 0)? Most Amiga software only needs the calls to succeed.
- `bpf_*`: full BPF VM, or the common-case filter subset (`ether proto`, host/port
  matching) with a documented gap?
- ~~IPv6 default: built and off, or built and on when router advertisements appear?~~
  **Settled by M8**: built off (`AMINETXDUO_IPV6=OFF` is the shipping floor), and when
  built, on by default with `CONFIGURE6=AUTO` — link-local always, SLAAC when a router
  advertises.

---

## 10. Sources

Primary (local): `amigaos/tools/m68k-amigaos-gcc/m68k-amigaos/ndk-include/` (Roadshow NDK
headers, `pragmas/bsdsocket_pragmas.h` LVO table), `amigaos/reference/amiga-bootcamp/12_networking/`,
`amiga-os-src/os-source/other_networking/sana2/` (SANA-II spec + autodocs).

- [bsdsocket.library autodoc](https://wiki.amigaos.net/amiga/autodocs/bsdsocket.doc.txt)
- [SANA-II Network Device Driver Specification (AmigaMail Vol. 2)](http://amigadev.elowar.com/read/ADCD_2.1/AmigaMail_Vol2_guide/node01DE.html) · [SANA-II Revision 7](https://wiki.amigaos.net/wiki/SANA-II_Revision_7)
- [eclipse-threadx/netxduo](https://github.com/eclipse-threadx/netxduo) · [eclipse-threadx/threadx](https://github.com/eclipse-threadx/threadx) · [NetX Duo BSD support](https://github.com/eclipse-threadx/rtos-docs/blob/main/rtos-docs/netx-duo/netx-duo-bsd/chapter2.md) · [issue #182: NetX Duo with and without an OS](https://github.com/eclipse-threadx/netxduo/issues/182)
- [rondoval/lwip-amiga](https://github.com/rondoval/lwip-amiga) · [MW0MWZ/AmiTCP_NG](https://github.com/MW0MWZ/AmiTCP_NG) · [tbdye/bsdsocktest](https://github.com/tbdye/bsdsocktest) · [jbilander/catalyst](https://github.com/jbilander/catalyst) · [aros-development-team/AROS — AROSTCP](https://github.com/aros-development-team/AROS/tree/master/workbench/network/stacks/AROSTCP)
- [Using NetBSD's TCP/IP stack for AmigaOS (port-amiga, 2017)](http://mail-index.netbsd.org/port-amiga/2017/12/13/msg008038.html)
- [Which is the best TCP/IP stack on the Amiga? — maidavale.org](https://maidavale.org/blog/amiga-which-is-the-best-tcpip-stack/)
- [Roadshow](http://roadshow.apc-tcp.de/index-en.php) · [Aminet: AmiTCP-SDK-4.3](https://aminet.net/package/comm/tcp/AmiTCP-SDK-4.3) · [AmiTCP/IP FAQ](https://wiki.preterhuman.net/AmiTCP/IP_Frequently_Asked_Questions)
- [Aros/Developer/Docs/Libraries/BSDsocket](https://en.wikibooks.org/wiki/Aros/Developer/Docs/Libraries/BSDsocket)
- [WinUAE features (uaenet.device / SLIRP / A2065)](https://www.winuae.net/features/) · [EAB: SANA2 / SLIRP / A2065](https://eab.abime.net/showthread.php?p=969044)
