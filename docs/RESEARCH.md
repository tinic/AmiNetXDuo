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
- **`NX_RAND` was undefined**, so `nx_api.h` fell back to newlib `rand()` — a 32-bit LCG.
  Now replaced by a SHA-256 hash DRBG (`src/common/ami_random.c`). The seed is weak by any
  modern standard — a vintage machine has no hardware RNG, and the credited entropy is
  ~21 bits — but this stack exists so a classic Amiga can talk to modern sites, not to
  protect valuable secrets. It is a strict improvement on the LCG, it does not gate
  anything, and the threat model does not justify refusing to run.
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

#### Update: the fast crypto is now IN the handshake — 185.5 s → 26.7 s, and a real site answers

Both optimisation workstreams above measured their speedups **standalone**. Neither was
connected: `src/crypto68k/` was not referenced by `src/tls/CMakeLists.txt`, and `nm` on
`tls_handshake` found zero `c68k` symbols. The handshake they were meant to improve still
ran entirely on the vendored arithmetic. That is now fixed.

**The mechanism: our own `NX_CRYPTO_METHOD` entries, no vendored source touched.**
`nx_secure_tls_session_create()` takes an application-supplied `NX_SECURE_TLS_CRYPTO *`
and `nx_secure_tls_ecc_initialize()` takes an application-supplied curve-method array —
both are the vendor's own extension points, and every path that reaches a big-number
operation goes through one of them. `src/tls/ami_tls_crypto.c` supplies:

- `ami_crypto_method_rsa`, the vendored dispatch with `_nx_crypto_rsa_operation()`
  replaced by one that calls `c68k_crt_power_modulus()` /
  `c68k_huge_number_mont_power_modulus()`;
- `ami_crypto_method_ec_secp256`, a curve method that returns a **private, mutable copy**
  of `_nx_crypto_ec_secp256r1` with `nx_crypto_ec_multiple` swapped for
  `c68k_p256_ec_multiple`.

The copy matters. The vendored curve is `const` and is aliased by every ECDH and ECDSA
context in the process; casting the `const` away and writing to it is undefined, is
process-global, and would silently defeat the differential tests that check us *against*
the vendored path. Verified first: ECDH and ECDSA obtain their curve **only** by calling
`NX_CRYPTO_EC_CURVE_GET` on whichever curve method they were handed, and they store the
pointer rather than copying the struct — so one function pointer in a private copy is the
whole integration.

**RSA CRT, on the two paths that skip it.** `NX_CRYPTO_SET_PRIME_P` appears exactly once
in all of `nx_secure/src`. The ECDHE_RSA ServerKeyExchange signature
(`nx_secure_tls_ecc_generate_keys.c:773`) and `nx_secure_tls_send_certificate_verify.c:670`
both hand over the full 2048-bit private exponent with `p` and `q` NULL. We cannot add the
two missing calls without editing vendored sources, so the primes arrive from the other
end instead: `ami_tls_rsa_key_register()` records `(modulus, p, q)` from a parsed
certificate, and the RSA method looks the modulus up when asked for a private-key
exponentiation with no primes set. The pairing comes from one certificate object, so it
cannot be mismatched any more than the vendored CRT path's can.

**Measured, emulated 68020, `tests/tls/tls_decompose` — four rounds in ONE process with
identical instrumentation, so this is a measurement and not a composition:**

| round | connect → first record | client arithmetic | server arithmetic | RSA private op |
|---|---|---|---|---|
| reference, no CRT (**the M9 gate**) | **185.8 s** | 11.2 s | 173.6 s | 166.7 s |
| reference, CRT — *the CRT lever alone* | 64.4 s | 11.2 s | 52.5 s | 45.4 s |
| crypto68k, no CRT — *the module alone* | 77.8 s | **3.2 s** | 73.9 s | 72.1 s |
| **crypto68k + CRT — shipping** | **26.7 s** | **3.2 s** | 22.6 s | **20.8 s** |

Round 1 reproduces the M9 gate's independently measured 185.5 s to within 0.3 s, which is
the check that this is timing the same computation. Every predicted ratio landed: CRT
alone 3.67× on the private operation (predicted 3.6), crypto68k alone 2.31× (predicted
2.4), **both together 8.0× exactly as predicted**, and 6.9× on the whole loopback
handshake.

**The number that matters is the client one, and it is 3.2 s.** A client fetching a page
never performs the private-key operation; the 185 s figure was always dominated by the
server half, which one Amiga was also running. Per operation, client side:

| | reference | crypto68k | ratio |
|---|---|---|---|
| RSA-2048 verify ×2 | 4.1 s | 1.4 s | 2.9× |
| P-256 `k·P` ×2 (keygen + ECDH) | 7.0 s | 1.8 s | 3.9× |
| **client public-key arithmetic** | **11.2 s** | **3.2 s** | **3.5×** |

**A real public HTTPS server answers.** `tests/tls/tls_https` brings the whole stack up
through `netstack_startup()` over SLIRP, resolves `tls-v1-2.badssl.com` by DNS, connects,
sends SNI, verifies a chain it did not issue against ISRG Root X1 compiled into the test,
and gets `HTTP/1.1 301 Moved Permanently` back. **Handshake, connect to Finished: 6.8 s**,
`TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256`, TLS 1.2, 23 checks and 0 failures. That is a
server that has never heard of us, and it is the strongest interop evidence available.

Of that 6.8 s, **5.9 s is public-key arithmetic**: three RSA-2048-style verifications at
4.1 s and two P-256 `k·P` at 1.7 s, leaving 0.9 s for DNS, TCP, DER parsing, key
derivation and the record layer. The 4.1 s is worth explaining rather than averaging,
because it is not 3 × 0.68 s: **ISRG Root X1 is a 4096-bit key**, so verifying the
intermediate's signature is a 4096-bit modular exponentiation and costs ~4× a 2048-bit
one. Root key size is a real term in a client's handshake cost on this machine, and it is
not something the client gets to choose.

**Cross-implementation interop, both directions** (`tests/tls/tls_interop`, 96 checks, 0
failures). If both ends change together a mutual arithmetic error is invisible, so each
round changes one side only:

| | connect → first record |
|---|---|
| crypto68k **client** vs stock `nx_secure` **server** | 177.9 s (our client verifies signatures the vendored code produced) |
| stock `nx_secure` **client** vs crypto68k **server** | 34.6 s (the vendored code verifies signatures our CRT path produced) |
| stock on both sides (control) | 185.7 s |

Correctness, all still passing: `crypto68k_test` 4964/0, `crypto68k_ec_test` 1730/0 with
all ten invalid signatures still rejected, `tls_handshake` 44/0 on **both** 68020 and
68030 (the original 38 checks unchanged, plus six new ones: the comb-table self check, the
curve build, and per-round assertions that the server really took CRT and the client
really went through crypto68k). The default build is **byte-identical** — every artifact,
`bsdsocket.library` included, compared against a build of the tree without these changes.

Only the 68020 column is a timing. The same binary under FS-UAE's 68030 model reports
**0.8 s** for the whole loopback handshake, which is not a clock ratio; the 68030 model
does not charge for `MULU.L`. It is run as a correctness check and nothing else.

**Size, measured from the link map of `tls_handshake`** (68020, `-O3`):

| component | text | data | bss |
|---|---|---|---|
| `nx_crypto` | 147,644 | 2,020 | 980 |
| `nx_secure` | 52,720 | 872 | 4,752 |
| `crypto68k` | 22,916 | 0 | 1,284 |
| `aminetxduo_tls` (glue + tables) | 4,000 | 292 | 428 |
| **TLS total** | **227,280** | **3,184** | **7,444** |

`bsdsocket.library` is 249,892 bytes today, so a TLS-carrying build lands at **≈ 480 KB —
inside the 512 KB budget with ~32 KB of headroom**, and that is before any trimming (the
147 KB of `nx_crypto` still includes DES/3DES, MD5, the CCM and GCM modes and ECJPAKE).

Per connection, measured rather than estimated: crypto metadata **16,272 bytes** (against
10,128 with the vendored tables — the delta is exactly one 6 KB sliding-window scratch,
because the public-cipher slot is already sized by `NX_CRYPTO_ECDH` and only the
public-auth slot grows), plus `sizeof(NX_SECURE_TLS_SESSION)` = 1,700, plus an
application-chosen reassembly buffer (8 KB holds a two-certificate RSA-2048 chain, 12 KB a
public three-deep one), plus one 252-byte `NX_SECURE_X509_CERT` and a DER buffer per
certificate in the chain. **≈ 28 KB for a client connection.**

##### What still stands between this and "TLS on by default"

Two things, and neither is speed.

1. **No application can open a TLS connection.** `AMINETXDUO_TLS=ON` builds static
   libraries and tests. It links nothing into `bsdsocket.library` — proved, the library is
   byte-identical either way — and `dist/make-dist.sh` ships only `bsdsocket.library`,
   `usergroup.library` and the tools. Flipping the default today changes build time and
   nothing a user can observe.

   Of the three routes: an **AmiSSL-shaped library** is rejected — `nx_secure` has no
   `BIO`, no `SSL_CTX`, no `EVP`, certificates are caller-allocated fixed buffers and
   ciphersuites are a static table, so a faithful emulation is a rewrite of OpenSSL's API
   on a library without its concepts, and a *partial* one is worse than nothing because
   software links, opens, and then fails somewhere unpredictable. A **`bsdsocket.library`
   socket option or new LVO** is rejected as the primary route: `SOL_SOCKET` option numbers
   are not ours to allocate (AmiTCP, Roadshow and the tunnel implementations each have
   their own), it puts 227 KB and ~28 KB/connection inside the resident library for every
   program including the ones that will never use it, and — decisively — a TLS record
   boundary is not a byte-stream boundary, so `WaitSelect()` readability would stop
   meaning "`recv()` will return data".

   **Recommended: a small `tls_*` API in its own library**, opened only by programs that
   want it, plus **one private LVO** on `bsdsocket.library` handing out the
   `NX_TCP_SOCKET *` behind an fd. That last part is forced, not chosen: `nx_secure` binds
   a session to `NX_TCP_SOCKET *` (`nx_secure_tls_session_start()`, and
   `nx_secure_tls_tcp_socket` in the session struct) and has no transport abstraction. The
   public 121-LVO contract is untouched. It needs its own task — the I/O currency is
   `NX_PACKET *`, so a byte-oriented `TLSRead()` has to buffer partial records, and the
   library scaffolding, docs and installer integration are all new.

2. **There is no trust store, and that is the bigger problem.** `tls_https` works because
   ONE root CA is compiled into it. A usable client needs ~140 roots: where they live on
   disk, in what format, parsed lazily by issuer match (parsing all of them up front is not
   viable on 4 MB), how they are updated, and what "expired" means on a machine whose
   battery clock reports `tv_secs == 0` — the same machines the `NX_RAND` work already
   found. That is a design task of comparable size to this one, and until it exists an
   application can only reach servers whose root it was compiled with.

**Recommendation: leave `AMINETXDUO_TLS` OFF until those two land.** Speed no longer
argues against TLS and size no longer argues against it; the only thing default-on would
currently produce is 227 KB of code with no route to it.

#### Update (2026-07-25): both of those landed — `tls.library` and a real trust store

A program that is linked against **nothing of ours** now opens two shared libraries by
name, fetches `https://tls-v1-2.badssl.com/` over SLIRP, and verifies the chain against
**119 Mozilla roots on disk** rather than one compiled in. `tests/tls/tls_api` — 26
checks, 0 failures, 6.8 s handshake, `HTTP/1.1 301`, `TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256`.

##### 1. `tls.library`, and the one private LVO under it

The route taken is the one recommended above, and the reason it is a separate library
rather than a corner of `bsdsocket.library` is unchanged: 227 KB and ~40 KB per connection
should not be resident on a machine that never makes a TLS connection, and **a TLS record
boundary is not a byte-stream boundary**, so putting TLS behind `recv()` would break what
`WaitSelect()` readability means.

The API is eight vectors. `include/aminetxduo/tlslib.h` is the contract and carries the
example program; the shape is:

```c
LONG s   = socket(AF_INET, SOCK_STREAM, 0);   connect(s, ...);
struct TLSConnection *tls = TLSOpen(TLSBase, SocketBase, s,
                                    TLSA_HostName, (ULONG)"example.com",
                                    TLSA_Error,    (ULONG)&why);
TLSWrite(TLSBase, tls, request, len);
while ((n = TLSRead(TLSBase, tls, buf, sizeof buf)) > 0) { ... }
TLSClose(TLSBase, tls);        /* the descriptor is the caller's again */
```

Chain verification and host-name checking are **on by default**; `TLSA_NoVerify` has to be
asked for in those words. Worth recording: `nx_secure` verifies the chain but does **not**
check who the certificate is *for* — `_nx_secure_x509_common_name_dns_check()` exists and
nothing calls it unless the application installs a certificate callback. `tests/tls/tls_https`
never did, so until now nothing in this tree checked the host name at all.

**How a second library borrows a singleton.** NetX Duo and ThreadX have file-scope state
and there is exactly one copy, inside `bsdsocket.library`. `tls.library` therefore links
`nx_secure`, `nx_crypto` and `crypto68k` — which have no such state — and links **no NetX
Duo object at all**. The coupling surface was measured, not guessed: `nm` over
`libnx_secure.a + libnx_crypto.a + libcrypto68k.a + libaminetxduo_tls.a`, minus everything
they define themselves, leaves 25 externals, of which **twelve** are NetX Duo/ThreadX:

| | |
|---|---|
| packets | `_nx_packet_allocate`, `_nx_packet_data_append`, `_nx_packet_data_extract_offset`, `_nx_packet_release` |
| TCP | `_nx_tcp_socket_receive`, `_nx_tcp_socket_send` |
| ThreadX | `_tx_mutex_create/delete/get/put`, `_tx_thread_identify`, `_tx_thread_sleep` |

The rest are `memcpy`/`memset`/`memcmp`/`memmove`, `__udivdi3`, `SysBase`, and the four
`ami_random_*` entry points. `src/tlslib/tls_netx.c` **defines those twelve names** as
one-line forwarders through a table obtained from `bsdsocket.library`, so the linker
resolves `nx_secure`'s references to us and no vendored source is touched. The
`ami_random_*` forwarders matter for a second reason: they mean a TLS handshake draws from
the entropy pool `bsdsocket.library` already seeded, rather than starting a colder second
one.

The table arrives through **one private LVO at -0x360** — the first slot past the six
reserved ones `clib/bsdsocket_protos.h` documents after `getnameinfo()`, i.e. past every
offset any published bsdsocket ABI assigns. It takes a magic (`'ANXD'`) and a version and
writes nothing unless both match, so a program aiming at some future vendor's vector at the
same offset gets a clean failure instead of a pointer. The version carries
`AMINETXDUO_IPV6` in its low half, because that option changes the layout of `NX_IP`,
`NX_PACKET` and `NX_TCP_SOCKET` — all of which cross this interface — so a mismatched pair
of libraries is refused at `TLSOpen()` rather than reading each other's structs at the wrong
offsets. It exists **only in an
`AMINETXDUO_TLS` build**: `src/bsdsocket/nxcontext.c` is not compiled and the table slot is
not emitted otherwise, which is what keeps the default build byte-identical (verified —
`bsdsocket.library`, `usergroup.library` and all six commands compare identical against a
tree with the vector removed).

Three ThreadX **data** symbols (`_tx_thread_current_ptr`, `_tx_thread_system_state`,
`_tx_timer_thread`) cannot be forwarded through a table — a copy would be a copy, not an
alias. They are referenced only by `nx_secure`'s `nxe_*` argument-checking wrappers, and
`tls.library` calls the `_nx_secure_*` entry points directly, so those archive members are
never pulled in. One exception had to be handled by hand:
`ami_tls_local_certificate_add()` spells its call `nx_secure_tls_local_certificate_add`,
which the vendored header maps to the wrapper, so `tls_netx.c` supplies
`_nxe_secure_tls_local_certificate_add()` itself. `nm tls.library` shows zero undefined
symbols after the link.

##### 2. What `WaitSelect()` means for a TLS socket, and what was done about it

Two ways it lies, and they are not symmetric:

- **Not readable while `TLSRead()` would return immediately.** The library reads a whole
  record off the socket and hands out plaintext from it a piece at a time; the socket is
  drained and `WaitSelect()` sees nothing. A program that waits on the descriptor alone
  **hangs with its answer already in memory**. This is the dangerous one.
- **Readable while `TLSRead()` must block**, because what arrived is half a record.

`TLSWaitSelect()` takes the same arguments as `WaitSelect()` plus the list of TLS
connections involved. If any of them already holds plaintext it reports that descriptor
readable and **returns without waiting**; otherwise it delegates to `bsdsocket.library`'s
`WaitSelect()` through the caller's own `SocketBase`. `TLSPending()` is the same test on
its own for a caller who would rather write the loop.

The second lie is not removable without a non-blocking record layer and is bounded — the
rest of a record is already in flight — so it is documented and `TLSA_Timeout` caps it.
Reporting *fewer* ready descriptors than exist is a spurious-wakeup shape every `select()`
caller already tolerates; claiming a TLS socket is not readable is a hang. The test proves
the fix rather than asserting it: it reads **one byte**, then calls `TLSWaitSelect()` with a
**zero timeout** — a poll that plain `WaitSelect()` must answer 0 — and gets 1 with the
right descriptor set.

##### 3. The trust store: ~120 roots, 126 KB on disk, 1.4 KB resident

`DEVS:Internet/certificates`, in the Roadshow configuration drawer the rest of the stack
already uses. Indexed binary, big-endian (the machine's own order, so nothing is swapped):

```
 0   'A' 'C' 'S' '1'
 4   ULONG count          8  ULONG index_offset      12  ULONG data_offset
16   count x { ULONG key, ULONG offset, ULONG length }   sorted by key
...  the DER blobs, concatenated
```

**Only the index is ever resident** — 12 bytes per root, 1,428 bytes for the Mozilla set —
and the one root a chain actually needs is read from the file *during* the handshake.
Parsing all 119 eagerly was never viable: an `NX_SECURE_X509_CERT` is 252 bytes, so the
parsed set alone is 30 KB before the DER it points into, on a 4 MB machine.

**The key is the whole encoded Name, not the common name.** FNV-1a 32 over the
certificate's subject `Name` SEQUENCE including its tag and length bytes; `tls.library`
computes the same hash over a received certificate's **issuer** `Name`. That is exactly RFC
5280's rule, and neither side parses attributes, so the generator and the Amiga cannot
disagree about what a Name means. This is not fussiness: `nx_secure`'s own store lookup
compares distinguished names by **common name only** unless
`NX_SECURE_X509_STRICT_NAME_COMPARE` is defined, and in the current Mozilla set **four roots
share the common name "GlobalSign" and four more have no common name at all**. Handing
`nx_secure` a store with all four GlobalSign roots in it would let it take the first and
fail the signature check. Matching on the full Name means exactly one root is added, so the
name `nx_secure` then looks up is unambiguous by construction.

**Where the laziness is hooked.** `NX_SECURE_TLS_SESSION` carries a
`nx_secure_remote_certificate_verify` function pointer, set by
`nx_secure_tls_session_create_ext()` and never consulted before. `tls.library` replaces it:
its version asks every certificate the server sent "who issued you?", looks the answer up in
the index, and adds a root only on a hit — then calls the vendored verifier. A two-deep
public chain costs one index miss (the leaf's issuer is the intermediate), one hit, and one
2 KB read. Measured: `TLSInfo()` reports `128 roots on disk, 1 parsed for this chain`.

**Updates: replace the file.** There is no package manager and there is not going to be
one. `tools/mkcertstore.py` turns any PEM bundle into the file (no dependencies beyond the
standard library — the DER walk is sixty lines, which is the entire reason no `cryptography`
package is needed), so the story is `curl -o cacert.pem https://curl.se/ca/cacert.pem` and
re-run it, or copy a prebuilt `certificates` over the old one. **The index is read fresh at
every `TLSOpen()` and belongs to that connection**, so a replacement takes effect on the very
next connection — no reboot, no `avail flush`, and no cache to invalidate. Caching it in the
library base was the first design and was wrong twice: it needs reload detection, and it puts
a pointer one task can free (a second `TLSOpen()` with a different `TLSA_TrustStore`) under a
pointer another task is reading from inside a handshake, on a machine with no memory
protection. A per-connection index costs 1,428 bytes against the ~40 KB the connection
already needs, and one 1.4 KB read against a handshake that spends seconds on arithmetic.

The bundle is deliberately *not* vendored: it changes every few weeks and a stale copy in git would be
worse than none. CMake and `tests/tls/run-api.sh` find the host's (`/etc/ssl/cert.pem` and
friends) or take `AMINETXDUO_CA_BUNDLE`.

##### 4. The clock, on a machine that does not have one

`tls_https` observed `tv_secs == 0`. An Amiga with a dead RTC battery reports 1 January
1978, which is before every certificate on the internet was issued, so a library that checks
validity dates unconditionally **cannot reach a single HTTPS site** from such a machine.

**Decision: skip the validity dates when the clock is obviously unset, check them when it is
not, and report which happened.** "Obviously unset" is anything outside a fifty-year window
starting at 2026-01-01 — the floor catches 1978 and every partially-set clock, and the
ceiling catches the machine whose date was typed in wrong and now reads 2145, which would
otherwise reject every valid certificate as expired and look identical to a real failure.
The floor is a constant, not `__DATE__`: a build-date check would make the binary
non-reproducible and make an old build behave differently from a new one on the same
machine.

No vendored change was needed. `nx_secure_x509_certificate_chain_verify.c` already reads
`if (current_time != 0)`, so returning 0 from the session's time function **is** its own
"do not check" encoding.

What this gives up, precisely: expiry does not stop impersonation — the signature chain to a
trusted root and the host-name check do that, and **both still run**. What expiry adds is a
bound on how long a certificate whose private key has leaked stays useful. An attacker who
has stolen a key *and* can get between this Amiga and the site can use it indefinitely
against a clockless machine. That is a real weakening; it is also the same weakening every
device with a dead RTC has, and this stack has no revocation checking of any kind, so the
stolen-key case was never covered. The alternative on offer is a machine that reaches
nothing. `TLSInfo()`'s `ti_ExpiryChecked` is FALSE when it happened, so a program that cares
can say so.

**Demonstrated, not asserted.** FS-UAE hands the guest the host's clock, so the branch would
never run by accident. `tests/tls/tls_api` sets the emulated machine's clock to the AmigaOS
epoch through `timer.device`'s `TR_SETSYSTIME`, fetches the same page again, and puts the
clock back: *"a machine with no clock still reaches the site"*, *"TLSInfo() reports the dates
were NOT checked"*, *"while the chain and the host name still were"*.

##### 5. Size, and the answer on the default

| | bytes |
|---|---|
| `bsdsocket.library`, default build | 249,636 |
| `bsdsocket.library`, `AMINETXDUO_TLS=ON` | 250,084 (**+448**, the private vector) |
| `tls.library` | 273,080 |
| **the pair** | **523,164 = 510.9 KiB** |
| `DEVS:Internet/certificates` | 128,928 (119 Mozilla roots) / 142,693 (128 Apple roots) — on disk only |

**1,124 bytes inside the 512 KiB budget.** That is not headroom, it is a coincidence, and it
should be said plainly rather than rounded off. The estimate this replaces (≈480 KB) counted
227 KB of *text* from a link map; a hunk file on disk also carries data, bss headers and
relocations, which is ~31 KB on `tls.library` alone. The trimming lever is untouched and
large: `src/tls/CMakeLists.txt` globs **all** of `crypto_libraries/src`, so `nx_crypto`
still contains DES, 3DES, MD5, CCM, GCM and ECJPAKE, none of which any shipping ciphersuite
reaches. Anyone who needs room should start there.

Per connection, allocated at `TLSOpen()` and freed at `TLSClose()`: crypto metadata sized by
`_nx_secure_tls_metadata_size_calculate()` rather than guessed (16 KB), a 10 KB record
buffer, four remote-certificate slots and two root slots at 2.5 KB of DER each — **about
40 KB**, and none of it resident when no connection is open.

**Recommendation: not yet, and for one reason that is not technical.** Both stated blockers
are gone and the pieces work. What is missing is a *traveller*: no command in the
distribution opens `tls.library`, so default-on would ship 273 KB that only third-party
software could use, and no third-party software exists because the library has never
shipped. Two things would settle it, and both are small:

1. **A shipped command that uses it** — a `fetch`/`urlget` alongside `ping` and `host`.
   `tests/tls/tls_api.c` is already the worked example; it needs a URL parser and an
   argument line.
2. **A deterministic trust store at release time.** Today `dist/make-dist.sh` packs whatever
   `tools/mkcertstore.py` could build from the host's CA bundle, and warns if there was
   none. A release built on a machine without one would ship a `tls.library` that refuses
   every connection with `TLS_ERR_TRUSTSTORE`. Pin a bundle hash and fetch it in the release
   job, or vendor a dated snapshot.

Neither is a research question. Until they land, `AMINETXDUO_TLS=ON` is a supported build
that is worth running — `tests/tls/run-api.sh` is the proof it works end to end — and the
default stays OFF.

`dist/make-dist.sh` and the Installer script already handle both files when the build has
them, and one trap surfaced doing it, worth recording because its symptom points at the
wrong thing entirely. **The Installer takes its window down while it copies**, and
`install/test/installdrive.c` treated six consecutive windowless polls as "the run has
finished". Adding `tls.library` (273 KB) and the certificate store (140 KB) to the archive
pushed that copy past six seconds, so the driver stopped clicking, `S:User-Startup` was never
written, and the harness reported *"the install run did not complete cleanly"* with every
file up to the copy correctly installed — which reads exactly like an Installer script that
aborted mid-way, and is not one. `GONE_LIMIT` is now 20; `MAX_POLLS` still bounds the run,
and a genuinely stuck Installer keeps its window up, so it fails on the cap instead.

#### Update (2026-07-25, later): the traveller and a reproducible store landed — the default still does not

The two things the section above said stood between this and "TLS on by default" are
both done. A third thing turned up while proving the first one works, and it is worse
than either of them: **a certificate chain of three or more takes the machine down.**

**→ Wrong, and corrected below.** Nothing took the machine down. The emulator was being
killed by SIGPIPE on the host, and everything in this section that reads as a library
defect is that. See "The three-certificate 'crash' was the emulator dying of SIGPIPE".

##### 1. `fetch`, the traveller

`src/tools/fetch.c`, one more Roadshow-shaped Shell command beside `ping` and `host`:

```
fetch URL/A,TO/K,HEADERS/S,QUIET/S,NOVERIFY/S,TIMEOUT/N/K
```

It resolves, connects, and — if the URL says `https:` — opens `LIBS:tls.library` and
runs the transfer through `TLSRead()`/`TLSWrite()`. **One binary serves both build
options**: nothing in it is conditional on `AMINETXDUO_TLS`, because the decision is an
`OpenLibrary()` at run time, so a default build ships a `fetch` that does `http:` and
says something legible when asked for `https:`. That is also why it is not in
`src/tlslib/`: it is a network command that happens to know about TLS, not a TLS demo.

Deliberately small: HTTP/1.0 with `Connection: close`, so the body is "everything until
the far end hangs up" and there is no chunked framing to get wrong. Up to five
redirects, and it **refuses one that steps down from `https:` to `http:`** rather than
quietly dropping the encryption that was asked for. Without `TO` the body goes to
standard output and nothing else does, so `fetch URL >file` is not corrupted by a
progress line; with `TO` there is a free channel and the summary is worth having.

Verified by running it, not by asserting it — `tests/tls/run-fetch.sh`, which stages the
two libraries, the trust store and an A2065 on SLIRP and drives the command through
`ToolsSmoke`'s staged command list (the harness starts one executable with no
arguments, and `fetch` takes arguments):

| | |
|---|---|
| `fetch ?` | prints the template, then the usage line, `rc 10` |
| `http://example.com/` | 559 bytes of HTML on stdout, `rc 0` |
| `http://example.com/ TO DH0:plain.txt` | `HTTP/1.1 200 OK`, `559 bytes -> DH0:plain.txt` |
| `https://tls-v1-2.badssl.com/ TO …` | 0xC027, chain verified, **6.8 s**; follows the 301 to `…:1012/` — a second handshake at 0x3D, 5.0 s — then 200 OK, 502 bytes |
| `https://ecc256.badssl.com/ TO …` | 0xC023 (ECDHE_ECDSA), chain verified, **23.3 s**, 200 OK, 684 bytes |
| `https://wrong.host.badssl.com/` | `fetch: wrong.host.badssl.com: the certificate is issued to another host`, `rc 10` |
| `ftp://example.com/` | `"ftp://example.com/" is not an http: or https: URL` |
| `http://example.invalid/` | `cannot resolve "example.invalid"` plus `tool_explain_resolve()`'s block |

That redirect line is the one worth keeping: two full handshakes, an absolute `Location`
on a **non-default port**, and the URL parser's `:1012` all in one command.

**A finding that belongs to anyone writing an Amiga TLS program, not just to this one.**
The first `https:` run took the machine down, and the reason was not the reason it
looked like. A command started by the Kickstart 3.1 Shell gets **4,096 bytes** of stack
(`tc_SPUpper - tc_SPLower`, measured on the machine), of which **2,736 were still free**
by the time `TLSOpen()` was reached. And `tls.library` brackets its caller into ThreadX
to reach the stack, which — `port/threadx-amiga/src/tx_amiga_adopt.c` — hands
`_tx_thread_create()` **the caller's own stack region** as the ThreadX thread stack. So
those 2,736 bytes are not the command's: NetX Duo, `nx_secure` and the bignum code all
run on them. `fetch` therefore allocates 64 KB and runs the transfer on it through
`StackSwap()` (exec V36, so it is on the 3.1 floor), rather than expecting a user to
type `stack 65536` first.

`StackSwap()` has one trap and it is worth writing down: **the function that calls it
must not touch a stack-based local of its own between the two calls**, because between
them the stack pointer belongs to the other stack. `fetch_trampoline()` therefore has no
locals, no arguments, only file-static state, and is `noinline` — GCC inlined the first
version straight into `main()`, which is exactly the hazard. The generated code was read
rather than hoped at: one `move.l a6,-(sp)` before the first swap and its matching
`move.l (sp)+,a6` after the second, which balances because `StackSwap()` restores the
pointer exactly.

(That was not, in the end, what was crashing — see §3 — but 2.7 KB for a handshake is
not something to ship either way.)

##### 2. A trust store that is the same bytes on every machine

`src/tlslib/CMakeLists.txt` used to scavenge the *host's* CA bundle
(`/etc/ssl/cert.pem` and friends) and `dist/make-dist.sh` warned if it found none. Two
failure modes, and the release engineer sees neither: a release carries whatever roots
that laptop happened to have, and a build on a machine with none ships a `tls.library`
that refuses every connection with `TLS_ERR_TRUSTSTORE`. On the machine this was
developed on the scavenged bundle was **Apple's 128 roots, 142,693 bytes** — which is a
perfectly good root set and is not the one anyone thought they were shipping.

**Now: a vendored, dated, hash-pinned snapshot.** `third_party/cacert/cacert.pem`, a
verbatim copy of `https://curl.se/ca/cacert.pem` ("Certificate data from Mozilla as of:
Thu Jul 16 03:12:01 2026 GMT", 119 roots), with curl.se's own published checksum file
beside it. The alternatives were considered rather than skipped: fetching a pinned URL
at release time makes the build depend on a third party's web server being up, which is
the opposite of reproducible; vendoring the generated binary store puts 126 KB of
unreviewable data in git and hides the input the licence attaches to.

**Two hashes are pinned and both failures are fatal.**

* the input, against `cacert.pem.sha256`, so a corrupted or swapped snapshot cannot be
  built from;
* **the generated store**, `certificates.sha256`, checked by `tools/mkcertstore.py`
  *before it writes the file*. That second pin is what turns "any host produces the same
  bytes" into a checked claim rather than an intention: if a host CA file, a different
  bundle or an edited generator leaked into the result, the digest moves and the build
  stops with both hashes printed.

`mkcertstore.py` also grew `--min-roots` and now treats an oversized or unparseable
certificate as a **hard error** instead of a warning — a store missing roots produces a
machine that reaches most sites and mysteriously refuses the rest, and finding that out
on the Amiga is much worse than finding it out in CI.

Proved rather than asserted, four ways:

| | |
|---|---|
| two build trees, same machine | byte-identical, `35a1d1c9…` |
| **a different host** — playhouse2, Linux 6.17 x86-64, Python 3.13.5, whose own `/etc/ssl/certs/ca-certificates.crt` is a *different* 224,449-byte bundle | byte-identical, `35a1d1c9…` |
| the vendored snapshot removed (a host with no CA bundle at all) | configure **fails**: "The vendored CA bundle is missing"; no store written |
| one byte appended to the snapshot | configure **fails**, printing expected and actual |
| `-DAMINETXDUO_CA_BUNDLE=/etc/ssl/cert.pem` | builds, is labelled `(NOT the pinned snapshot)`, keeps only the `--min-roots` floor — and `dist/make-dist.sh` then **refuses to pack it**, exit 2 |

`dist/make-dist.sh`'s warning is now a hard failure in both directions: no store beside a
packed `tls.library` is exit 2, and a store whose digest is not the pin is exit 2.

**The licence, since MPL 2.0 in an MIT tree deserves an answer.** MPL 2.0 is file-scoped
copyleft: §1.10 defines a Modification as a change to a Covered File's contents, and §3.3
permits distributing a Larger Work under other terms provided the covered files keep the
MPL and their notices. MIT source beside an MPL data file is the case §3.3 is written
for, and it is what `certifi` (MPL 2.0) inside MIT/BSD Python applications and
`webpki-roots` (MPL 2.0) inside Rust applications have done for a decade. We do not
modify the file, so there is nothing to relicense; we treat the *generated*
`DEVS:Internet/certificates` as covered too, because a re-encoding of the same
certificate set is the conservative reading of "Modification" and a change of container
should not be argued to launder a licence. §3.2's "tell recipients how to get the Source
Code Form" is discharged in `dist/ReadMe`, which names the file, its upstream URL and
`tools/mkcertstore.py`. Nothing else in the project is affected. The full argument,
including why there is no permissively-licensed root bundle to use instead, is in
`third_party/cacert/README.md`.

##### 3. What replaced them: three certificates take the machine down

The first `https://example.com/` fetch killed the emulator. So did the second, and the
tenth. The bisect, all on the A1200 profile at 14 MHz unless stated:

| host | chain | ciphersuite | result |
|---|---|---|---|
| `tls-v1-2.badssl.com` | 2, RSA | 0xC027 | **OK**, 6.8 s |
| `ecc256.badssl.com` | 2, ECDSA | 0xC023 | **OK**, 23.3 s |
| `www.iana.org` | 3, ECDSA | 0xC023 | **crash** |
| `www.iana.org`, `NOVERIFY` | 3 | 0xC023 | **OK**, 4.5 s, 6,253 bytes |
| `www.iana.org`, **`-k 28`** (28 MHz, same cycle-exact profile) | 3 | 0xC023 | **OK** |
| `www.iana.org`, 68030 under Enforcer (no cycle accounting) | 3 | — | **OK** |
| `example.com` | 4, ECDSA | — | **crash** |
| `example.com`, `-k 28` | 4 | — | **crash** |

Four things that pin it down:

1. **It is not `fetch`.** `tests/tls/tls_api` — linked against nothing of ours, run
   directly from the Startup-Sequence with its own stack — dies at the same place when
   its `A_HOST` is changed to `www.iana.org`. One line, reproducible by anyone.
2. **It is not the stack.** It survives the 64 KB `StackSwap()` unchanged, and it kills a
   program that never swapped.
3. **It is time, not structure.** The *same* host, the *same* binary, the *same* chain:
   dead at 14 MHz, alive at 28 MHz. Four certificates are still dead at 28 MHz, which is
   what you would expect if the threshold is wall-clock and each extra certificate costs
   one public-key verification.
4. **It is verification that costs the time.** `NOVERIFY` on the same host is 4.5 s and
   fine; verifying is three more signature checks and tens of seconds.

Where it dies, from a serial trace through `_nx_secure_tls_client_handshake` (removed
again afterwards; `third_party/` is clean):

```
session_start begin
msg type 2 len 87            ServerHello
msg type 11 len 2464         Certificate, three of them
  verify hook: cert 1 issuer 6A6A82DB -> miss
               cert 2 issuer F95413DF -> 525 bytes read from the store, parsed, trusted
               cert 3 issuer 7560C1C5 -> miss
  vendored chain verify -> 0
  host-name check -> 0
msg type 12 len 144          ServerKeyExchange, ECDSA signature verified
msg type 14 len 0            ServerHelloDone
premaster -> 0 ; client key exchange -> 0 ; record sent -> 0 ; generate_keys -> 0
mutex put -> packet allocate -> 0 -> mutex get
ChangeCipherSpec being built ...        <- and never anything again
```

So the handshake gets to within two records of finishing and stops. Under Enforcer on a
68030 the same run **completes** and reports exactly two hits, both
`LONG-READ from 00000000` at `PC 00290E46` inside `SYS:fetch` — a null dereference that
is survivable on a machine with real memory at address 0 and is not, on its own, the
crash. (`tools/enforcer-run.sh` gained a `-n` so this could be run at all; it had no way
to attach the A2065.)

The most likely story, unproven: the far end gives up on a handshake that has taken half
a minute, and the library walks into freed or reused state instead of returning
`TLS_ERR_IO`. Both failing hosts are Cloudflare-fronted; both passing ones are badssl's
nginx. Whatever the mechanism, the shape of it is the part that matters: **a peer can
crash this machine by being slow to be verified**, and there is no application-side
defence — `fetch` cannot decline to be verified against a three-deep chain.

**→ Answered, and the answer is no.** The peer did send a FIN, and the library handled it
exactly as it should — `TLS_ERR_CLOSED`, "the connection is closed", return code 10. What
died was `fs-uae` itself, on SIGPIPE, when SLIRP wrote the guest's ChangeCipherSpec to a
host socket the far end had already closed. The serial trace above stops where it does
because the emulator stops there, not because the record layer does. See "The
three-certificate 'crash' was the emulator dying of SIGPIPE" below; the two Enforcer hits
in `SYS:fetch` are real and still worth chasing, but they are not this.

##### 4. The answer on the default, and what it would take to change it

**Still OFF, and now for a reason that is a bug rather than a gap.**

**→ Still off, but not for that reason: there was no bug.** The paragraphs below argue
from a crash that does not exist. The real limit is that a 14 MHz 68020 takes longer over
a three-certificate chain than a busy front end will wait, which is a slowness gap and
not a hole anyone can shoot through. Re-argued at the end of the section named above.

The three arguments that used to be made against it are all gone. Speed: a public
handshake is 6.8 s. Size: the pair is 523,164 bytes, unchanged (`bsdsocket.library`
250,084 with the private vector, `tls.library` 273,080), which is still 1,124 bytes
inside 512 KiB. A traveller: `fetch` ships. A trust store: 119 roots, 128,928 bytes,
byte-reproducible on any host.

What is left is that turning it on would put a `tls.library` in `LIBS:` that any program
can open and that a large share of the public web — everything behind Cloudflare and
Google Trust Services — can use to take the machine down. That is not a default. It is
also not a reason to hide the work: `-DAMINETXDUO_TLS=ON` remains a supported,
CI-covered configuration, `fetch` works over `https:` to two-deep chains today, and the
CI matrix still builds all four configurations with the TLS one among them.

**The one thing to fix, in order:** find why the record layer walks off after a long
verification. Start from the trace above with `-k 14` and a packet capture, and the first
question to answer is whether the peer sent a FIN or an RST while the 68020 was still
doing arithmetic — because if it did, the fix is a mid-handshake disconnect being
handled, and every other TLS user of this stack needs it too. After that, the remaining
work is arithmetic, and arithmetic is the one thing on this machine that has always been
possible to make faster.

The default-build artefacts are **byte-identical** to the pre-change build — every one of
`bsdsocket.library`, `usergroup.library` and the eight existing commands compares equal
against a build of the tree before this work — so the whole of the change to the shipping
floor is one new 45,632-byte command. Baselines re-run on it: conformance **125/142**
(loopback, 1 failed, 16 skipped), soak 98/0, libraries 8/0, ram_driver 32/0,
`tls_handshake` 44/0 on the TLS build, `tools/ci.sh host cross conformance` all green
across all four configurations.

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

### Data-path performance (2026-07-25): 261 KB/s → 356 KB/s, and the checksum was the reason

TCP throughput measured **261 KB/s loopback / 312 KB/s to a host over SLIRP** and nobody
had profiled the stack. It now measures **356 KB/s loopback / 368 KB/s over SLIRP** —
**+36%** and **+18%** — from two changes in `src/net68k/` plus one in `src/sana2/`. There
is no profiler on this platform, so the method is the one §5.4 and the crypto68k work
use: measure each primitive, count how often the data path runs it, multiply, and check
the model against an end-to-end run.

| | before | after | |
|---|---:|---:|---:|
| TCP loopback, 1 MB sustained | 261 KB/s | **356 KB/s** | +36.4% |
| TCP to a host over SLIRP, 1 MB sustained | 312 KB/s | **368 KB/s** | +17.9% |

Both rows are the conformance suite's throughput category against two
`bsdsocket.library` builds from one tree, differing only in
`AMINETXDUO_NET68K_CHECKSUM` and `AMINETXDUO_NET68K_MEMCPY`. The SANA-II change of §3
below is present in *both* arms, so the SLIRP row is attributable to the other two
changes alone.

The wire gains less than loopback because it gains less *per byte*: a loopback megabyte
is checksummed twice and copied three times, while over the wire there is one fewer
full-payload copy and 180 MSS-sized segments per megabyte instead of 128 8 KB ones, so
the per-packet costs the changes do not touch weigh more heavily.

**Only the 68020 column is meaningful.** Every figure below is FS-UAE's A1200 model
(68020, Kickstart 3.1 40.68), timed with `ReadEClock()` and the bracket's own cost
(29 ticks, 41 µs) calibrated over 256 pairs and subtracted. The 68030 model is used as a
correctness check and nothing else, for the same reason the P-256 work records — and
this harness makes the point unusually plainly, because it measures a copy loop rather
than arithmetic:

| same binary, same input | 68020 model | 68030 model |
|---|---:|---:|
| `memcpy`, 1460 B aligned | 184.3 ns/B | 1.93 ns/B |
| checksum, vendored | 629.2 ns/B | 16.4 ns/B |
| TCP loopback, end to end | 512 KB/s | 16,000 KB/s |

95× on a memory-to-memory copy is not a clock ratio; it is a model that does not charge
for the bus. The 68030 runs are quoted here **only** as evidence that they must not be
quoted anywhere else. *(Since resolved into a mechanism rather than a suspicion: FS-UAE
switches cycle accounting off for every CPU above a 68020, and there is now a probe that
measures it — see "Machine profiles, and calibrating the emulator before believing it"
below.)*

The harness is `tests/perf/perf_test.c`, run with
`AMINETXDUO_RUN_TAG=perf tools/fsuae-run.sh -t 900 build/cm/tests/perf/perf_test`.

#### Where a megabyte of TCP goes

Loopback, through `bsdsocket.library`, 8 KB application writes — the shape the
conformance suite's throughput category measures. Cost centres are per-primitive
measurements multiplied by the invocation counts the harness reports; the subtotals are
independently measured, and they agree.

| cost centre | ms/MB before | ms/MB after | share before |
|---|---:|---:|---:|
| TCP checksum, transmit | 651 | 199 | 16.6% |
| TCP checksum, receive | 651 | 199 | 16.6% |
| `nx_packet_data_append` — the `send()` copy, plus the chain it allocates | 295 | 254 | 7.5% |
| `nx_packet_copy` — the loopback hand-over | 321 | 280 | 8.2% |
| `nx_packet_data_extract_offset` — the `recv()` copy | 234 | 193 | 6.0% |
| **data path, subtotal** | **2152** | **1125** | **54.9%** |
| NetX Duo protocol + ThreadX scheduling | 758 | 750 | 19.3% |
| `bsdsocket.library` — per-call adopt/orphan, `WaitSelect`, descriptor lookup | 1007 | 1011 | 25.7% |
| **total** | **3922** | **2869** | |

Provenance, because a table like this is only worth having if it says which numbers were
measured and which were derived. Every per-byte figure in the "before" column is a
measured primitive multiplied by the invocation count the harness reports; the data-path
subtotal is *also* measured directly and independently, by a benchmark that runs exactly
those operations in order with no protocol — 2057 ns/B, i.e. 2157 ms/MB, against the
2152 the parts add up to (0.2%). In the "after" column the two checksum rows and their
subtotal are likewise measured directly (1190 ns/B, 1248 ms/MB); the three copy rows are
the same measurement minus the 39 ns/B the `movem.l` copy saves, and the end-to-end
figure is the check on that: parts 2886 ms against 2869 measured, 0.6% out.

The three layers were separated by measuring the same transfer three ways in the same
session: a pipeline benchmark that performs exactly the operations a loopback segment
pays for with no protocol at all (474 → 820 KB/s), the same transfer through raw NetX Duo
sockets (351 → 512 KB/s), and the conformance suite through the library (261 → 356 KB/s).
The differences are stable across the checksum change — the protocol layer costs
~755 ms/MB and the library ~1010 ms/MB whatever the copies underneath are doing — which
is what makes the subtraction trustworthy.

**The library layer is a quarter of a megabyte's cost and is the biggest thing left.**
It is not touched here.

#### 1. The IP checksum: 3.11× (the whole of the loopback win, and then some)

`third_party/netxduo/common/src/nx_ip_checksum_compute.c` reads a longword and splits it
into two 16-bit halves by hand:

```c
checksum += (*long_ptr & NX_LOWER_16_MASK);
checksum += (*long_ptr >> NX_SHIFT_BY_16);
```

GCC 15.2 `-O2 -m68020` compiles that to **seven instructions per longword** —
`move.l (a1)+,d1 / move.l d1,d0 / andi.l #65535,d0 / clr.w d1 / swap d1 / add.l d0,d1 /
adda.l d1,a0` — plus the `dbf`. It is the price of expressing a carry in a language that
has none.

`src/net68k/n68k_checksum.S` does it in **two**: `add.l (a0)+,d0` then `addx.l d2,d0`
with `d2` zero, unrolled eight times. Measured on a 1460-byte payload:

| | ns/byte | cycles/byte @14.19 MHz |
|---|---:|---:|
| vendored | 626.3 | 8.9 |
| net68k | 200.7 | 2.8 |
| **ratio** | **3.11×** | |

Two other C spellings were compiled and disassembled first, because the RSA and P-256
work both found the compiler had already done the obvious thing:

- a 64-bit accumulator gives five instructions and a `dbf`, and **rebuilds the zero
  register on every iteration** (`suba.l a0,a0 / move.l a0,d1`) rather than hoisting it —
  the same failure the P-256 notes record;
- an explicit carry test (`acc += w; if (acc < w) acc++;`) gives four and a branch. That
  one is the portable fallback in `n68k_checksum.c`, so the C path is within ~2× and the
  assembly is worth about that much and no more.

**The vendored checksum was the most expensive per-byte operation in the stack — three
times the cost of copying the same bytes.** A loopback byte is checksummed twice
(transmit and receive), so at 626 ns/B the two passes alone were a third of the entire
per-byte budget.

Predicted saving from the micro-benchmark: 2 × (621 − 190) ns/B × 1 MB = **908 ms**.
Measured on the conformance suite's 1 MB sustained loopback transfer: 3922 ms → 3009 ms =
**913 ms**, i.e. 261 → **340 KB/s**. The cost model is right to within 0.5%.

*Correctness.* A faster checksum that is wrong corrupts silently — every packet still goes
out and the peer quietly drops some. So `n68k_ip_checksum_compute()` is not "a correct
internet checksum", it is *exactly what NetX Duo returns*: the pseudo-header arithmetic,
the chain walk, the end-pointer rounding, the two-byte carry across a packet boundary
whose append pointer is 2 mod 4, and the trailing 1/2/3-byte case including its zero-write
into the pad byte are all structurally identical. Only the inner loop changed.
`tests/perf/host/` compiles the vendored function under a renamed symbol
(`-D_nx_ip_checksum_compute=n68k_checksum_reference`, no vendored file edited) and checks
the two differentially: **10,030 comparisons, 0 failures**, over every length 0–96 at
every prepend alignment 0–7, lengths to 8 KB, chains of 2–5 packets with append pointers
on all four residues mod 4, `data_length` shorter than the packet and longer, and
all-zero / all-ones payloads — the inputs where a one's-complement sum can disagree with a
16-bit accumulation over 0x0000 against 0xFFFF. That tier runs under `ctest` on every
push. The assembly cannot be assembled on a host, so the on-Amiga harness repeats the
comparison for the 1460-byte, chained and 0–40-byte cases.

#### 2. `memcpy`: no cliff, but 23% was available anyway

The misaligned-`memcpy` hypothesis is **wrong for this toolchain, and the measurement
that kills it is worth keeping**.

`-m68020` selects the `libm020` multilib — verified in the link map, not assumed — and
that `memcpy` is not the one in the base `libc.a`. The base (68000) version does require
length ≥ 8 **and both pointers 4-byte aligned**, falling back to `moveb a1@+,a0@+`
otherwise. The `libm020` version aligns **only the destination** and then moves longwords
whatever the source is doing. Measured over 1460 bytes, all sixteen alignment
combinations:

| | ns/byte |
|---|---:|
| `memcpy`, both aligned | 216 – 224 |
| `memcpy`, any misalignment | 252 – 260 |

**An 18% penalty, not a cliff** — and the alignment census says it never fires anyway:
application buffers, `nx_packet_data_start`, `nx_packet_prepend_ptr` and
`nx_packet_append_ptr` are all 0 mod 4, and **0 of the 272 checksum calls in a 256 KB
transfer saw a misaligned prepend pointer**. The 16-byte `NX_PHYSICAL_HEADER` that
`nx_user.h` deliberately leaves at its default is doing exactly the job its comment claims.

What *was* available is `movem.l`, which moves eight longwords per instruction pair and
has no C spelling at all — the compiler cannot emit a memory-to-memory move, let alone a
multi-register one. `src/net68k/n68k_copy.S`:

| | ns/byte |
|---|---:|
| `libm020 memcpy`, aligned | 216 – 224 |
| `n68k_copy_bytes`, aligned | 176.6 – 179.5 |
| `n68k_copy_bytes`, misaligned | 225.8 – 228.7 |

**1.23× on the primitive.** `AMINETXDUO_NET68K_MEMCPY` resolves `memcpy()` to it for the
whole library by the same mechanism the checksum uses — define the symbol, and the linker
never pulls libm020's member. The loopback data path spends three copies on every byte,
so this was predicted to be worth 123 ms/MB; measured **140 ms/MB**, 340 → **356 KB/s**.

*Correctness.* 1,552 cases on the target — every length 0–96 × every one of the sixteen
alignment combinations, checking the copied bytes, the byte before the destination and
the byte after it — plus one 8,184-byte misaligned copy that exercises the `movem` block.

#### 3. The cliff that does exist is in our own code

`ami_sana2_copy_bytes()` — the loop the SANA-II shim runs at interrupt level on every
frame in both directions — took its longword path only when source and destination agreed
mod 2:

```c
if (((ULONG)to & 1UL) == ((ULONG)from & 1UL))
```

and copied **one byte per iteration** when they did not. Measured over 1460 bytes:

| | ns/byte |
|---|---:|
| parities agree | 240.3 |
| parities differ | **1203.4** |

**A 5.0× cliff**, avoided only because the driver buffers happen to land on the right
parity — nothing in SANA-II promises anything about a device's buffer alignment, so that
was luck. It is now `n68k_copy_bytes()`, which has no such condition: 179.5 ns/B when the
parities agree (1.34× on what used to be the fast path) and 228.7 when they differ (5.3×
on what used to be the slow one).

This one is **not isolated end to end**, and that is stated rather than glossed: loopback
never touches SANA-II, and separating it from the other two changes on the wire path
would mean keeping the old loop behind a switch, which this project does not do. The
micro-benchmark is the evidence for it.

#### Measured and rejected

- **NetX Duo's `_nxe_` error-checking wrappers cost 3%.** `nx_user.h` leaves
  `NX_DISABLE_ERROR_CHECKING` unset with a note to "revisit for the release build". On
  the hottest call in the stack, `nx_packet_allocate` + `nx_packet_release` is 90 µs
  through the wrappers and 87 µs through `_nx_packet_allocate` / `_nx_packet_release`
  directly. **Not a performance reason to turn error checking off.**
- **A packet allocation costs 88–90 µs, and roughly half of it is `Forbid()`/`Permit()`.**
  One pair measures **9.9 µs** on this port, and `TX_DISABLE`/`TX_RESTORE` expand to a
  `_tx_amiga_forbidden()` plus a `Forbid()`, and a `Permit()`. At twelve packets per 8 KB
  loopback segment that is ~135 ms/MB, about 3.5% — real, but behind the library layer
  and the remaining copies, and changing the port's interrupt-lockout model is not a
  performance decision (§6.2). Recorded, not acted on.
- **`ami_sana2_copy_bytes()`'s own C longword loop is 11% *slower* than `memcpy`**
  (240 against 216 ns/B) for the reason the crypto68k work keeps running into: C forces
  the value through a register, so GCC emits a load and a store where the machine has one
  memory-to-memory move.

#### What is left, in order

1. **`bsdsocket.library` itself: ~1010 ms/MB, now 35% of a loopback megabyte** and
   completely untouched here. It is a per-call `tx_amiga_adopt_thread()` /
   `tx_amiga_orphan_thread()` bracket (§6.3), a `WaitSelect()` poll loop, and a
   descriptor lookup, on every `send()` and `recv()`. The per-call bracket is a
   correctness requirement, not an oversight — an adopted Task holds the ThreadX baton,
   so it may not be held across application code — but "per call" is not the only
   granularity that satisfies that.
2. **The copies that remain: ~727 ms/MB** across `nx_packet_data_append`,
   `nx_packet_copy` and `nx_packet_data_extract_offset`. Two of the three are real work.
   The third is not: `nx_packet_copy()` exists because `_nx_ip_driver_packet_send()`
   duplicates every loopback packet so the receive path can enqueue it, which is 280
   ms/MB of pure API tax on loopback and nothing at all on a real interface. Removing it
   means changing vendored code, so it stays.
3. **`Forbid()`/`Permit()` at 9.9 µs a pair**, below.

#### A mistake worth recording, because its shape is easy to repeat

The first end-to-end pass reported ~45 KB/s for every configuration, and a 3.11× checksum
moved it by 4%. The harness was stopping its clock after `nx_tcp_socket_disconnect()`,
and because the server closed first while the client was still waiting to be told the
transfer had finished, that call burned its full five-second timeout on every run. A flat
constant added to both arms of an A/B **does not cancel** — it flattens the ratio and
makes a real 1.55× look like 1.04×. The phase accounting that found it (bracket every
call, and check that the parts add up to the whole) is still in `p_transfer()`; the
symptom was that sender-busy plus receiver-busy came to 1.25 s of a 5.65 s "transfer".

#### Baselines

Re-measured on the final build, not assumed:

| | 68020 | 68030 |
|---|---|---|
| ThreadX-on-Exec `soak` | 98 checks, 0 fail | 98 checks, 0 fail |
| `ram_driver` | 32/32 | 32/32 |
| `netstack` (real SANA-II device) | 14/14 | — |
| `libraries` (the ABI, through `OpenLibrary`) | 8/8 | — |
| conformance, loopback tier | 125 passed, 1 failed, 16 skipped | 125 passed, 1 failed, 16 skipped |
| conformance, network tier | 6/6 throughput, 0 skipped | — |
| `perf_test` self-checks (checksum differential + copy exactness) | 28/28 | 28/28 |
| host `ctest` | 6 suites, 0 fail (was 5; `net68k_checksum` is the new one) | |

The one conformance failure is the pre-existing `SOCK_RAW`/`EACCES` disagreement
documented in the README, unchanged.

### Machine profiles, and calibrating the emulator before believing it (2026-07-25)

Every performance figure above is an A1200: 68EC020 at 14 MHz, 16-bit path to memory.
An A3000 — 68030 at 25 MHz, 32-bit Fast RAM on a 32-bit bus, and a data cache the 020
does not have at all — is a materially different machine, and for a stack dominated by
copying and checksumming the memory width plausibly matters more than the clock. So
`tools/fsuae-run.sh` grew an A3000 profile.

It also grew the thing that had to come first. This project has concluded three separate
times, from three unrelated symptoms, that FS-UAE's 68030 model cannot be trusted for
timing — 1.93 ns/byte for a memory copy against the 68020's 184, a `MULU.L` that appears
to cost nothing, and one RSA measurement that came out 1.7×, then 3.0×, then 3.1× on
three runs of one binary. Nobody had measured *what* it gets wrong, so nobody could say
what an A3000 profile would be good for. **`tests/perf/cpucal.c` measures it**, and the
answer turns out to be a single mechanism that explains all three symptoms at once.

#### The probe, and why its primary results need no clock

`cpucal` runs instruction sequences whose cost on real silicon is published (`cpucal.S`),
times them with `ReadEClock()`, and reports what the emulator charged. An absolute
nanoseconds-per-instruction is worth nothing on its own — it conflates the model's cycle
accounting with whatever clock the emulator thinks it is running at, and one figure
cannot separate them. So the primary results are **ratios between kernels measured in
the same run**, which are clock-independent by construction:

- `MULU.L` against `ADD.L` is 44/2 = **22.0** on a real 68030 (43/2 = 21.5 on a 68020);
- a 32 KB read window against a 64-byte one is ~1.0 on a part with **no** data cache, and
  well above 1.0 on one with 256 bytes of it;
- Chip RAM against Fast RAM is ~2× on an A1200's 16-bit path and much more on an A3000's.

Only after those does it quote an implied clock, from `ADD.L` at its published two
cycles. The published costs are from the MC68020UM and MC68030UM timing appendices
(`MUL.L EA,Dn` 43 and 44 respectively — checked in the manuals, not recalled).

#### What the A1200 profile reproduces: essentially everything

| | measured | real 68020 | |
|---|---:|---:|---|
| `ADD.L Dn,Dm` | 143.30 ns | 2 cycles | the yardstick |
| `MOVE.L Dn,Dm` | 144.05 ns | 2 | **2.00** implied |
| `ADDX.L Dn,Dm` | 143.13 ns | 2 | **1.98** implied |
| `MULU.L Dn,Dm` | 2302.90 ns | 43 | **32.12** implied |
| `MULU.L Dn,Dh:Dl` | 2303.71 ns | 45 | **32.14** implied |
| implied clock | **13.95 MHz** | 14.187 MHz | **1.7% low** |

Three runs of the binary gave `ADD.L` at 143.304, 143.317 and 143.317 ns — a spread of
one part in ten thousand. The memory rows repeat to 1.4%.

**Two-cycle integer work is faithful to under 2%. `MULU.L` is not: the model charges 32
cycles where the part charges 43, so it is 25% cheap even here.** That is a new caveat on
the crypto68k figures, and a mild one — it flatters assembly that moves work out of the
multiplier by at most a quarter, where the 68030 model flatters it by a factor of ten.

The memory model is internally coherent, which is the check that matters more than any
single row. Per longword, Fast RAM: read 6.75 cycles, write 3.64, and memory-to-memory
10.47 — against 6.75 + 3.64 = 10.39 for the parts, i.e. 0.8% out. Chip RAM comes out
1.88× slower than Fast, which is the right shape for a machine whose Chip bus is shared
with the DMA. And the 32 KB / 64 B read ratio is 0.89, correctly saying that a 68020 has
no data cache.

| A1200, 32 KB window, ns/byte | Fast RAM | Chip RAM |
|---|---:|---:|
| `MOVE.L (An)+,Dn` | 121.07 | 227.29 |
| `MOVE.L Dn,(An)+` | 65.31 | 148.66 |
| `MOVE.L (An)+,(Am)+` | 187.83 | 364.31 |
| `MOVEM.L`, 8 registers | 152.97 | 321.21 |

These agree with `perf_test`'s independent measurements of the same primitives, in the
directions the code says they should: 187.83 for a bare `MOVE.L (An)+,(Am)+` against
libm020 `memcpy`'s 216–224, which is the same instruction with a fourfold rather than a
sixteenfold unroll; and 152.97 for a bare `MOVEM.L` burst against `n68k_copy_bytes`'s
184.7, which pays head alignment, tail longwords and a tail byte loop that this kernel
does not. Two harnesses written months apart, one answer.

#### What the A3000 profile reproduces: not enough to quote

| | A1200 model | A3000 model | real 68030 |
|---|---:|---:|---:|
| implied clock | 13.95 MHz | **323.9 MHz** | 25 MHz |
| `MULU.L` implied cycles | 32.1 | **3.2** | 44 |
| Fast RAM read, ns/B | 121.07 | 2.31 | — |
| Chip RAM read, ns/B | 227.29 | 2.60 | — |
| Chip / Fast | 1.88× | **1.12×** | should be several × |
| 32 KB / 64 B read | 0.89× | 0.82× | above 1 — the 030 has a data cache |
| forcing `CACRF_EnableD` on | n/a | **no effect at all** | should be large |

**The root cause is one line in FS-UAE's own log, and it explains all three of this
project's earlier observations at once.** The emulator's A3000 quickstart runs *after* it
has read the configuration file and overwrites what it finds there:

```
set option "cpu_speed" to "max" (result: 1)
set option "cpu_compatible" to "false" (result: 1)
set option "cpu_cycle_exact" to "false" (result: 1)
currprefs.m68k_speed is -1, not allowing full sync
```

The A1200 model, in the same build, gets `cpu_speed = real` and `cpu_cycle_exact = true`.
**FS-UAE 3.2.35 switches cycle accounting off for every CPU above a 68020**, and that is
why `-c 68030` has always produced nonsense too: the A1200 model with `cpu = 68030` gets
the same `max` / `false` pair. With no cycle accounting the CPU runs as fast as the host
allows, so the E-Clock — which advances with emulated chipset time — sees the work
complete almost instantly. A 95× memory copy, a free `MULU.L` and an RSA ratio that
wanders between 1.7× and 3.1× are three faces of one thing.

#### Everything that was tried to fix it

Swept with `cpucal`, one variable at a time, through `AMINETXDUO_FSUAE_EXTRA`:

| | effect |
|---|---|
| `accuracy = 1` | none — it is already the A3000 model's default (`accuracy=1` in the log) |
| `cpu_speed = real` | none — overwritten by the quickstart |
| `cpu_cycle_exact = 1`, `blitter_cycle_exact = 1` | none — overwritten |
| `cpu_frequency = 25000000` | none — not an FS-UAE option at all, silently ignored |
| `cpu_multiplier = 4`, `uae_cpu_frequency` | none |
| `cpu_model = 68020` on the A3000 model | none — the log still reports `CPU=68030` |
| **`uae_cpu_cycle_exact = true`** | **works** — the `uae_` passthrough is applied last |

So cycle accounting *can* be forced back on (`CPU=68030 … ~cycle-exact fast` in the log),
and the result is deterministic to the picosecond across runs. It is still not an A3000:

- the clock lands at an implied **3.38 MHz** and `uae_cpu_multiplier` does not move it
  (3.36 at ×2, 3.29 at ×4) — seven times too slow;
- `MULU.L` is charged **4.1 cycles against 44**, so the crypto caveat is unaffected;
- Chip RAM and 32-bit motherboard RAM measure **479.0 and 480.0 ns/B** — the model does
  not distinguish them, which is exactly the distinction an A3000 profile exists to make;
- `MOVEM.L` comes out **5.4× cheaper per byte** than `MOVE.L (An)+,(Am)+`, against about
  1.3× on the real part — so it is wrong in the one place `src/net68k/` is optimised.

**It is not more faithful, it is differently unfaithful, and three times slower to run.**
It is therefore not in the profile; the recipe is in the comment in `tools/fsuae-run.sh`
for anyone who wants to reproduce the finding.

#### What the A3000 profile IS for

`-m A3000` gives a 68030 with an MMU, both caches, 32-bit addressing and 8 MB of
motherboard RAM rather than Zorro II. That is a genuine second target for **correctness**
— it is where Enforcer can run, and where a DMA-coherency bug against the 030 data cache
would show — and the profile is built and kept for that. It prints
`(NOT a timing profile)` when it starts, because the failure mode being guarded against
is somebody quoting it in six months.

Deliberately *not* in the profile: `cpu_model`, `cpu_frequency`, `cpu_cycle_exact` and
`blitter_cycle_exact`. All four were in the first version, all four were swept, and all
four are inert. Configuration that looks like it pins the machine down and does not is
worse than none.

#### The measurement that survives: `-k`, a clock that moves without breaking anything

The 68030 model cannot be made faithful. The 68020 model already is — and
`uae_cpu_multiplier` *does* get through on it, because the A1200 quickstart leaves
cycle-exact on. `tools/fsuae-run.sh -k MHZ` uses that:

| `-k` | nominal | implied clock, measured | `ADD.L` | `MULU.L` |
|---|---:|---:|---:|---:|
| (none) | 14.0 MHz | 13.95 MHz | 2.00 cycles | 32.12 |
| `-k 7` | 7.0 MHz | 6.80 MHz | 2.00 | 32.1 |
| `-k 25` | 24.5 MHz | **24.48 MHz** | 2.00 | 31.98 |
| `-k 28` | 28.0 MHz | 28.01 MHz | 2.00 | 31.98 |

The instruction accounting is unchanged at every clock — only the rate moves. The
multiplier's unit is half the 7.09 MHz PAL chipset clock, ~3.5 MHz per step, measured
because the emulator documents it nowhere.

**And the memory model does the right thing under it, which is what makes the option
worth having.** Doubling the clock from 13.95 to 28.01 MHz buys **2.03×** on Fast RAM
and only **1.49×** on Chip RAM, and `MOVEM.L` from Chip RAM barely moves at all
(**1.07×**) — because Chip RAM is chipset-bound and the model knows it. A knob that made
everything uniformly faster would be measuring nothing.

So `-m A1200 -k 25` is a **clock-matched, cycle-accurate 68020 at 24.5 MHz**. It is not
an A3000, and it differs in exactly two known ways: it is a 68020, so there is no data
cache; and its memory is the A1200's, which the model charges 6.75 cycles per longword
read where a 32-bit port costs the 68020's published 4. **It is therefore a defensible
lower bound on an A3000, not an estimate of one** — a real A3000 has the same clock and a
wider path to memory, so it can only be faster.

Three clocks make a slope rather than a ratio, and the slope is the interesting part:

| Fast RAM read, `MOVE.L (An)+,Dn` | 6.80 MHz | 13.95 MHz | 24.48 MHz | 28.01 MHz |
|---|---:|---:|---:|---:|
| ns/byte | 243.97 | 120.00 | 67.55 | 58.90 |
| **ns/byte × MHz** | **1659** | **1674** | **1653** | **1650** |

Constant to 1.4% — Fast RAM costs a fixed number of *CPU cycles* in this model, as
CPU-synchronous memory should. Chip RAM does not: the same product goes 1667 → 3171 →
3737 → 4269, i.e. it is CPU-bound below ~7 MHz and bus-bound above it. **The model has a
memory wall in it and puts it in the right place**, which is the licence to use `-k` for
the question below.

#### The measurements, with the caveat attached to each row

Every figure from `tests/perf/perf_test.c`, 256 KB per transfer, raw NetX Duo sockets
with `src/net68k/`'s checksum and copy, 28/28 self-checks passing on every profile.

| | 6.80 MHz | **13.95 MHz** | **24.48 MHz** | A3000 model |
|---|---:|---:|---:|---:|
| | trustworthy | trustworthy | trustworthy | **fiction** |
| TCP loopback, drain only | 286 KB/s | 610 KB/s | **1080 KB/s** | 28,444 KB/s |
| TCP loopback, `+extract` | 251 KB/s | 541 KB/s | **966 KB/s** | 25,600 KB/s |
| TCP over the RAM driver | 105 KB/s | 174 KB/s | **239 KB/s** | 412 KB/s |
| pipeline ceiling, no protocol | 426 KB/s | 900 KB/s | **1611 KB/s** | 45,976 KB/s |
| IP checksum, ns/B | 426.8 | 203.8 | 112.6 | 5.3 |
| `n68k_copy_bytes`, ns/B | 383.7 | 184.7 | 99.5 | 2.6 |
| `nx_packet_copy`, ns/B | 578.3 | 276.6 | 153.1 | 4.7 |
| `Forbid()`/`Permit()` pair | 18.9 µs | 10.5 µs | 5.4 µs | 0.13 µs |
| TLS 1.2 handshake, loopback | — | **26.7 s** | **15.0 s** | 0.8 s |
| … client-only public-key arithmetic | — | **3.2 s** | **1.8 s** | 0.0 s |
| ECDSA P-256 verify, `crypto68k` | — | **1966 ms** | **1113 ms** | 44 ms |
| ECDHE P-256 shared secret | — | **1372 ms** | **776 ms** | 29 ms |

And the figure the README quotes, which is the same transfer one layer further up —
`bsdsocktest`'s throughput category through `bsdsocket.library`, 1 MB sustained:

| | **13.95 MHz** | **24.48 MHz** | A3000 model |
|---|---:|---:|---:|
| TCP sustained loopback, 1 MB, through the library | **357 KB/s** | **636 KB/s** | 17,066 KB/s |
| TCP loopback, 512 KB | 358 KB/s | 638 KB/s | 12,800 KB/s |
| conformance score | 125/1/16 | 125/1/16 | 125/1/16 |

357 KB/s against the 356 recorded above, and **636 KB/s at 24.48 MHz — 1.78× for a 1.76×
clock**, the same linear scaling the raw-socket figure shows. The library layer is as
CPU-bound as everything under it.

**The A3000 column is printed to be argued with, not quoted.** A 2.6 ns/byte copy is
faster than the 68030's bus can physically move a byte at any clock; it is what "no cycle
accounting" looks like.

And one row that shows the distortion directly rather than by argument. The `crypto68k`
speedups are *ratios* measured back to back in one process, so they should not depend on
the clock at all — and on the profiles that charge cycles, they do not:

| `crypto68k` against vendored | 13.95 MHz | 24.48 MHz | A3000 model |
|---|---:|---:|---:|
| ECDSA P-256 verify | 3.5× | **3.5×** | **4.5×** |
| ECDHE P-256 shared secret | 3.8× | **3.8×** | **4.9×** |
| ECDHE P-256 keygen | 3.8× | **3.8×** | **5.1×** |
| RSA-2048 public | 2.9× | **2.9×** | **4.1×** |
| RSA-2048 private, CRT | 2.2× | **2.2×** | **3.0×** |
| RSA-2048 private, plain | 2.4× | **2.4×** | **3.7×** |

Identical across a 1.76× clock change, inflated by 25–54% on the A3000 model — which is
exactly the failure the P-256 notes predicted from first principles ("the 68030 model
does not charge for `MULU.L`, so it flatters exactly the work this change moves *out* of
the multiply"). It is pleasant to have it confirmed by an experiment designed
independently of the prediction, and **it retires the last unexplained symptom**: the RSA
ratio that came out 1.7×, 3.0× and 3.1× on three runs was not a flaky measurement, it was
a ratio with no fixed value to converge on.

Repeatability, since that was the third symptom. Three runs of one binary:

| `ADD.L` | run a | run b | run c | spread |
|---|---:|---:|---:|---:|
| A1200 | 143.304 ns | 143.317 ns | 143.317 ns | **0.01%** |
| A3000 model | 324.35 MHz | 325.30 MHz | 317.91 MHz | 2.3% |

The A3000 model is more repeatable than the RSA episode suggested *for a short kernel*,
because `cpucal` auto-scales each measurement to about a tenth of a second. Over the
minutes an RSA benchmark takes, the same unthrottled CPU is measuring the host's
scheduler, and that is where 1.7× against 3.1× comes from.

The 13.95 MHz column reproduces what this section recorded before the profiles existed —
203.8 against 200.7 ns/B for the checksum, 541 against 512 KB/s for loopback, 26.7 s
against 26.7 s for the handshake, and RSA-2048 public at 681.1 ms against 681 — which is
the check that nothing about the new profiles disturbed the old ones.

**A defect in `perf_test` that the new `-k` option exposed, and what it does and does not
change.** `p_report()` computed a per-primitive ns/byte from `ticks / reps` — an integer
count of 1.409 µs E-Clock ticks. That is harmless at 14 MHz, where a 1460-byte copy is
~190 ticks and the truncation is bounded by 0.5%; at 24.48 MHz it is ~105 ticks, and
three copy routines that differ by 4% all printed *the same* 102.29 ns/B. It now divides
once, by the total byte count, at the end.

Fixing it moved the 13.95 MHz `n68k_copy_bytes` row from the 176.6 ns/B recorded above to
**184.7**, and the arithmetic says that is mostly *not* the fix: truncation can only
account for 0.5% of a 4.6% move. The rest is run-to-run variation, which the copy rows
have and the others do not — the same binary reported 257 µs and 269 µs per repetition in
two sessions, while the checksum repeated to 0.6% and every end-to-end throughput figure
repeated exactly. **Read a 3% difference between two copy rows as noise.** The
1.23×-over-`libm020` conclusion stands regardless: that comparison was two arms of one
session, truncated identically, and the gap is far larger than either effect.

The per-cycle cost falls slightly as the clock rises — checksum ns/B × MHz goes 2902 →
2843 → 2756 across the three columns, about 5%. That is not a modelling error: interrupts
arrive at a fixed *wall* rate and cost a fixed number of *cycles*, so a benchmark that
finishes in half the time absorbs half of them. Real hardware does the same.

#### What it implies for what to optimise next

**Loopback TCP is instruction-bound, not bus-bound, and the evidence is the slope.**
Throughput against clock: 251 → 541 → 966 KB/s for 6.80 → 13.95 → 24.48 MHz, i.e. 2.16×
for a 2.05× clock and 1.79× for a 1.76× clock. **Linear, slightly better than linear.**
Every primitive underneath behaves the same way: the checksum, the copy and
`nx_packet_copy` all hold their ns/byte × MHz product to within a few percent.

So the answer to "does an A3000 change what to optimise?" is **no, and the reason is
worth having**: nothing on the loopback path is waiting for memory that a faster CPU
would not fix. The ranking recorded above — `bsdsocket.library`'s per-call overhead
first at ~1010 ms/MB, the remaining copies second — survives the clock change unaltered,
because both scale with it. A 25 MHz machine runs the same profile 1.8× faster.

**The wire path is the exception, and it is the finding the A3000 profile earned its
keep with.** Over the RAM driver, throughput goes 105 → 174 → 239 KB/s: 1.66× for a
2.05× clock, then 1.37× for a 1.76×. Sub-linear, and flattening. The unfaithful A3000
profile is what identifies the wall — with the CPU effectively free it still manages only
**412 KB/s**, so 412 KB/s is the ceiling imposed by everything that is *not* the CPU.
Fitting the three trustworthy points against a fixed ceiling in series:

| | 6.80 MHz | 13.95 MHz | 24.48 MHz |
|---|---:|---:|---:|
| measured | 105 KB/s | 174 KB/s | 239 KB/s |
| implied CPU-bound component | 141 KB/s | 301 KB/s | 569 KB/s |
| scaling of that component | — | 2.14× | 1.89× |

The CPU-bound part is linear in the clock to within 7%, and the model fits all three
points. **So on a real interface, above about 15 MHz, roughly half of what is left is
not CPU at all** — it is the 50 Hz IP periodic tick and the round trips paced by it, and
no amount of assembly in `src/net68k/` will touch it. That is a different optimisation
(window sizes, delayed-ACK behaviour, tick rate) from anything this section has done so
far, and it is the one that matters for the machine most likely to have an Ethernet card.

**TLS is entirely CPU-bound**, as arithmetic should be: 26.7 s → 15.0 s for a 1.76×
clock is 1.78×. An A3000 owner sees a ~15 s handshake and a ~1.8 s client-side
public-key cost. That is the same shape of answer AmiSSL's own A3000 datapoint gives
(issue #67, `SSL_connect` in 2.99 s on a 68030 at 25 MHz — TLS 1.3, a different
handshake and a heavily optimised bignum library, but the same universe).

#### Baselines, on both profiles

The A3000 profile is a correctness vehicle, so it was checked as one. Nothing regressed,
and nothing about the harness change disturbed the A1200 path:

| | A1200 | A1200 `-k 25` | A3000 profile |
|---|---|---|---|
| ThreadX-on-Exec `soak` | 98/0 | 98/0 | 98/0 |
| `ram_driver` | 32/0 | — | 32/0 |
| `libraries` (the ABI through `OpenLibrary`) | 8/0 | — | 8/0 |
| conformance, loopback tier | 125 passed, 1 failed, 16 skipped | same | same |
| `perf_test` self-checks | 28/0 | 28/0 | 28/0 |
| `tls_handshake` | 44/0 | 44/0 | 44/0 |
| `crypto68k_ec_bench` / `crypto68k_bench` | 0 failures | 0 failures | 0 failures |
| host `ctest` | 6/6 | | |

The one conformance failure is the pre-existing `SOCK_RAW`/`EACCES` disagreement.

#### What would settle it on real hardware

Three measurements, in order of what they would change:

1. **`cpucal` on an A3000.** It is 300 lines and needs nothing but `timer.device`. It
   would give the one number this whole section had to work around: what a 68030 at 25
   MHz actually charges for a longword read from 32-bit motherboard RAM. Everything
   above is bounded rather than known because of that single unknown.
2. **`perf_test` and the conformance suite on an A3000**, for the loopback figures — the
   predictions being **≥966 KB/s** raw and **≥636 KB/s** through the library, those being
   the `-k 25` lower bounds, with the excess over them being what the 32-bit path and the
   data cache are worth.
3. **The conformance suite's throughput category over a real Ethernet card**, because the
   412 KB/s non-CPU ceiling above is the RAM driver's, and a real interface's will be a
   different number for different reasons.

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

### The three-certificate "crash" was the emulator dying of SIGPIPE (2026-07-25)

`fetch https://www.iana.org/` at the A1200's 14 MHz took FS-UAE down: no output, no
`DH0:.done`, the harness reporting `fs-uae exited early after 57s`, and the UAE core log
offering `B-Trap F201 at 00F80CA0 -> 00F80CC0` by way of explanation. The same binary
against the same host at `-k 28` was clean. A chain of two certificates was fine, three
died at 14 MHz, four died at 28 MHz as well. That looked like a threshold in elapsed
time, and the standing theory was that a slow handshake ran out the far end's patience
and `nx_secure` then walked into reused or freed state on the close.

**It was none of that. `fs-uae` was being killed by SIGPIPE.** Run in the foreground the
emulator exits **141**, and 141 is 128 + 13. Set the disposition to `SIG_IGN` in the
parent — which, unlike a handler, survives `execve()` — and the identical run at the
identical clock finishes: `fetch: www.iana.org: the connection is closed`, return code
10, `DH0:.done` written, and the commands after it still run.

The chain from cause to symptom, read off FS-UAE's own A2065 packet dump:

1. The client sends its ClientHello. The server answers with a 2,732-byte flight:
   ServerHello, three certificates (883 + 675 + 894 bytes), ServerKeyExchange,
   ServerHelloDone.
2. The client disappears into the verification arithmetic for tens of seconds and
   **acknowledges nothing at all** while it is in there — SLIRP retransmits the same
   segment three and four times over. The stack cannot answer, because the calling task
   is holding the ThreadX baton across the whole of `_nx_secure_tls_session_start()`.
3. The far end gives up waiting for a ClientKeyExchange and closes. Its FIN sits behind
   the unacknowledged data in SLIRP's send queue, so it reaches the guest only *after*
   the client finally speaks — which is why the FIN's sequence number is exactly the end
   of the certificate flight.
4. The client, which has meanwhile finished and knows nothing of any of this, sends
   ClientKeyExchange, ChangeCipherSpec and Finished.
5. SLIRP writes those to a host socket whose peer is gone. A socket is spared SIGPIPE
   only by `SO_NOSIGPIPE` or by sending with `MSG_NOSIGNAL`, and fs-uae 3.2.35 on macOS
   demonstrably uses neither. The emulator dies mid-instruction.

Everything that made this look like a guru follows from step 5. No `.done`, because there
is no emulator left to write one. Nothing on the serial port, for the same reason. And a
core log whose size is always an exact multiple of 4,096 because stdio's buffer was never
flushed — four older runs in `build/` carry that same fingerprint.

**`B-Trap F201 at 00F80CA0` is Kickstart's own FPU probe and is unrelated.** It is the
UAE core's `op_illg()` reporting a line-F opcode; it sits at line 910 of the log, some
120 lines before the TCP connection is even opened, and it is there in every run,
including the ones that pass. The harness only prints the core log when the emulator dies
early, which is the whole reason it had never been seen next to a success. Treat that
section of the output as evidence about the host, not about the Amiga.

**Proved, not inferred:** the exit status, the disappearance of the failure under
`SIG_IGN` (repeated for both the three- and the four-certificate host), the packet
sequence above, the position of the `B-Trap` line, and the four-kilobyte log truncation.
**Inferred:** that the EPIPE is on SLIRP's host socket specifically. Nothing else in the
configuration is a pipe or a socket — stdout, the emulator log and the serial port are
all plain files — and there is a control for it: `run-hangup.sh`, where the peer closes
*before* the guest writes again, does **not** kill the emulator even with SIGPIPE left at
its default. The death needs a guest write into a closed connection, which is what SLIRP
turns into a host `write()`. What was not done is attaching a debugger to catch the
`write()` itself; `lldb` on the Homebrew fs-uae ran the emulation far too slowly to reach
the handshake and was abandoned.

The fix is in `tools/fsuae-run.sh` and `tools/enforcer-run.sh`: launch the emulator from
a subshell that has ignored SIGPIPE, and report a death by signal *as* a death by signal
instead of as "exited early". Both scripts now say so in as many words, because the next
person to meet this needs to be told at the top of the output that what died was the
emulator.

#### The library was already right, and now there is a test that says so

`tests/tls/run-hangup.sh` (with `tests/tls/hangup-server.py`) is the direct version of
the experiment that waiting for `www.iana.org` was the indirect one of. Four listeners on
the host, reached through SLIRP's `10.0.2.2` gateway alias, each of which reads the
ClientHello and then misbehaves in one specific way. On the 14 MHz A1200:

| the peer | `fetch` says | rc |
|---|---|---|
| resets (`SO_LINGER 0`) | the connection is closed | 10 |
| closes tidily (FIN) | the connection is closed | 10 |
| answers nothing, ever | the server stopped responding | 10 |
| answers bytes that are not TLS | the connection is closed | 10 |

Four legible errors, and the machine carries on to the next command every time. **A peer
cannot take this machine down by hanging up, and could not before this change either** —
the fault was never in `tls.library`. Unlike `run-fetch.sh` and `run-api.sh` this one *is*
a baseline: the rude peer is a script in this tree, on loopback, so there is no internet,
no third party and no certificate that can rotate underneath it.

#### What is actually left: the far end's patience

With the emulator no longer dying, the three- and four-certificate hosts fail honestly.
Measured through `fetch`, `TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256` throughout:

| host | front end | certificates | 14 MHz | `-k 28` | `-k 56` |
|---|---|---|---|---|---|
| `ecc256.badssl.com` | GCP | 2 | **23.3 s** verified | **11.7 s** verified | — |
| `www.iana.org` | Cloudflare | 3 (883 + 675 + 894 B) | closed on us | **11.3 s** verified | — |
| `example.com` | Cloudflare | 4 (1003 + 742 + 824 + 1090 B) | closed on us | closed on us | **9.8 s** verified |

Every one of those that completes reports `chain verified, validity dates checked`, so
nothing about three- or four-deep chains is wrong: **the only variable is elapsed time
against how long a given front end will wait for a ClientKeyExchange**, and that differs
between operators by more than a factor of two. The two Cloudflare hosts bracket their
own timeout — `www.iana.org` succeeds at 11.3 s, and `example.com`, which by its 56 MHz
figure costs about 19.6 s at 28 MHz, does not. So Cloudflare's patience is somewhere
between 11.3 and roughly 20 seconds, which a **15-second handshake timeout** would fit.
badssl.com's, on Google Cloud, is over 23.3 s, which is why the two-certificate hosts in
`run-fetch.sh` have never shown any of this.

The practical reading: at the 14 MHz floor, only a two-certificate chain finishes inside
a mainstream CDN's patience, and two-certificate chains are the minority of the web.

Two things follow that are worth keeping.

- **The obvious cheap win is not there, and it was worth checking.** The suspicion was
  that a server which helpfully includes the cross-signing root in its own chain makes us
  verify a certificate we already trust, because the vendored store lookup would find the
  server's copy before ours. It does not:
  `_nx_secure_x509_store_certificate_find()` searches **trusted first**, local second,
  remote third, and `_nx_secure_x509_certificate_chain_verify()` returns the moment an
  issuer comes back from the trusted store. A four-certificate chain therefore already
  costs only as many signature checks as it takes to reach a root we hold. There is no
  redundant verification to remove.
- **The stack stops while the arithmetic runs, and the wire shows it.** Three and four
  SLIRP retransmissions of the same segment, with not one ACK from the guest, is the
  ThreadX baton being held by the adopted task across the whole handshake
  (`port/threadx-amiga/src/tx_amiga_adopt.c` documents the hazard; this is it happening).
  Under SLIRP it is cosmetic, because SLIRP terminates TCP and has already acknowledged
  the real server on our behalf — what the real server is waiting for is a
  ClientKeyExchange, not an ACK. On real hardware it will not be cosmetic: the server
  sees the silence directly. `tls_store_fetch()` already shows the shape of the fix,
  releasing and reacquiring the baton around a blocking call, and the certificate
  verification is pure arithmetic that touches no ThreadX object, so the same bracket
  would fit around it. Costed, not done: it is a separate unit of work and its benefit
  cannot be measured on this harness.

#### TLS by default: still no, and now for a different reason

The blocker is no longer a crash — there was no crash, and the argument in §4 above that
rested on one is void. Nothing can be taken down by a peer that is slow, rude or absent;
that is four measured cases and a baseline test that keeps them measured.

It stays off anyway, and the honest reason is worse for being ordinary: **at 14 MHz this
finishes a three-certificate handshake in about 23 seconds and Cloudflare waits about
fifteen.** A `LIBS:tls.library` that fails on a large fraction of the web through no
fault of the caller is not a default; it is a footgun with good error messages. The
two-certificate case works today and works well, `-DAMINETXDUO_TLS=ON` stays a supported,
CI-covered configuration, and `fetch` says something true and actionable when it cannot
connect.

What would change the answer is roughly a **2× on the client half of the handshake** —
enough to bring 23 s under 15 — and it is arithmetic, which is the one thing on this
machine that has repeatedly turned out to be possible to make faster. Specifically not
the error paths, which are now known good, and specifically not the certificate store or
the chain walk, both of which were checked here and are already doing the minimum.
The first place to look is what the 14 MHz column is actually spent on, which nothing has
yet measured: `TLSInfo()` reports only the total.

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

### `nc`, `telnet` and `ftp`: the server half of the ABI, and what SLIRP will not do (2026-07-25)

Three more Roadshow-era commands, written against `bsdsocket.library`'s published
vectors rather than ported: `src/tools/nc.c`, `src/tools/telnet.c`,
`src/tools/ftp.c`, sharing `src/tools/toolsock.c` for the LVO calls. Nineteen
vectors between them, against `fetch`'s eight — and the extra eleven are the
point. `bind`, `listen` and `accept` had never been called by anything in this
tree except `tests/conformance/conf_probe.c`, which calls them from **one**
process. `nc -l` calls them from a different process from the one that connects,
and active-mode FTP calls them in the middle of a session that already has a
connection open.

#### What the commands are

- **`nc`** — `HOST,PORT,LISTEN=-l/S,UDP=-u/S,SCAN=-z/S,TIMEOUT=-w/N/K,LOCALPORT=-p/N/K,HALFCLOSE=-N/S,VERBOSE=-v/S,CRLF/S`.
  Connect mode, listen mode, and `-z` for a connection test over a port or a
  range. `-w` switches `connect()` to the non-blocking `FIONBIO` +
  `WaitSelect(writefds)` + `SO_ERROR` form, so the plain path stays blocking
  and simple.
- **`telnet`** — `HOST/A,PORT,DEBUG=-d/S,QUIET/S`. Agrees to `ECHO` and
  `SUPPRESS-GO-AHEAD`, refuses everything else with `WONT`/`DONT`, and skips
  subnegotiations to `IAC SE`.
- **`ftp`** — `HOST,PORT,USER/K,PASSWORD/K,ACTIVE/S,DATAPORT/N/K,TIMEOUT/N/K,DEBUG=-d/S`,
  commands on standard input. Both transfer modes; `DATAPORT` pins the
  active-mode data port so a forwarding rule can name it.

#### The half-close defect, found and then fixed underneath us

`nc -N` (shutdown the write half at end of input) was the first thing in the
tree to call `shutdown(SHUT_WR)` in anger, and on a connection whose **two ends
were both on this machine** it wedged the caller inside the library — past the
reach of Ctrl-C, past `-w`, and past the harness timeout. The peer's next
`recv()` failed with an errno the command had no name for.

The A/B that identified it, on `fd1de16`: the identical staged command list
completed in 70 s with the `shutdown()` call compiled out, and never finished
with it in. Over a wire — the same `nc` against a host-side echo server through
SLIRP — the half-close was correct throughout: the FIN arrived, the answer came
back, the connection closed.

By the time it was written up, `src/bsdsocket/` had it. `e63e5f1` gave
`shutdown(SHUT_WR)` a real FIN instead of a `nx_tcp_socket_disconnect()` RESET,
and `ed548df` stopped `bsd_readable()` counting `FIN_WAIT_1`/`FIN_WAIT_2` as
readable — which is what left a half-closed socket spinning on a `select()` that
returned immediately and a `recv()` that then blocked forever. Re-verified on
`ed548df`: all three cases of `tests/tools/commands-samehost.txt` pass, `-N`
included, and that file is now the regression test rather than the reproducer.

**Nothing in `src/bsdsocket/` was touched from this side.**

#### FS-UAE's SLIRP accepts `slirp_redir` and does not implement it

SLIRP is a NAT, so testing `accept()` against something outside the guest needs
a forward. FS-UAE has the option and takes it: with
`uae_slirp_redir = tcp:7042:7042` in the config, `fs-uae.log.txt` records
`set option "slirp_redir" to "tcp:7042:7042" (result: 1)` — and **no host socket
is ever created**. Measured three ways: `lsof -nP -iTCP:7042 -sTCP:LISTEN`
finds nothing while the emulator is up; a host-side dialler retrying every two
seconds got `ECONNREFUSED` 120 times in a row; and the guest's `nc -l 7042`
sat out its full 120-second timeout and reported "nobody connected". The parse
succeeds, the forward does not exist. FS-UAE 3.2.35, macOS, `qemu_uae_slirp`.

So of the two ways to exercise the server half:

- **Host → guest: impossible in this emulator.** Not attempted further.
- **Guest → guest: done, and it is what the claims below rest on.** Two `nc`
  processes on the Amiga, over `127.0.0.1` and over the machine's own
  `10.0.2.15`, with `ToolsSmoke` running one of them in the background
  (`&` prefix) so both are alive at once.

The same limit is why **active-mode FTP is proven up to `accept()` and not
through it**: the client binds, listens, sends a correct
`PORT 10,0,2,15,27,148` (27·256+148 = 7060, the pinned `DATAPORT`), takes the
server's `200` and `150` — and the callback cannot reach it, so it reports
"the server never opened the data connection" and carries on with the session.
The `accept()` that would have completed it is the same code path the guest-to-
guest `nc` runs, which does complete.

#### What was verified, against real servers

`tests/tools/run-nettools.sh` boots the A1200 profile on SLIRP with
`tests/tools/netpeer.py` on the host: an echo server, a telnet server that
offers `WILL ECHO`, `WILL SGA`, `DO TERMINAL-TYPE`, `DO NAWS` and records what
comes back, and an FTP server doing both `PASV` and `PORT`.

- `nc -z` names an open port, a refused one, and a range.
- `nc` client carries 21 bytes to the echo server and prints the echo; with
  `-N` the server sees the FIN and closes, and `nc` exits 0.
- `nc -l` binds, listens, accepts a connection from another process on the
  Amiga and prints what it is sent, over `127.0.0.1` and over `10.0.2.15`.
- `telnet` answers `DO ECHO`, `DO SUPPRESS-GO-AHEAD`, `WONT TERMINAL-TYPE`,
  `WONT WINDOW-SIZE` — verified from the server's side, not just ours — then
  carries the session and reports the close.
- `ftp` passive: `USER`/`PASS`/`SYST`/`PWD`/`PASV`/`LIST`/`TYPE I`/`RETR`/
  `SIZE`/`STOR`/`QUIT`, 49 bytes down and 21 up, both byte-exact.
- `ftp` active: as above.
- The failures read as sentences: "connection refused", "cannot resolve", "the
  server never opened the data connection" with the advice that passive is the
  way round that works.

#### Two things learned about the harness

`ToolsSmoke` grew a `&` prefix (`SystemTags` with `SYS_Asynch` and its own
`NIL:` handles) and a `wait <secs>` line, because a listener and the thing that
connects to it must be running at the same time and `SystemTagList()` waits.
Its redirection also now adds only the half the command has not brought itself
— appending a second `>` made the Shell silently drop one of them.

And `tool_output_write()` calls `Flush()` first. These commands print through
`VPrintf()`, which dos.library buffers, and socket bytes through `Write()`,
which it does not; without the flush an `ftp` transcript comes out with the
directory listing spliced into the middle of the reply that announced it.
Observed exactly that way.

---

## 11. curl on the 68020 (2026-07-25)

Upstream curl, unpatched, cross-built for m68k AmigaOS 3.x and fetching `http://` URLs
through our `bsdsocket.library`. Read from the emulated 14 MHz A1200, `clients/curl/
run-fsuae.sh`:

```
--- SYS:curl --version
curl 8.21.0-DEV (m68k-unknown-amigaos) libcurl/8.21.0-DEV
Protocols: dict file ftp gopher http imap ipfs ipns mqtt pop3 rtsp smtp telnet tftp ws
Features: alt-svc threadsafe
--- rc 0, 0.32 s

--- SYS:curl -sS -o DH0:ex.html -w "..." http://example.com/
example.com: HTTP 200, 559 B, dns 0.98s connect 1.48s total 2.02s
--- rc 0, 2.40 s

--- SYS:curl -sS -o DH0:sdk.lha -w "..." http://ftp.fau.de/aminet/comm/tcp/AmiTCP-SDK-4.3.lha
AmiTCP-SDK-4.3.lha: HTTP 200, 657797 B in 5.60s (117463 B/s)
--- rc 0, 6.00 s
```

The 657,797-byte file is **byte-identical to the host's copy** (SHA-256
`52f664c7…`). A range request answers 206 with exactly 65,536 bytes, a chunked
7 KB page decodes, `https://` is refused with *"Protocol \"https\" is disabled"*,
a redirect from `http:` to `https:` is refused *in the redirect*, and an
unresolvable host is `curl: (6) Could not resolve`. The binary is 899,048 bytes
(781 KB text) and links nothing of ours.

**Throughput in context**: 117 KB/s against the 356 KB/s the data-path work
measured for `recv()` into a buffer. curl copies through more layers and writes
to a directory hard drive, so the gap is expected; it is not a regression in the
stack.

### 11.1 Does curl still build for classic AmigaOS? Yes, and it is CI-covered

This needed checking rather than assuming, because AmigaOS 4 / PPC support and
AmigaOS 3.x / m68k support are different ports and neither implies the other.

- `lib/amigaos.c` has **two** bodies. `#ifdef __amigaos4__` is the PPC one
  (`GetInterface`, `SocketIFace`, `CURLRES_AMIGA`, `gethostbyname_r`); the
  `#elif !defined(USE_AMISSL)` half, headed *"Amiga OS3 specific code"*, is the
  classic one and is one `OpenLibrary("bsdsocket.library", 4)` plus one
  `SocketBaseTags()`. This build uses the second.
- `.github/workflows/non-native.yml` has an `amiga` job that cross-builds m68k on
  **every push**, with both autotools and CMake, against bebbo's amiga-gcc 6.5.0
  and AmiSSL 5.27. Classic AmigaOS is a live target in curl, not a historical one.
- **8.21.0 was chosen over master**, not because master is broken — the OS3 branch
  is identical in both — but because a build harness should pin a release. Nothing
  older is needed: there is no "last version that worked on Amiga" cliff to find.

The upstream CI recipe is `-mcrt=clib2 … -lnet -lm -latomic`. Our toolchain is
newlib and has neither `-mcrt` nor a `libnet`, so everything below is the delta
between that CI job and this one.

### 11.2 What the toolchain does not provide, enumerated by linking

`libcurl.a` builds clean. The `curl` command-line tool does not, and the whole
gap is this list — the complete set of undefined symbols from the first link:

| missing | class | answer |
|---|---|---|
| `stat` `fstat` `mkdir` `unlink` `isatty` `ftruncate` `link` `_gettimeofday` | newlib has no non-underscore wrappers and, for most of these, no implementation either | `clients/compat/amiga_posix.c`, over `dos.library` |
| `__udivdi3` `__umoddi3` `__divdi3` `__moddi3` | zero-byte `libgcc.a` | `src/common/ami_udivdi3.c`, the one copy already in the tree |
| `__ctzdi2` `__popcountdi2` `__floatdidf` `__fixdfdi` `__atomic_exchange_4` | ditto, and further than the stack ever reached | `clients/compat/amiga_libgcc.c` |

Three of those deserve their own note.

**`_fstat` exists and is worse than missing.** `libc.a`'s `lib_a-dummy.o` defines
it as `moveq #0,d0 / rts` — success, with **nothing written to the caller's
struct**. So `S_ISREG(st.st_mode)` reads whatever was on the stack. Our
replacement zeroes the struct, asks `_isatty()` what the descriptor is, and
measures a regular file with `_lseek()`.

**`gettimeofday` is not a nicety.** Without it curl falls back to `time()`, whose
resolution is one second, and every timeout and timer in curl then runs on whole
seconds. Measured, same binary, same host, only this changed: a 559-byte GET of
example.com reported **31 s** and a 404 reported **122 s**. curl's CMake cannot
find it with `check_function_exists()` because the probe links a generated
`main()` with no object of its own, so `CMAKE_C_STANDARD_LIBRARIES` is where our
archive lands and the probe never gets there; `clients/curl/build.sh` sets
`-DHAVE_GETTIMEOFDAY=1` by hand.

**Doubles pull in `mathieeedoubbas.library`, and it is not in ROM.** newlib
implements `__adddf3` and friends by calling that library (`lib_a-__adddf3.o`
references `_MathIeeeDoubBasBase` and nothing else), and the startup opens it
before `main()`. Checked rather than assumed: the Kickstart 3.1 40.68 A1200 image
contains `mathieeesingbas.library` and **no other math library**. Every Workbench
install has `mathieeedoubbas.library` in `LIBS:`, so a real machine is fine and a
bare directory hard drive is not; `clients/curl/run-fsuae.sh` stages one and says
so if it cannot find one. curl cannot avoid doubles — the progress meter,
`--max-time` and `--write-out` all use them — and there is no reason to try.

### 11.3 The toolchain hands `main()` the wrong `argv`

`$AMIGA_TOOLCHAIN_ROOT/m68k-amigaos/lib/crt0.o` calls `main()` like this:

```
    pea      ___argv          ; 4879 xxxxxxxx  -- pushes &__argv
    move.l   ___argc,-(sp)    ; 2f39 xxxxxxxx
    jsr      _main            ; 4eb9 xxxxxxxx
```

`__argv` is a pointer, not an array — crt0.o's entire `.bss` is 16 bytes, holding
`__argv` at 0 and `__savedSp` at 8 — so `pea` pushes its **address** and `main()`
gets one level of indirection too many. Measured, `argvtest alpha beta`:

| | argc | argv[0] | argv[1] | argv[2] |
|---|---|---|---|---|
| stock crt0 | 3 | `<>` | `NULL` | `<>` |
| repaired | 3 | `<SYS:argvtest>` | `<alpha>` | `<beta>` |

The garbage is legible once the cause is known: `argv[0]` is the real argv array,
whose first byte is the high byte of a pointer and therefore zero, so it prints
empty; `argv[1]` is the next `.bss` word, which is 0; `argv[2]` is `__savedSp`, a
stack pointer, which also starts with a zero byte.

**Nothing in AmiNetXDuo had ever noticed**, because every command in this tree
takes its arguments through `ReadArgs()` and reads `argc` only, to tell a
Workbench launch from a Shell one. A ported Unix client reads `argv` and nothing
else, so for curl this is the difference between working and printing nothing.

`clients/compat/fix-crt0.py` swaps two bytes — `pea (xxx).L` and
`move.l (xxx).L,-(sp)` are both six bytes with the same 32-bit absolute operand,
so no offset and no relocation moves — into a **copy in the build directory**. The
installed toolchain is never modified: a build host is not ours to change, and the
next person to fetch the pinned toolchain has to get the same result. The script
matches the whole three-instruction sequence rather than a bare `pea`, patches
both call sites (the Shell one and the Workbench one), and says so loudly if it
finds a number other than two.

### 11.4 Every DNS lookup in this tree was costing 30 seconds

Found while wondering why curl's `time_namelookup` was 30 s. It was not curl.

```
nameserver 10.0.2.2    gethostbyname("example.com")   30.06 s
nameserver 10.0.2.3    gethostbyname("example.com")    0.58 s
```

`DEVS:Internet/name_resolution` in `tests/netstack/devs/` said 10.0.2.2, with a
comment asserting that FS-UAE's SLIRP puts the gateway and the DNS forwarder at
the same address. **It does not.** 10.0.2.2 is the gateway and does not answer
DNS; the forwarder is 10.0.2.3.

The lookups **succeeded either way**, which is why this survived so long: DHCP
supplies a working server as well, so the resolver waits out its failover on
10.0.2.2 and then answers, thirty seconds late. `fetch`, `host`,
`tests/tls/run-fetch.sh`, `tests/tls/run-api.sh` and every conformance DNS case
were all paying it. Measured in one boot, before the fix:

```
--- SYS:fetch http://example.com/ TO DH0:f1.txt      rc 0, 31.16 s
--- SYS:host example.com                             rc 0, 30.08 s
--- SYS:dnsprobe example.com                         rc 0, 30.16 s
```

**No harness in this tree had ever timed a single command**, which is the actual
finding. `ClientRun` now reports elapsed time per line, and that is what turned
"curl is slow" into "everything is slow, and here is the line to change". A
one-line fixture fix; `src/bsdsocket/` is untouched.

What is *not* fixed, and is a stack question rather than a fixture one: **thirty
seconds is a long time to spend deciding a nameserver is not answering.** BIND's
own default is five seconds a try with two retries. Whatever `nx_dns`'s
`wait_option` is set to on our resolve path, a machine whose first configured
nameserver is dead currently stalls every lookup by half a minute — and a real
Amiga with a hand-typed `name_resolution` is exactly where that happens.

### 11.5 The harness

`clients/` is a general *port a Unix network client to m68k AmigaOS* harness, not
a curl-specific one — wget is the next tenant and should need nothing new from
`clients/compat/`.

- `clients/amiga-client.sh` — sourceable. Resolves the toolchain through
  `tools/amiga-toolchain.sh` (so a client build and a stack build never disagree
  about the compiler), builds the support archives, exports the flags.
- `clients/compat/` — the libc, libgcc and crt0 gaps above.
- `clients/curl/build.sh` — CMake, not autotools, because autotools would need
  `autoreconf` on the build host and CMake is already a dependency of this tree.
- `clients/curl/run-fsuae.sh`, `clients/curl/clientrun.c` — the run.
- `third_party/curl` — a submodule pinned to `curl-8_21_0`. **Nothing in curl is
  patched.**

Three flags every such port needs, all of them Roadshow NDK facts rather than
curl facts:

- **`-D__USE_NEW_TIMEVAL__`** — the NDK's `<devices/timer.h>` and newlib both
  define `struct timeval`, incompatibly. The NDK provides this switch itself, in
  as many words, and it is the supported route: define it and AmigaOS uses
  `struct TimeVal` and leaves `struct timeval` to libc.
- **`-D_SYS_MBUF_H`** — `<proto/bsdsocket.h>` reaches `<net/if.h>`, which has
  `struct __timeval ifi_lastchange;` as a **field**, and `struct __timeval` is
  never defined anywhere in the NDK. It is an opaque type the inline stubs use
  behind a pointer. Nothing in a client wants mbufs; the header is suppressed by
  its own guard.
- **`-include sys/types.h`** — the NDK's `<sys/socket.h>` declares
  `recv`/`send`/`sendto` returning `ssize_t` without declaring `ssize_t`. Without
  this, `proto/bsdsocket.h` does not compile at all — which a configure script
  reports as *"AmigaOS bsdsocket.library not found"* and then silently builds a
  client against a libc networking API this toolchain does not have.
  `tests/conformance/build.sh` carries the identical line for the identical
  reason.

Two settings that are curl's own quirks:

- **`-DHAVE_SELECT=1`.** `lib/select.c` is `#if !defined(HAVE_SELECT) &&
  !defined(HAVE_POLL)` → `#error`. curl's probe cannot see `select` here because
  Roadshow has `WaitSelect()` and no `select()` — and `lib/curl_setup.h` then
  `#define`s `select(a,b,c,d,e)` to `WaitSelect(a,b,c,d,e,0)` itself. The function
  is there; the probe does not include `curl_setup.h`. On the clib2 toolchain
  upstream tests against, libc has a real `select()` and the probe passes for the
  wrong reason.
- **`-DENABLE_IPV6=OFF`.** The NDK has `struct sockaddr_in6`, so curl's probe turns
  IPv6 on, and `AMINETXDUO_IPV6` is OFF in the shipping stack. Turn it back on when
  the stack under it was built with it.

**A fabricated `libnet.a` is load-bearing and not cosmetic.** curl's CMakeLists
hardcodes `list(APPEND CURL_NETWORK_AND_TIME_LIBS "net" "m" "atomic")` for AMIGA,
with no switch. `libm.a` is real; the other two we make. `libatomic.a` is empty on
purpose. `libnet.a` holds **one weak `SocketBase`** — because the NDK inlines all
dereference it, and a `check_symbol_exists("IoctlSocket", …)` probe has no
translation unit that defines it, so the probe **links** rather than compiles and
fails. That is how curl silently loses `HAVE_IOCTLSOCKET_CAMEL_FIONBIO`, which is
the only way it knows to make a socket non-blocking on this platform. Fabricating
an archive to satisfy a hardcoded `-l` beats patching a build system that would
have to be re-patched on every version bump.

**A ported client cannot run on a Shell's stack.** Kickstart 3.1 gives a command
4,096 bytes; this toolchain's `crt0.o` exports no `__stack` hook to ask for more
(checked). `ClientRun` starts everything with `NP_StackSize` = 256 KB, `System()`
passing unknown tags through to `CreateNewProc()` being the documented route. A
human at a Shell prompt needs `stack 200000` first — ordinary Amiga practice for a
ported program, and worth putting in the README.

### 11.6 The TLS question: write the backend, do not import a library

**Recommendation: write a curl `vtls` backend over `tls.library`.** Not mbedTLS,
not wolfSSL. The reason is not size — a multi-megabyte curl is explicitly
acceptable here — it is that `src/crypto68k/` is wired into `nx_secure` and a
third-party library would not reach it.

The arithmetic, from the measured per-operation figures earlier in this document
(14 MHz 68020, `tests/crypto68k/crypto68k_ec_bench`):

| per handshake | with `crypto68k` | vendored `nx_crypto` |
|---|---|---|
| ECDHE_RSA, 2-cert chain: 3 × RSA-2048 public + keygen + shared | 3×0.681 + 0.381 + 1.368 = **3.79 s** | 3×2.011 + 1.475 + 5.245 = **12.75 s** |
| ECDHE_ECDSA, 2-cert chain: 3 × P-256 verify + keygen + shared | 3×1.961 + 0.381 + 1.368 = **7.63 s** | 3×7.028 + 1.475 + 5.245 = **27.80 s** |

Against the **measured whole-handshake** totals through `tls.library` — 6.8 s for
the RSA case, 23.3 s for `ecc256.badssl.com` — losing `crypto68k` adds about
**9 seconds** to an RSA chain and **20 seconds** to an ECDSA one. Cloudflare gives
up somewhere between 11.3 s and roughly 20 s. So this is not "slower", it is the
difference between a handshake that completes and one the far end abandons: the
RSA case goes from 6.8 s (comfortable) to ~15.8 s (marginal), and the ECDSA case
from 23.3 s (already failing on Cloudflare, fine on GCP) to ~43 s (failing
everywhere).

**Two honest caveats on those numbers.** The right-hand column is *vendored
`nx_crypto`*, not mbedTLS or wolfSSL, and neither of those is that slow — both
have decent portable bignum, and `crypto68k`'s win came mostly from *algorithmic*
fixes (sliding-window plus leading-zero skipping for RSA; a limb-domain Solinas
reduction for P-256) that a well-maintained library already has. Hand-written
assembly was only 1.13× of it. A fair guess is that wolfSSL lands between the two
columns, perhaps 1.5–2.5× slower than `crypto68k` rather than 3–4×. That is still
the wrong direction across a hard timeout, and it is a guess: **nobody has
measured wolfSSL on this hardware, and if anyone wants to overturn this
recommendation that is the measurement to make.**

**On mbedTLS's `MBEDTLS_*_ALT` hooks specifically**, since they are the obvious
counter-argument: they do not rescue this. `crypto68k`'s P-256 code is written
against `nx_crypto`'s `NX_CRYPTO_EC` structures and its huge-number
representation, and the two things that made it fast — the Solinas reduction
rewritten to work on limbs instead of a byte stream, and the eight-limb
add/subtract carry chains in `c68k_p256.S` — are *representation-specific*.
Retargeting them to `mbedtls_mpi` is a rewrite of the arithmetic layer, not a
recompile, and `MBEDTLS_BIGNUM_ALT` is all-or-nothing. The work is larger than
writing the vtls backend, and at the end of it there would be two TLS
implementations in the tree.

And there is a fourth argument that has nothing to do with speed: `tls.library`
already has the trust store on disk with lazy per-chain root parsing, the
host-name check, the dead-RTC clock rule, and a shipped `fetch` that proves them.
All of that would have to be built again around an imported library.

#### What the backend actually costs

`struct Curl_ssl` (`lib/vtls/vtls_int.h`) is 20 slots and many are NULL-able —
`mbedtls.c` leaves six NULL and uses the shared `Curl_ssl_adjust_pollset`. The
mapping is close to one-to-one:

| curl slot | `tls.library` |
|---|---|
| `do_connect` | `TLSOpen()` |
| `recv_plain` / `send_plain` | `TLSRead()` / `TLSWrite()` |
| `data_pending` | `TLSPending()` |
| `shut_down` / `close` | `TLSClose()` |
| `version` | `TLSInfo()` |
| `adjust_pollset` | `Curl_ssl_adjust_pollset`, shared |
| `random` | nothing exported; return `CURLE_NOT_BUILT_IN` |
| `sha256sum` | needed only for `--pinnedpubkey`; NULL |
| `set_engine*`, `engines_list`, `get_channel_binding`, `cert_status_request`, `close_all` | NULL |

Roughly **600–900 lines**, against 1,638 for `mbedtls.c` (which also does CA
parsing, cipher-list mapping and CRLs, all of which `tls.library` owns).

Four things it is *not* free of, stated in advance:

1. **`TLSOpen()` blocks and curl's SSL filter does not.** `do_connect` is meant to
   be called repeatedly until `*done`. The milestone-1 answer is to put the socket
   back to blocking around `TLSOpen()` (curl has set it non-blocking with
   `IoctlSocket(FIONBIO)` by then) and accept that curl looks stalled for 7–23 s.
   That is precisely the trade this project has already accepted. Doing it
   properly means a state-machine handshake in `tls.library`, which is a bigger
   piece of work than the backend.
2. **No ALPN**, so HTTP/1.1 only. `nghttp2` is not built for m68k anyway.
3. **curl has to be patched after all** — a new `CURLSSLBACKEND_*` value in
   `include/curl/curl.h`, an entry in `vtls.c`'s backend table, `Makefile.inc`,
   `CMakeLists.txt`, `curl_setup.h`. Five small patches, rebased on each pinned
   tag. The "nothing in curl is patched" property of the `http://` build does not
   survive TLS.
4. **No session resumption** — see below, because it is the largest number on the
   table and it is missing at the `nx_secure` level, not the `tls.library` level.

#### Where a verified-chain cache would sit, and what it is worth

It belongs **inside `tls.library`**, in the `nx_secure_remote_certificate_verify`
replacement it already installs — the one that asks each received certificate who
issued it and looks the answer up in the trust-store index. Add a second index,
keyed by a hash of the intermediate's whole DER, of certificates whose signature
has already been checked against a trusted root; on a hit, admit the intermediate
as an issuer without the public-key operation. It wants the same on-disk shape as
`DEVS:Internet/certificates` (`tools/mkcertstore.py` already writes that format)
so it survives a reboot, which is the whole point.

**What it buys is less than it sounds, and the estimate should be written down
before anyone builds it.** `_nx_secure_x509_certificate_chain_verify()` already
stops at the first issuer that comes back from the trusted store, so a public
chain costs **three** public-key operations regardless of depth: the leaf's
signature, the intermediate's signature, and the ServerKeyExchange signature. A
cached intermediate removes exactly **one** of the three:

| | saved | of a measured handshake | share |
|---|---|---|---|
| RSA-2048 chain | 0.681 s | 6.8 s | **10%** |
| ECDSA P-256 chain | 1.961 s | 23.3 s | **8%** |

Eight to ten per cent. It does **not** bring a three-deep chain under Cloudflare's
fifteen seconds, and anyone who expects it to will be disappointed. It is cheap
and it is worth having; it is not the answer.

**The answer, if there is one, is session resumption**, because a resumed
handshake does *no* public-key work at all — 23 s would become well under one
second on the second and every subsequent connection to a host. curl already has
the machinery on its side (`lib/vtls/vtls_scache.c` exists for exactly this).
`nx_secure` does not: `nx_secure_tls_send_clienthello.c:199` sets
`nx_secure_tls_session_id_length = 0` unconditionally, so the TLS 1.2 client
**always offers an empty session ID and never attempts resumption**, and there is
no `session_ticket` extension anywhere in the tree (`nx_secure_tls_process_
newsessionticket.c` is TLS 1.3 only). The ServerHello's session ID *is* stored
(`nx_secure_tls_process_serverhello.c:158`) and then never used. So resumption is
a real piece of work in the vendored library — offering the stored ID, and the
abbreviated-handshake path when the server accepts it — and it is the single
highest-value thing anyone could do to make HTTPS practical on this machine.
Larger than the backend; larger than the chain cache; worth more than both.

#### Order

1. `http://` — **done**, above.
2. The vtls backend over `tls.library`, with a blocking handshake. Puts `https://`
   on two-certificate hosts and on anything with a patient front end.
3. Session resumption in `nx_secure`, then the verified-chain cache. In that
   order, because the first is worth 20× the second.
4. A non-blocking handshake in `tls.library`, if curl's multi interface ever
   matters here.

### 11.7 What wget will need that curl did not

Read from wget git (`bootstrap.conf`, `configure.ac`, `src/ssl.h`, `msdos/`),
not built. Four differences that matter, in the order they will bite:

1. **wget has no AmigaOS awareness at all.** `grep -ril amiga src/ lib/
   configure.ac` finds nothing. curl's `lib/curl_setup.h` knows that a socket is
   not a file descriptor on this platform, that `close()` is `CloseSocket()`,
   that `fcntl()` must not be used on a socket and that `select()` is
   `WaitSelect()`; **wget knows none of it**, and gnulib's `socket`, `connect`,
   `recv`, `send`, `select` and `close` modules assume POSIX descriptors or
   Winsock. Teaching wget the bsdsocket ABI is the port, and it is a larger job
   than everything in §11.2 and §11.3 put together — those were toolchain gaps,
   this is the client's own model of the world.

2. **gnulib, and therefore autotools.** wget's `bootstrap.conf` lists ~110
   modules, including `posix_spawn`, `spawn-pipe`, `pipe-posix`, `sigprocmask`,
   `sigpipe`, `flock`, `futimens`, `symlink`, `group-member`, `getpass-gnu`,
   `iconv`, `regex` and `unicase/u8-tolower`. A git checkout needs `./bootstrap`,
   which needs autoconf, automake, libtool, gettext and a gnulib checkout on the
   build host — exactly what `clients/curl/build.sh` avoids by using CMake. **Use
   a pinned release tarball**, which ships a generated `configure` and the gnulib
   sources in `lib/`. Note also that this toolchain has no `pipe()`, no `fork()`
   and no `posix_spawn()`; several of those modules have gnulib replacements and
   several do not.

   The precedent to copy is in the tree already: `msdos/config.h` +
   `msdos/Makefile.DJ` is a **hand-written config and makefile** for DJGPP that
   skips `configure` entirely, and `vms/` is the same idea again. An
   `amiga/config.h` in that style is very likely the right shape, and it means
   `clients/` grows a second build style rather than reusing curl's.

3. **No non-blocking connect, and no timeout that works.**
   `connect_with_timeout()` bounds a *blocking* `connect()` with
   `run_with_timeout()`, which is `sigsetjmp` + `alarm` — neither of which does
   anything here. A blocking `connect()` to a dead port therefore has to fail
   promptly by itself or wget hangs with nothing of its own to save it. Our
   `connect()`'s own timeout becomes load-bearing in a way it is not for curl.

4. **TLS is *easier* than curl's, which is the one pleasant surprise.** wget has
   no plugin backend — `--with-ssl={gnutls,openssl,no}` and nothing else — but
   the interface those two implement is four functions (`src/ssl.h`):

   ```c
   bool ssl_init (void);
   void ssl_cleanup (void);
   bool ssl_connect_wget (int fd, const char *host, int *continue_session);
   bool ssl_check_certificate (int fd, const char *host);
   ```

   plus one `fd_register_transport()` call to route reads and writes. Against
   `tls.library` that is on the order of 150 lines, against 600–900 for curl's
   20-slot `Curl_ssl` vtable — and `ssl_check_certificate()` is a no-op for us,
   because `TLSOpen()` has already verified the chain and the host name and will
   not return a connection it could not vouch for.

So the order stands: curl first because it already knows this platform, wget
second because it does not.


### 11.8 `curl https://` works (2026-07-25)

The vtls backend of §11.6 is written and `https://` fetches real pages from the
emulated A1200. Read from `clients/curl/run-fsuae.sh`, 14 MHz 68020:

```
--- SYS:curl --version
curl 8.21.0-DEV (m68k-unknown-amigaos) libcurl/8.21.0-DEV tls.library/1.0
Protocols: dict file ftp ftps gopher gophers http https imap imaps ipfs ipns
           mqtt mqtts pop3 pop3s rtsp smtp smtps telnet tftp ws wss
Features: alt-svc HSTS SSL threadsafe

--- SYS:curl -v -sS -o DH0:t12.html https://tls-v1-2.badssl.com:1012/
* SSL connection using TLSv1.2 / AES256-SHA256
* Server certificate chain: 2 certificate(s), verified, handshake 4.99 s
* ALPN: server did not agree on a protocol. Uses default.
> GET / HTTP/1.1
> Host: tls-v1-2.badssl.com:1012
< HTTP/1.1 200 OK
< Server: nginx/1.10.3 (Ubuntu)
< Content-Length: 502
{ [502 bytes data]
--- rc 0, 6.56 s
```

The 502 bytes are byte-identical to the host's copy (SHA-256 `7e93f4f1…`), and
so are `ecc256.badssl.com`'s 684 and — the one that matters for the record
loop — **998,733 bytes of `www.iana.org/assignments/media-types/media-types.xhtml`
over TLS, SHA-256 `f0771af7…`**.

#### Which hosts answer, and where the failures are

`%{time_appconnect}` is the handshake alone. Every row is a real fetch of a
real page; the body sizes are the servers'.

| host | chain | key exchange | 14 MHz | `-k 28` |
|---|---|---|---|---|
| `tls-v1-2.badssl.com:1012` | 2 | RSA, `AES256-SHA256` | **200**, 502 B, 6.14 s | **200**, 3.94 s |
| `ecc256.badssl.com` | 2 | ECDHE-ECDSA | **200**, 684 B, 24.26 s | **200**, 12.38 s |
| `www.iana.org` | 3 | ECDHE-ECDSA, Cloudflare | (35) closed at 23.3 s | **200**, 6,253 B, 12.04 s |
| `example.com` | 4 | Cloudflare | (35) closed at 39.7 s | (35) closed at 19.9 s |
| `wrong.host.badssl.com` | 2 | — | **(60) refused**, 6.1 s | — |

**The two failures are the far end's patience, not ours.** Both are Cloudflare
and both are `curl: (35) the connection is closed` — the peer hanging up while
this machine is still doing arithmetic, which is the same wall §11.6 predicted
and `src/tools/fetch.c` already documents. Note `ecc256.badssl.com` succeeding
at 24.26 s where a Cloudflare host gives up at 20: nginx will wait and a CDN
will not, and that difference is worth more than a second of CPU either way.
`example.com` is the only host that fails at both clocks, and it is the deepest
chain of the five.

#### Certificate verification, proved rather than asserted

```
--- SYS:curl -v -sS -o DH0:wrong.html https://wrong.host.badssl.com/
*   Trying 104.154.89.105:443...
* wrong.host.badssl.com: the certificate is issued to another host
* closing connection #0
curl: (60) wrong.host.badssl.com: the certificate is issued to another host
--- rc 60, 5.52 s

--- SYS:curl -sS -k -o DH0:wrongk.html -w "insecure: HTTP %{http_code}, %{size_download} B\n" https://wrong.host.badssl.com/
insecure: HTTP 200, 500 B
--- rc 0, 5.00 s
```

A backend that succeeded by not checking would be worse than no backend, so
both directions are in the default command list: refused by default with curl's
own exit 60, and connected with `-k` because that is what `-k` is for.
`--cacert` reaches `TLSA_TrustStore` and is tested too.

#### Throughput: TLS costs 7× on the wire

Nobody had measured this. Same machine, same run, `-k 28`:

```
http  ftp.fau.de   657,797 B in  5.74 s = 114,598 B/s
https www.iana.org 998,733 B in 60.68 s =  16,464 B/s   ECDHE-ECDSA-AES128-SHA256

  and the same pair again, later, with the non-blocking read in place:
http  ftp.fau.de   657,797 B in  5.58 s = 117,884 B/s
https www.iana.org 998,733 B in 63.90 s =  15,634 B/s
```

**7.0× to 7.5×**, and it is all symmetric: AES-128-CBC plus HMAC-SHA256 over every byte,
twice (decrypt and authenticate). That is a `crypto68k` question and not a
backend one — the handshake is a fixed cost per connection and this is a cost
per byte, so it is the number that decides whether a 5 MB download over HTTPS is
five minutes or fifty. It is also the reason the 998 KB test exists at all:
without it the backend would have been declared working on three pages of under
1 KB each.

#### What is in the tree

| | |
|---|---|
| `clients/curl/amitls.c` | the backend, 703 lines — ~300 of preamble and 405 of code, against the 600–900 estimated |
| `clients/curl/amitls.h` | the one `extern` |
| `clients/curl/curl-amitls.patch` | **31 added lines over six files** |

The 20-slot `struct Curl_ssl` mapped as §11.6 said it would. Eight slots are
NULL — `shut_down` (`TLSClose()` sends `close_notify` itself), `cert_status_
request`, `close_all`, the three engine slots, `sha256sum` and
`get_channel_binding` — and `adjust_pollset` is curl's shared
`Curl_ssl_adjust_pollset`. What was not foreseen is that `random` cannot be
NULL: with `USE_SSL` defined, `lib/rand.c` routes **every** `Curl_rand()` in
libcurl through the TLS backend, and `Curl_ssl_random()` answers
`CURLE_NOT_BUILT_IN` when the slot is empty.

**The patch is six files rather than five**, and the sixth is the one worth
naming. `_curl_ca_bundle_supported` is a variable each backend's CMake block
sets for itself; a backend that does not set it skips curl's whole "CA
handling" section, and a `-DCURL_CA_BUNDLE=…` then reaches `curl_config.h`
**verbatim** — including the literal string `none`, which libcurl dutifully
tries to open. That failure arrives as `TLS_ERR_TRUSTSTORE`, which reads as
"your trust store is missing" and sent this in the wrong direction for a while.

`clients/curl/build.sh -t` copies the two source files into `lib/vtls/` and
applies the patch; `-R` undoes both. The submodule pin never moves, no curl
source is committed modified, and a build without `-t` is bit-for-bit the
unpatched curl of §11 (`899,048` bytes, unchanged). With TLS it is `939,232`.

#### Three things the design got wrong on paper, and what they actually are

**1. `TLSOpen()` blocks, and the socket's blocking mode is irrelevant.** §11.6
proposed flipping the descriptor back to blocking around the handshake, because
curl has set it non-blocking with `IoctlSocket(FIONBIO)` by then. That is not
needed and the reason is structural: `FIONBIO` sets a flag in
`bsdsocket.library`'s own per-socket state (`src/bsdsocket/options.c:406`) that
only `bsd_recv()` and `bsd_send()` read, and **tls.library never calls them**.
It takes the `NX_TCP_SOCKET` behind the descriptor through the private context
vector and blocks in NetX Duo on its own `wait_option`. So there is no mode to
flip and no window in which the descriptor is wrong.

What the blocking handshake does cost is exactly one thing: for 4 to 24 seconds
curl's event loop is stopped, so the progress meter does not move, `--max-time`
cannot fire and Ctrl-C is not read. `TLSA_Timeout` is set from
`Curl_timeleft_ms()` so a dead peer is still bounded. Making it properly
non-blocking means a state-machine handshake **inside tls.library** — `TLSOpen()`
returning `TLS_PENDING` and a `TLSHandshake()` to pump — which is a larger piece
of work than this whole backend and is worth doing only if curl's multi
interface ever matters here.

**2. Reading needs three questions answered, and asking one of them deadlocks.**
The obvious `recv_plain` polls the socket with a zero timeout and answers
`CURLE_AGAIN` when nothing is readable. It hangs, and the reason is a layer
nobody had looked at: `nx_secure` keeps *undecrypted* bytes of its own in
`nx_secure_record_queue_header` (`nx_secure_tls_session_receive_records.c:106`)
whenever one TCP segment carried more than one TLS record — the ordinary case
for a server that writes headers and body separately. In that state the socket
is not readable, `TLSPending()` is 0 because no plaintext exists yet, and a
complete record is sitting there ready to decrypt. A backend that answered
`CURLE_AGAIN` would wait on a descriptor that will never become readable again.

`TLSBuffered()` now answers that half, so `amitls_recv()` asks all three —
plaintext ready, ciphertext held, bytes on the socket — and only answers
`CURLE_AGAIN` when all three say no. **Reads are therefore non-blocking**:
`--max-time` fires during a transfer, the progress meter moves and Ctrl-C is
read. The handshake is the one place that still stops the world. What is not
removed is a bounded block inside `TLSRead()` when what is buffered turns out
to be half a record, which is the residual `tls.library`'s own documentation
already names for `TLSWaitSelect()`.

The 998 KB transfer measured 16,464 B/s blocking and 15,634 B/s asking, one run
each, against a `http://` control that moved 114,598 → 117,884 B/s between the
same two runs. That is the network, not the extra library call: a few hundred
`TLSBuffered()` calls do not register against sixty seconds of AES.

**3. No ALPN, and curl says so out loud.** `Curl_alpn_set_negotiated(…, NULL, 0)`
prints *"ALPN: server did not agree on a protocol. Uses default."* and curl
uses HTTP/1.1. Nothing is lost — `nghttp2` is not built for m68k.

Connection reuse across requests works and is tested: two URLs on the same host
in one command line report `1 connects` then `0 connects`, the second answering
1,506 bytes over the kept-alive TLS connection with no second handshake.

#### Two vectors this backend asked for, and got

Both landed in `src/tlslib/` while this was being written, and both are wired.

**`TLSRandom(base, buffer, length)`** is the machine's one entropy pool — the
SHA-256 DRBG the session keys come from — and it is what `amitls_random()` asks
now. It answers -1 until a connection has been opened in the calling program,
because the pool lives in `bsdsocket.library` and `tls.library` reaches it
through the link `TLSOpen()` makes; curl calls `Curl_rand()` well before its
first `https://` URL, so a fallback is still there and is still a time-seeded
LCG. What keeps that from mattering: an `http://`-only run never reaches TLS at
all, and once anything has, the real pool is answering. Opening `tls.library`
merely to seed a boundary string would make every `http://` fetch depend on a
library it does not use.

**`TLSBuffered(base, conn)`** is what makes the non-blocking read above
possible. It is deliberately not folded into `TLSPending()` and should not be:
`TLSPending()` promises `TLSRead()` will not block and this one does not.

**One hazard they arrived with.** `TLS_LIB_VERSION` is still 1 and
`TLS_LIB_REVISION` still 0, so `OpenLibrary("tls.library", 1)` happily returns a
library that predates both vectors — and calling one would jump past the
`(APTR)-1` terminator `MakeLibrary()` stopped at, on a machine with no memory
protection. `amitls_open_library()` compares `lib_NegSize` against the LVO and
falls back to the blocking read and the LCG when they are missing, which is what
`src/tlslib/tls_netx.c` already does against `bsdsocket.library`'s private
vector. A revision bump would let a caller say `OpenLibrary(…, 1)` and mean it.

#### Session resumption: nothing to do, and it works

The concurrent work in `src/tlslib/` landed while this was being written, and
the API it chose is **no API**: `tls.library` resumes by itself, keyed on
`TLSA_HostName`, with the cache in the library and mirrored to
`DEVS:Internet/tlssessions`. So the backend adopted it by being rebuilt. The
only line of curl that knows about it is the one that reports `ti_Resumed`.

Three `curl` invocations, three separate processes, 14 MHz:

```
--- SYS:curl -v -sS -o DH0:a1.html -w "1st: HTTP %{http_code}, handshake %{time_appconnect}s\n" https://tls-v1-2.badssl.com:1012/
* SSL connection using TLSv1.2 / AES256-SHA256
* Server certificate chain: 2 certificate(s), verified, handshake 5.10 s
1st: HTTP 200, handshake 6.000000s
--- rc 0, 6.66 s

--- SYS:curl ... (same URL again)
* Resumed a cached session: no certificate sent, no signature verified, handshake 0.62 s
2nd: HTTP 200, handshake 1.620000s
--- rc 0, 2.24 s

--- SYS:curl ... (and again)
* Resumed a cached session: no certificate sent, no signature verified, handshake 0.62 s
3rd: HTTP 200, handshake 1.620000s
--- rc 0, 2.26 s
```

**5.10 s to 0.62 s**, and it crosses process boundaries because the cache is the
library's rather than curl's — which is why `lib/vtls/vtls_scache.c` is compiled
in and unused, and should stay that way. A per-process cache would have helped
nobody here: the case that matters on this machine is running the same command
twice.

`TLSA_NoResume` is deliberately not wired to a curl option. curl has no switch
that means it — `--no-sessionid` turns off *curl's* cache, not the library's —
and inventing one would be a patch to curl for something nobody asked for.

**The one thing that did not resume, chased down.** During development a
second connection to `ecc256.badssl.com` failed with `(35) the connection is
closed` after **61.6 s**, against 24.9 s for the full handshake before it. The
standing hypothesis was the header bug that `1ceb741` fixed — `a0`/`a1` were
listed as inputs only in the inline stubs, so the compiler assumed they survived
a call and a second call could be made on a stale pointer.

**It is not that, and the way to know is cheap.** Build the backend twice
against the same `tls.library`, once with the fixed header and once with only
the `"+r"` constraints reverted, and compare the objects:

```
build/curl-tls/…/vtls/amitls.c.obj      6216 bytes
build/curl-oldabi/…/vtls/amitls.c.obj   6216 bytes   IDENTICAL
```

Byte for byte, at `-O2`. The bug needs two stub calls close enough together for
the compiler to keep the register live across both, which is what
`src/tools/fetch.c` does — `io_write()` then `io_read()` through one small
struct in one function. This backend loads `backend->conn` from memory in each
of `amitls_recv`, `amitls_send`, `amitls_close` and `amitls_data_pending`, so
there was never a load to eliminate. The generated code was correct by accident
of shape, not by the constraint being right.

So the 61.6 s remains unexplained, and the most likely answer is the dullest:
it ran against a `tls.library` built from an uncommitted working tree, mid
development, which has never existed as a commit. Against `e42db07` the case is
clean — `ecc256.badssl.com` three times in three separate processes, 14 MHz:
**23.27 s, then 0.58 s, then 0.58 s**, all three HTTP 200 with the same 684
bytes.

**The tables above are first connections**, which is what a host still costs
before it is in the cache. They were first measured against the `tls.library` at
`f535728` and re-measured against `e42db07` with the non-blocking read in place;
nothing moved by more than a tick — `tls-v1-2` 6.18 → 5.78 s, `ecc256` 24.42 →
24.38 s, `www.iana.org` and `example.com` still closed on by the CDN at 23.5 and
39.5 s, `wrong.host.badssl.com` still refused.


## 12. Conformance, named — and the client access patterns behind it (2026-07-25)

The loopback tier reads **125 passed, 1 failed, 16 skipped**. A count is not a work list,
so here is every one of the 17 with its name, its cause, and a classification that does
not flatter us:

- **(a)** a real gap a client could hit
- **(b)** a divergence we stand behind
- **(c)** an artefact of the suite or the environment
- **(d)** out of scope

| # | Name | Class | Why |
|---|---|---|---|
| 3 | `socket(): create SOCK_RAW (ICMP)` — **failed** | **(a)** + (b) | We do not implement `SOCK_RAW` at all. The *red* is (b): `test_socket.c:52` skips only on `EACCES`, and `EACCES` means "you lack privilege" on an OS with no privilege model, so we answer `ESOCKTNOSUPPORT` and the test stays red. The *absence* is (a). |
| 27 | `recv(MSG_OOB): urgent data delivery` | **(a)** | NetX Duo's TCP has no urgent pointer: no `URG` on transmit, none parsed on receive. `ftp`'s `ABOR` and `telnet`'s interrupt both send `IAC IP` as urgent data. |
| 64 | `WaitSelect(): exceptfds detects OOB data` | **(a)** | Same root as 27. Skipped because the `send(MSG_OOB)` that sets it up already fails. |
| 39–42 | `tcp_network_64k`, `udp_network_datagram`, `accept_external`, `tcp_network_large` | **(c)** | `helper_is_connected()` is false on the loopback tier by construction. 39, 40 and 42 pass on the network tier. |
| 103–104 | `gethostbyname_external`, `gethostbyaddr_external` | **(c)** | Same gate; both pass on the network tier. |
| 132–136 | `icmp_loopback`, `icmp_network`, `icmp_large_payload`, `icmp_multi_ping`, `icmp_timeout` | **(a)** | All five skip on the same `socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)` that fails as test 3. |
| 138, 140, 142 | `tp_tcp_network`, `tp_udp_network`, `tp_tcp_sustained_network` | **(c)** | Helper gate; all three pass on the network tier. |

Nothing here is (d). The `ipf_*` family that §3.2 calls Tier 4 is not tested by this suite
at all, so it never appears in the 17.

**The network tier** is **133 passed, 2 failed, 7 skipped**. It differs from the loopback
tier by exactly the nine helper-gated tests: eight of them turn green, and the ninth,
**41 `accept(): incoming connection from remote host`**, turns *red* rather than green.
That one is **(c)**, and the evidence is on the host side rather than the Amiga side: the
suite asks the helper to connect back to the Amiga on port 7861, and the helper log says
`CONNECT to 127.0.0.1:7861 failed: [Errno 61] Connection refused`. Under FS-UAE's SLIRP
the guest has no inbound path unless one is opened explicitly, and the obvious lever does
not work — `uae_slirp_ports = 7861` reaches the config (it is in the FS-UAE log) and
changes nothing; FS-UAE 3.2.35's inbound mode appears to be the fixed `21-23,80` set, and
7861 cannot be moved into it because the suite derives that port as `base + 161`.
The capability itself is verified elsewhere, on loopback, by `tests/clients` groups D, E,
I and M — see below.

### 12.1 What curl actually calls

Read from curl 8.22.0-DEV (`lib/curl_setup.h`, `lib/amigaos.c`, `lib/cf-socket.c`,
`lib/select.c`, `lib/socketpair.c`, `lib/curlx/nonblock.c`, `CMakeLists.txt`). curl's
AmigaOS awareness is real but thinner than it looks: most of `lib/amigaos.c` is OS4-only,
and the OS3 path is one `OpenLibrary` plus one `SocketBaseTags`.

- `OpenLibrary("bsdsocket.library", 4)`, then
  `SocketBaseTags(SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(errno))), &errno,
  SBTM_SETVAL(SBTC_LOGTAGPTR), "curl", TAG_DONE)`. **Any nonzero return is fatal** —
  curl reports "SocketBaseTags ERROR" and refuses to run. This one call gates the port.
- `select()` is `#define`d to `WaitSelect(a, b, c, d, e, 0)` (`curl_setup.h:461`).
  `HAVE_POLL` is off, so every wait in curl goes through it, with `FD_SETSIZE` = 64 from
  this toolchain's `sys/select.h` — which happens to equal our `BSD_DEFAULT_DTABLESIZE`.
- `HAVE_FCNTL` is explicitly undefined for the bsdsocket build, so non-blocking mode is
  `IoctlSocket(sockfd, FIONBIO, (char *)&flags)` with a `long`.
- Non-blocking connect, then writable, then `getsockopt(SOL_SOCKET, SO_ERROR)`
  (`verifyconnect()`, `cf-socket.c:913`), treating `0` and `EISCONN` as success.
- `setsockopt`: `TCP_NODELAY` (default on), `SO_KEEPALIVE` and the `TCP_KEEP*` family
  (all `#ifdef`-guarded, and the `TCP_KEEP*` names do not exist in this NDK), `SO_SNDBUF`,
  `SO_REUSEADDR`. `SO_NOSIGPIPE` and `SO_BINDTODEVICE` are guarded out.
- `HAVE_GETADDRINFO` is forced to 0 for Amiga by curl's own CMake, so **curl resolves with
  `gethostbyname`**, not `getaddrinfo`.
- **The one that is not obvious**: `lib/socketpair.c`. With no `AF_UNIX`, curl builds its
  multi-handle wakeup pair out of `socket` + `setsockopt(SO_REUSEADDR)` +
  `bind(127.0.0.1, 0)` + `getsockname` + `listen(1)` + `connect` + non-blocking
  `accept(listener, NULL, NULL)` + a nonce round trip. So **curl exercises the server side
  of the ABI on every multi handle**, `accept()` with two NULLs included. If `HAVE_PIPE`
  is detected instead, curl uses `pipe()` — and a newlib pipe descriptor is not a
  bsdsocket descriptor, so it would end up inside a `WaitSelect()` fd set and nothing
  would ever wake. Checked rather than assumed: this toolchain **declares** `pipe()` in
  `sys-include/sys/unistd.h:182` but does not **link** it (`undefined reference to
  'pipe'`), and curl detects it with `check_function_exists`, which links — so
  `HAVE_PIPE` comes out 0 and `wakeup_inet` is chosen. That is luck, not design: a port
  that pre-fills the feature cache or sets `HAVE_PIPE` by hand breaks curl's multi
  interface in a way that looks like a hang rather than an error.

`wget` 1.25.0 (`src/connect.c`) demands a slightly different set, and in one respect a
harder one: `connect_with_timeout()` bounds a **blocking** connect with
`run_with_timeout()`, which needs `sigsetjmp` + `alarm` and therefore does nothing on
AmigaOS. wget has no non-blocking connect path at all, so a blocking `connect()` to a
dead port must fail promptly by itself or wget hangs with no timeout of its own to save
it. wget also uses `bind_local()` + `accept_connection()` for FTP active mode — the same
`socket`/`SO_REUSEADDR`/`bind`/`getsockname`/`listen`/`select`/`accept` sequence, this
time with a real `sockaddr` out of `accept()` — and `getaddrinfo` when built with IPv6.

### 12.2 `tests/clients` — the sequences, not the vectors

`tests/clients/client_patterns.c` replays those sequences: an ordinary AmigaOS program,
linked against none of our code, with sixteen groups each named after the program and file
it came from. It found three defects that 142 per-vector conformance tests did not,
because none of them is reachable one vector at a time.

- **`shutdown(SHUT_WR)` was sending a RESET.** `nx_tcp_socket_disconnect()` cannot express
  a half-close in either mode: with `NX_NO_WAIT` it resets
  (`nx_tcp_socket_disconnect.c`, the `!NX_DISABLE_RESET_DISCONNECT` branch), and with a
  wait option it sends a FIN and then calls `_nx_tcp_socket_block_cleanup()` when the peer
  does not also close. Either way the direction the caller asked to *keep* died: after the
  half-close the peer's `send()` failed with `EPIPE` and its queued data went in the bin.
  The state machine itself is fine with it — `nx_tcp_socket_packet_process.c` runs
  `_nx_tcp_socket_state_data_check()` in both `FIN_WAIT_1` and `FIN_WAIT_2` — so
  `bsd_tcp_send_fin()` open-codes the graceful branch and stops before the suspension and
  the cleanup. This is `nc -N`, it is `telnet`, and it is every ftp data connection.
- **Ten simultaneous listeners, and the eleventh failed with `ENOBUFS`.**
  `NX_MAX_LISTEN_REQUESTS` was NetX Duo's default of 10 and is a hard ceiling on
  `listen()`: `nx_tcp_server_socket_listen()` returns `NX_MAX_LISTEN` and closing other
  kinds of socket does not help. `ssh -L` wants one listener per forward and an ftp client
  one per active-mode transfer. Now 32, which costs under a kilobyte inside the single
  `NX_IP`.
- **A half-closed socket then reported itself readable**, forever, with nothing to read —
  a defect the half-close fix created and the same test caught in the next run.
  `bsd_readable()` tested `nx_tcp_socket_state >= NX_TCP_CLOSE_WAIT` to mean "the peer's
  FIN arrived before the disconnect callback was attached", and NetX Duo numbers the
  states `CLOSE_WAIT` 6, `FIN_WAIT_1` 7, `FIN_WAIT_2` 8 — so `>=` also caught the two
  that mean *we* sent the FIN. `nc -N` half-closes and then selects for the rest of the
  answer, and would have spun at full speed on a `select()` that returned at once and a
  `recv()` that returned `EWOULDBLOCK`. The states are named individually now, and the
  fix was confirmed in both directions: the new check fails against the old `>=` and
  passes against the named list.

Everything else curl and wget need was already right, and is now pinned: the
`SocketBaseTags` init gate, errno mirrored into the caller's own `int` at the caller's own
width, non-blocking connect to both a live and a dead port with `SO_ERROR` read once and
cleared, curl's `wakeup_inet` socketpair step for step, a control connection surviving
three rounds of active-mode data connections, `WaitSelect()` over a wide sparse set
returning exactly the right count and exactly the right bits, 64 rounds of
connect/accept/transfer/close with descriptors recycling rather than accumulating,
`send()` after the peer has gone failing rather than hanging, wget's blocking connect
failing with `ECONNREFUSED` rather than a timeout, a descriptor table raised to 128 with
`WaitSelect()` past the old 64, `FIONREAD` before and after a partial read, and four
listen/accept/close cycles on one port.

It also covers `getaddrinfo`, `getnameinfo`, `freeaddrinfo` and `gai_strerror`, which
**nothing in the tree called in the default build**: they live at `0x324`–`0x336`, in the
Roadshow tail past where the FD that generated
`tests/conformance/compat/inline/bsdsocket.h` stops, so bsdsocktest cannot reach them and
the only other coverage was the IPv6-only socket test. All twelve of those checks passed
as they stood — coverage, not a fix.

The whole file is **94 checks, 0 failures** on A1200/68020, and both conformance tiers are
unchanged by the three fixes: **125/1/16** on loopback, **133/2/7** on the network tier,
with the same names in each list as before.

### 12.3 What is left, in the order it matters

1. **`SOCK_RAW`** (test 3 plus skips 132–136 — six results, and the only red on the
   loopback tier). NetX Duo can do it: `nx_ip_raw_packet_enable()`,
   `nx_ip_raw_packet_send()` and `nx_ip_raw_packet_filter_set()` are all there. Two things
   make it more than a wrapper. `ICMP` is dispatched to `nx_ip_icmp_packet_receive` before
   the raw path is consulted, so reaching it needs `NX_ENABLE_IP_RAW_PACKET_ALL_STACK` —
   which puts our filter callback on **every** inbound IP packet, TCP included. And when a
   filter is installed, `_nx_ip_raw_packet_processing()` stops queueing entirely
   (`nx_ip_raw_packet_processing.c`) and expects the filter to consume the packet, so
   `nx_ip_raw_packet_receive()` becomes unusable and bsdsocket has to own the queue,
   the per-protocol demux and the wakeup. Nothing on the target tool list needs it —
   `ping` already works through `nx_icmp_ping()` — but `traceroute` and any third-party
   `ping` port do.
2. **`MSG_OOB`** (skips 27 and 64), plus `SIOCATMARK`, which `IoctlSocket()` currently
   answers with `ENOSYS`. This is not a bsdsocket change: NetX Duo's TCP neither sets the
   `URG` bit nor parses it, so the work is in the TCP core and on the hot receive path.
   `ftp`'s `ABOR` and `telnet`'s interrupt are the callers; both degrade to the inline
   copy of the command that the protocols send anyway, which is why this is second and
   not first.
3. **`CloseSocket()` on a connected TCP socket also resets** rather than finishing the
   connection, for a reason the half-close fix does not remove: `nx_tcp_socket_delete()`
   requires `CLOSED`, and a graceful close leaves the socket in `FIN_WAIT_1`, so going
   graceful here needs a deferred-reap list or it leaks an `AmiSocket` per connection.
   Group N of `tests/clients` writes a whole response and closes without the peer having
   read a byte, and every byte still arrives — but that is loopback, where everything is
   acknowledged before the close, so **this is a risk that has not been reproduced, not a
   defect that has been ruled out**. On a slow link with a large final write it would
   truncate.


## 13. Session resumption: 23 s becomes 0.6 s (2026-07-25)

`https://` on a 14 MHz 68020 was never limited by anything we control. A full TLS 1.2
handshake costs **6.8 s** for a two-certificate RSA chain and **23.3 s** for a
two-certificate ECDSA one, essentially all of it public-key arithmetic, and Cloudflare
abandons a handshake somewhere between 11 and 20 seconds. Raising the clock fixes it and
we cannot raise the clock.

**A resumed handshake does none of that arithmetic.** Measured on the A1200 profile,
cold and resumed back to back in one process with identical instrumentation
(`tests/tls/tls_resume`, 28 checks, 0 failures):

| host | chain | cold | resumed | ratio |
|---|---|---|---|---|
| `tls-v1-2.badssl.com` | 2, RSA, 0xC027 | **6,807 ms** | **596 ms** | 11.4× |
| `ecc256.badssl.com` | 2, ECDSA, 0xC023 | **23,419 ms** | **595 ms** | 39.4× |

The resumed figure is essentially constant, because what is left is one round trip, four
PRF invocations and two hashes. It does not depend on the chain, which is the whole
point: the ECDSA case, the expensive one, is the one that gains most.

**And the headline. `www.iana.org` is three certificates behind Cloudflare and does not
complete a cold handshake at 14 MHz — the front end gives up first.** Seeded once at
`-k 28` (11.2 s), then the machine REBOOTED and only the 436-byte session file carried
across:

```
===== SYS:fetch https://www.iana.org/ TO DH0:warm.txt =====
www.iana.org: TLS 0x303, ciphersuite 0xC023, 0 certificate(s), 0.5 s (resumed session)
  chain verified, validity dates checked
----- rc 0 -----
```

**0.5 s, at the A1200's own 14 MHz, on a host that cannot be reached cold at that clock.**
That is the difference between `https://` working and not.

### 13.1 Tickets, not session IDs, and the measurement that decided it

Both mechanisms were probed before anything was written, against the four hosts this
library is developed against — ten trials each, first connection then immediate second,
`openssl s_client` with `-no_ticket` for the session-ID arm:

| host | session ID | ticket |
|---|---|---|
| `www.iana.org` | **2/10** | **10/10** |
| `tls-v1-2.badssl.com` | 0/10 | 10/10 |
| `ecc256.badssl.com` | 0/10 | 10/10 |
| `example.com` | 0/10 | 10/10 |

Not a close call. A modern front end is a farm of machines behind one address; a session
ID is state on **one** of them and a ticket is stateless, so every machine in the farm can
decrypt it. The two hits on iana are the second connection happening to land on the same
edge node. **The probe method is sound and was controlled** — the same script against a
local `openssl s_server -no_ticket` resumes every time, which is what says the 0/10s are
the servers' answer and not the script's bug.

So: **RFC 5077 tickets.** Session IDs come along free, because the acceptance signal for a
ticket is the server echoing back the session ID the client offered, and offering the
previous session's ID is what RFC 5077 §3.4 asks for anyway. That is where those two iana
hits go, and it is what makes this work against an intranet server that has never heard of
tickets.

**One risk was checked before building rather than after.** RFC 7627's
`extended_master_secret` extension: `nx_secure` does not implement it, so every session
this library creates is a non-EMS session, and RFC 7627 §5.3 says a server SHOULD refuse
to resume one abbreviated. Measured with `openssl s_client -no_ems`: **5/5 resumed on all
four hosts.** Nobody enforces the SHOULD. Had they, this work would have needed EMS first
and that is a different piece of work entirely.

### 13.2 No vendored source was touched, and `--wrap` is why

`nx_secure` has no resumption of any kind, and the previous agent's three findings all
hold up:

- `nx_secure_tls_send_clienthello.c:199` sets `nx_secure_tls_session_id_length = 0`
  unconditionally, with a comment saying session resumption is not implemented;
- there is no `session_ticket` extension anywhere in the tree — not even a constant for
  0x0023; `nx_secure_tls_process_newsessionticket.c` is TLS 1.3 only;
- `nx_secure_tls_process_serverhello.c:158` stores the ServerHello's session ID and
  nothing ever reads it.

A fourth, found here and worse than any of them: **`_nx_secure_tls_client_handshake()`
has no case for a NewSessionTicket**, and its `switch` leaves `status` at
`NX_SECURE_TLS_HANDSHAKE_FAILURE`, so merely *asking* for a ticket breaks every full
handshake. Any implementation of this has to change that function.

The project's established pattern for vendored behaviour is to define the symbol in our
own archive and let the linker resolve it there. That works here and costs more than it
should: replacing `_nx_secure_tls_client_handshake()` outright means **copying 450 lines
of state machine that have nothing to do with resumption**, and re-merging every one of
them by hand at each submodule bump.

**So this uses the linker's other tool for the same job.**
`-Wl,--wrap=_nx_secure_tls_client_handshake` sends every call from every other object to
`__wrap__nx_secure_tls_client_handshake()` in `src/tlslib/tls_resume.c`, and leaves
`__real__nx_secure_tls_client_handshake()` pointing at the vendored function — still
linked, still doing all the work it always did. Two functions are wrapped and nothing
else. Verified on this toolchain **before** anything was written, with a three-object test
where the caller is a separate archive member: it disassembles to `jsr ___wrap_vendored`.
Verified again on the shipping binary — `_nx_secure_tls_process_record` calls
`___wrap__nx_secure_tls_client_handshake`, and the wrapper calls `__nx_secure_tls_client_handshake`.

The flag is on `tls.library`'s link **only**. `tls_handshake`, `tls_https` and `tls_bench`
link the same archives without it, so their baselines are untouched by construction rather
than by testing — `tls_handshake` is still 44/0.

**Carrying this across a submodule bump.** The `ClientHello` interception is a splice into
the finished message and reads only TLS wire format — RFC 5246's field order — so a
vendored rewrite cannot silently invalidate it; a shape it does not recognise makes it
leave the message alone and lose resumption rather than corrupt anything. The handshake
interception is the exposure: it calls seven `_nx_secure_*` entry points and depends on
`_nx_secure_tls_process_changecipherspec()` demanding `SERVERHELLO_DONE` and on
`_nx_secure_tls_process_finished()` refusing a peer that sent no credentials. If either
changes, resumption fails closed (the handshake errors) rather than silently, because the
tests assert `ti_Resumed` and not just success.

### 13.3 Three handshake messages, and what each needed

**ServerHello.** Handed to the vendored function unchanged; afterwards its echoed session
ID is compared against the one offered. Equal and non-empty means the server resumed. Then
the cached master secret goes into the key material and the record keys are derived from
it through `nx_secure_generate_session_keys` — a *session function pointer*, so a caller
that replaced it keeps its replacement, and `_nx_secure_tls_generate_keys()` never has to
be reimplemented. Two pieces of state have to be set that the vendored code has no path
to: `nx_secure_tls_received_remote_credentials`, because `_nx_secure_tls_process_finished()`
refuses a Finished from a peer that sent no certificate and a resumed handshake sends none
*by design*; and `client_state = SERVERHELLO_DONE`, because
`_nx_secure_tls_process_changecipherspec()` rejects a client CCS in any other state and
the server's CCS is the very next thing on the wire.

**NewSessionTicket.** Hashed into the transcript (RFC 5077 §3.3 — it counts) and kept.

**Finished, on a resumed handshake only.** The abbreviated handshake reverses the order:
the server finishes first. So the server's Finished has to go *into* the transcript before
ours is generated, and our ChangeCipherSpec and Finished have to be sent afterwards —
neither of which the vendored state machine does. It also **destroys the SHA-256 handshake
hash context the instant it processes a Finished**, which is exactly the context our own
Finished needs, so this message cannot be delegated at all.

Everything else goes to the vendored function **in the record boundaries it would have
seen**. That is deliberate and not tidiness: the record carrying ServerHello, Certificate,
ServerKeyExchange and ServerHelloDone together is passed whole, with the same
`data_length`, because that length is what `_nx_secure_tls_process_remote_certificate()`
bounds itself with. Only a record containing a message the vendored code cannot handle is
taken apart, and such a record never contains a certificate.

### 13.4 The bug that cost a day: a server issuing a ticket sends an EMPTY session ID

The first working build resumed nothing, and the reason is worth writing down because it
is not in any summary of RFC 5077 and it is invisible without a wire trace.

A serial trace through the wrapper said it in one line:

```
[resume] stored tls-v1-2.badssl.com: sid 0 ticket 192 suite C027
[resume] offering tls-v1-2.badssl.com: sid 0 ticket 192
[resume] msg type 2 len 45          <- a resumed ServerHello: no certificate follows
[resume] serverhello: echoed sid 0, offered 0
```

**The server WAS resuming.** A 45-byte ServerHello with nothing after it is an abbreviated
handshake. But a server that issues a ticket returns an **empty** session ID — RFC 5077
§3.4, and nginx does exactly that — so the session recorded from the first handshake had
no session ID at all, there was nothing to echo, and the only acceptance signal a TLS 1.2
client gets could never fire. The client then walked into the ChangeCipherSpec with no
keys derived and failed.

`openssl s_client` hides this, which is why the pre-build probe did not catch it: it
reports a `Session-ID:` for a ticket session because it **fabricates one** for its own
cache. The bytes on the wire have none.

RFC 5077 §3.4 provides for exactly this — a client presenting a ticket MAY generate a
session ID and a server accepting the ticket MUST echo it. So the client now offers **32
random bytes from the machine's own pool** when the cached session has no session ID of
its own. Per attempt, never cached: it is a correlation handle, not a secret, and reusing
one would let an observer link two connections.

### 13.5 A public-header bug that had nothing to do with resumption

Found chasing a second symptom and worth more than the feature that uncovered it.

`tests/tls/tls_resume` completed a handshake, reported 6.8 s, and then `TLSRead()`
returned **-1 with no error set** — a combination the library cannot produce. It happened
on every connection including `TLSA_NoResume`, i.e. on a code path byte-identical to the
one before this work. `tests/tls/tls_api` did the same thing and worked.

The disassembly:

```
620:  moveal a5,a0            ; a0 = the connection
622:  lea    10,a1            ; a1 = the request
62a:  jsr    a6@(-48)         ; TLSWrite
630:  bnew   ...
634:  lea    1e0,a1           ; a1 = the read buffer
640:  jsr    a6@(-42)         ; TLSRead -- and a0 was never reloaded
```

**`d0`, `d1`, `a0` and `a1` are SCRATCH registers in the AmigaOS ABI.** The inline stubs
in `include/aminetxduo/tlslib.h` listed `a0` and `a1` as *inputs only*, which tells the
compiler the opposite, and the compiler believed it: two calls in a row got the arguments
loaded once, and the second ran on whatever the first left behind. `TLSRead()` was
dereferencing a stale pointer and taking its own `conn == NULL` branch.

`tls_api` escaped it by accident — different register pressure, so GCC reloaded anyway.
Fixed by marking them `"+r"`; the reload appears in the disassembly, and `tls_api` went
from 24 checks/1 failure back to **26/0**.

**It looked like the explanation for the curl backend's one-off `ecc256` failure and it is
not.** That agent rebuilt with and without the `"+r"` constraints and got **byte-identical
object files**, because its stub callers reload the connection pointer from memory every
time, so there was no load for the compiler to eliminate. The fix is still right — that
code was correct by the accident of its shape rather than by the constraint that should
have guaranteed it — but it explains nothing about the 61.6 s. See §13.8.

Two things this says beyond the fix. A hand-written inline stub is ABI-critical code and
should be read as such — the register assignment appears in exactly two places
(`tlslib.h` and `src/tlslib/tls_vectors.h`) and only one of them was wrong. And a
symptom that says "the library is broken" while the library's own tests pass is usually
the caller.

### 13.6 Where the cache lives, and its security properties in one paragraph

**Both the library base and a file, for two different reasons.** The base, because a
shared library on AmigaOS outlives the programs that open it: `fetch` runs, exits, and
`tls.library` stays in memory. That already answers the case that matters — somebody
running curl twice — with no disk access at all, which on a floppy machine is worth
having. The file, `DEVS:Internet/tlssessions` beside the trust store, because "stays in
memory" is not a guarantee: `AllocMem()` failure or `avail flush` expunges the library,
and so does a reboot. It is read once per library lifetime and written only when the cache
changes, which is once per full handshake against a handshake that just spent seconds on
arithmetic. Eight entries, fixed 420-byte records, 3,376 bytes at most; the whole file for
one host is 436 bytes. **Shared between programs, deliberately** — a ticket `fetch`
obtained is a ticket curl can use, against the same server on behalf of the same user.

Lifetime and invalidation: the server's ticket lifetime hint, clamped to 24 hours (iana
says 64,800 s, badssl 300). A machine with no clock cannot age anything and therefore does
not try — a stale ticket costs one round trip and a full handshake, which is what would
have happened anyway. An entry is replaced when a new session for the same host arrives,
evicted LRU when the table is full, and **evicted immediately when a handshake that
offered it failed**, so a broken entry cannot make a host permanently unreachable.

Entries are keyed by host, port **and whether the chain was verified**. That last one is
not fussiness: without it a program that used `TLSA_NoVerify` to reach a printer would
poison the cache for every program that came afterwards, and resumption skips
verification by construction.

**The security properties, stated and then left alone.** Each entry holds a 48-byte TLS
master secret and a session ticket in the clear. Anyone who can read the library's memory
can decrypt any session resumed from it — and this is a machine with no memory protection
where every task can already read every other task's memory. Anyone who can read
`DEVS:Internet/tlssessions` can do the same, offline, until the entry ages out; the file
is exactly as sensitive as the sessions it stands for and is not protected, because on
AmigaOS there is nothing to protect it with. What is specifically given up is forward
secrecy for the resumed sessions: an attacker who takes the file can decrypt captured
traffic from them, which the ECDHE full handshake would not have allowed. That is the
price, it is the price every TLS session cache has paid since 1996, and this stack exists
so a classic Amiga can read the web. `TLSA_NoResume` turns it off; `TLSA_SessionFile` with
an empty string keeps the cache in RAM and off the disk.

### 13.7 The API did not change, and that was the design

`TLSOpen()` resumes when it can. There is no tag to ask for it, no call to make first, and
a program written against the previous header gets resumption by being relinked. The curl
vtls backend adopted it that way — rebuilt, not restructured.

Three additions, all optional:

| | |
|---|---|
| `TLSA_NoResume` (BOOL) | do not offer a cached session and do not remember this one |
| `TLSA_SessionFile` (STRPTR) | mirror somewhere other than `DEVS:Internet/tlssessions`; an empty string means RAM only |
| `TLSInfo()` | gains `ti_Resumed`, `ti_Resumable`, `ti_SessionsCached` (library version 2) |

`struct TLSInfo` grew, and `ti_Size` is what makes that safe: a caller compiled against the
older header passes the older size and gets every field it knows about. `TLS_INFO_SIZE_V1`
is 40 — **not 44, because `BOOL` on classic AmigaOS is a SHORT** — and the library asserts
that number against the real offset at build time rather than trusting the arithmetic.

Two more vectors were added for the curl backend, neither related to resumption. **Both
are library version 2**, and a program calling either must open with 2 — see the version
note below, which is where that nearly went wrong:

- **`LONG TLSRandom(struct Library *base, APTR buffer, LONG length)`**, LVO **-78**,
  `a0` = buffer, `d0` = length, `a6` = base, returns bytes written or -1. Puts the
  machine's one entropy pool — the SHA-256 hash DRBG `bsdsocket.library` seeds, the same
  generator the session keys come from — behind a published vector, so a ported client
  does not seed an LCG off the clock. It answers -1 until a connection has been opened in
  that program, because the pool is reached through the link `TLSOpen()` establishes;
  §9's assessment of how little entropy the seed carries applies unchanged and this call
  does not pretend otherwise.
- **`LONG TLSBuffered(struct Library *base, struct TLSConnection *conn)`**, LVO **-84**,
  `a0` = connection, `a6` = base, returns undecrypted bytes held or -1. `TLSPending()`
  answers "is plaintext ready"; this answers the other half. `nx_secure` keeps
  *undecrypted* bytes in `nx_secure_record_queue_header` whenever one TCP segment carried
  more than one TLS record — the ordinary case — and in that state the socket is not
  readable, `TLSPending()` is 0, and a whole record is sitting in memory. Non-zero means
  `TLSRead()` can make progress without another byte arriving. It deliberately does **not**
  promise `TLSRead()` will not block, because what is buffered may be half a record; that
  is the same bound `TLSA_Timeout` already caps. Kept separate from `TLSPending()` rather
  than folded in, because the two answers mean different things and a caller that conflates
  them will call `TLSRead()` and block where it meant to poll.

#### The version bump those two vectors needed, and did not get until it was pointed out

`TLSRandom` and `TLSBuffered` went into the vector table while `TLS_LIB_VERSION` stayed at
1. **Exec opens a library when `lib_Version >= the version asked for` and looks at nothing
else**, so `OpenLibrary("tls.library", 1)` would have handed an older library to a caller
compiled against the newer header — and `MakeLibrary()` stopped at the `(APTR)-1`
terminator, so the jump table is not that long. The jump lands in whatever is in front of
the base, on a machine with no memory protection. Caught by the curl backend agent, which
had already defended itself with a `lib_NegSize` check.

**Version 2, revision 0.** The revision convention is the one `bsdsocket.library` and
`usergroup.library` already follow and it is worth stating because it looks like laziness
and is not: nothing in this project reads a revision, and a number nobody reads goes
stale. Those two report *version* 4 because 4 is the AmiTCP/Roadshow level applications
pass to `OpenLibrary()` — fixed by an external compatibility contract, as
`include/aminetxduo/version.h.in` says in as many words. `tls.library`'s ABI is ours, so
its version is simply its vector level and moves when the table does.

**The rule is now a build failure rather than a comment**, which is the part worth
copying elsewhere. `TLS_LIB_VECTORS` is *derived* from `TLS_LIB_VERSION` by token
pasting — `TLS_LIB_VECTORS_V1` is 8, `TLS_LIB_VECTORS_V2` is 10 — and
`src/tlslib/tls_vectors.c` asserts the real table length against it. Add a vector and the
build stops; the only way to make it pass is to declare a `TLS_LIB_VECTORS_V<n>` and point
`TLS_LIB_VERSION` at it, which puts the version constant under your cursor at the moment
you need to change it. A second assertion ties `TLS_LVO_LAST` to the same table so a
caller checking `lib_NegSize` checks the right offset. **Both were verified to fire**, by
setting the version back to 1 and watching the build stop — a guard that has never been
seen to fail is not a guard.

**`fetch` asks for 1, not `TLS_LIB_VERSION`**, because 1 is what it uses: every vector it
calls is original. Asking for the constant would mean a recompile silently demanded a
newer library than the transfer needs, and a user with a working older pair would lose
`https:` for nothing. It zeroes its `TLSInfo` before the call so `ti_Resumed` — a
version-2 field — reads FALSE against a version-1 library instead of reading the stack.
That is the general rule and the header now states it: **`ti_Size` lets an old caller talk
to a new library; a new caller talking to an old one has to zero the structure**, because
the old library will fill what it knows and leave the rest of your stack alone.

### 13.8 What was measured, and what was not

**Cross-process, proved.** Four `fetch` invocations — four processes — in one boot:

| | |
|---|---|
| `https://tls-v1-2.badssl.com/` (follows a 301 to port 1012) | 6.8 s and 5.0 s, both full |
| the same command again | **0.6 s and 0.6 s, both resumed** |
| `https://ecc256.badssl.com/` | 23.3 s, full |
| the same command again | **0.5 s, resumed** |

Both hosts on the redirect are the same name on different ports with different
ciphersuites (0xC027 on 443, 0x3D on 1012), which is also the check that the cache is
keyed by port and not only by name.

**Cross-boot, proved.** §13's headline: the machine was rebooted between the two runs and
only `DEVS:Internet/tlssessions` crossed.

**A rejected ticket falls back, proved.** `tests/tls/tls_resume` copies the session cache,
flips a byte in the ticket *and* in the session ID — both, because a server with a working
session-ID cache could otherwise resume from the ID and the test would fail for being
right — and connects. The result is a full handshake, 6.8 s, the page still arrives, and
**the very next connection resumes again**, which is the check that the broken entry was
replaced rather than left to fail forever.

**`TLSA_NoResume`, proved.** Full handshake with a valid session sitting in the cache.

**The one-off `ecc256.badssl.com` failure reported against `f535728` is closed, and it was
never a library bug.** It did not reproduce here — two cross-process attempts resumed in
0.5 s and the in-process test resumes it every run — and the register bug in §13.5 turned
out not to explain it either, because the backend compiles byte-identically with and
without the fix. It is now believed to have been a `tls.library` built from an uncommitted
working tree. Against `e42db07` that host gives 23.27 s cold and then 0.58 s and 0.58 s
resumed over three separate processes, all HTTP 200.

Worth keeping as a process note rather than a technical one: **a measurement taken against
an unbuilt-from-a-commit binary is not a measurement**, and two agents chased it for a
while on the strength of a plausible mechanism that happened to be in the right area.

**Not measured, and worth saying:** how long a Cloudflare ticket actually stays good on
this path (the hint is 64,800 s and nothing here has waited that long); whether an
`AmigaOS` machine with a genuinely dead clock resumes across a reboot (it should — a
zero timestamp disables ageing — but the test sets the clock rather than removing it);
and anything at all about a server that rotates its ticket keys mid-session, which is the
case the fallback path exists for and which no public host will perform on demand.

### 13.9 Size

| | bytes |
|---|---|
| `tls_resume.c` object, text | 8,120 |
| the additions to `tls_conn.c` (two vectors, three `TLSInfo` fields, the cache hooks) | ~1,300 |
| the resident session cache, allocated on first use | 3,408 |
| `tls.library` | 282,516 |
| `bsdsocket.library` (`AMINETXDUO_TLS=ON`) | 250,248 |
| **the pair** | **532,764 = 520.3 KiB** |

Roughly 9.4 KiB of `tls.library`'s growth is this work; the rest is other traffic in the
tree. Nothing resident changes for a machine that never opens a connection — the cache is
allocated on the first handshake that has something to remember and freed at expunge.

**Against a 512 KiB figure the pair is now 8.3 KiB over**, which §9 tracked closely when
that number looked like a ceiling. It is not one, so this is recorded and not acted on.
Worth knowing anyway: the trimming lever is untouched and much larger than the delta.
`src/tls/CMakeLists.txt` globs **all** of `crypto_libraries/src`, so `nx_crypto` still
carries DES, 3DES, MD5, CCM, GCM and ECJPAKE, none of which any shipping ciphersuite
reaches. Anyone who ever does need the space should start there.

### 13.10 What this does not fix

**The first connection to a host still costs what it always cost.** Resumption is a second
connection's optimisation, so a machine that has never spoken to `www.iana.org` still
cannot reach it at 14 MHz. The seed has to come from somewhere: a faster clock, a patient
host, or — the honest option nobody has built — a way to carry a session file between
machines.

**A verified-chain cache is still worth 8–10% and is still not the answer.** That estimate
in §11 stands and this work does not change it. What it does change is the priority: with
resumption in, the expensive handshake is the one that happens once, and shaving 10% off
something that happens once is worth much less than it was.

**Nothing here helps TLS 1.3**, which is where the web is going. `nx_secure`'s TLS 1.3 is
impractical on this hardware for an unrelated reason (a bit-serial GHASH), and its
resumption is a different mechanism — PSK, not tickets — so none of this code carries over.


## 14. curl as an adversary: a verification suite for bsdsocket.library (2026-07-25)

§11 established that curl runs on the 68020 and fetches pages. This is the
other half of the question: **what does curl do to the stack that our own
tools do not**, and does the stack survive it.

The framing matters, because it decides what a red result means. curl is not
the thing under test. It is a client that has been driven against every TCP
implementation in commercial use for twenty-five years, so its transfers are a
far better probe than anything written here on purpose — and every failure the
suite finds is a bug in `bsdsocket.library` until somebody proves otherwise.

**It found three.** The SANA-II receive queue was four frames deep and sixteen
simultaneous connections lost six of them; every last close of the library cost
fifteen seconds and left a reader thread running on freed memory; and a resumed
TLS handshake accepts a certificate chain that the trust store it was handed
would have refused. The first two are fixed here. The third is in
`src/tlslib/`, which this work does not own, and is reported rather than
touched.

### 14.1 What is in the tree

| | |
|---|---|
| `tests/curl/curlpeer.py` | the host end: HTTP/1.1 with keep-alive, ranges, chunking and drip-feeding; seven HTTPS servers on seven certificate chains; FTP (borrowed from `tests/tools/netpeer.py` rather than rewritten); and four deliberately rude listeners |
| `tests/curl/mkpki.sh` | a whole test PKI — RSA and ECDSA roots, three levels of intermediates, expired, self-signed, and a root nobody trusts |
| `tests/curl/curlsuite.py` | the cases and the scoring, in one file |
| `tests/curl/curlcheck.c` | the Amiga-side driver |
| `tests/curl/run-curlverify.sh` | stage, serve, run, score |

**Hermetic by default.** Groups A–F talk to this host and to nothing else: no
DNS beyond one deliberate NXDOMAIN, no internet, no public CA. `-g G` is the
internet suite and is **NOT A BASELINE**, kept separate for that reason.

Three decisions are worth writing down.

**The servers are hand-rolled and `http.server` is not used**, because the half
of the suite that matters cannot be built on a framework that guarantees a
well-formed response. The suite needs a server that promises a
`Content-Length` and then closes at a quarter of it, one that answers with a
RESET, one that accepts and says nothing for ever, and one that answers with
bytes that are not HTTP. Those are the cases where a stack crashes instead of
returning an error.

**Every body is checked byte for byte, on the host.** `DH0:` is a host
directory, so what the Amiga wrote is already here; the server's bodies are
slices of one seeded 2 MiB buffer, so `/bytes/1200000` and `/bytes/1` are
checked the same way and no manifest has to travel. Nothing is taken on trust
from a status line.

**The scoring is on the host, not on the Amiga**, because a run that dies has
to be scored too. `CurlCheck` appends one line per case as it goes and flushes
it by closing the file; every case with no line is a failure, and the first
missing one names the command that took the machine down. A driver that scored
its own run would report nothing at all in exactly that case.

Each result line also carries `AvailMem(MEMF_ANY)`, which is how a leak of a
few kilobytes per socket becomes a trend somebody can see.

### 14.2 The first finding: the SANA-II receive queue was four frames deep

`d03_parallel_40` — forty concurrent transfers through curl's multi interface —
came back `curl: (7) Could not connect` for thirteen of the forty, each after
about thirteen seconds. A sweep found the cliff between eight and sixteen:

| concurrent transfers | lost |
|---|---|
| 8 | 0 |
| 16 | **6** |
| 24 | **7** |
| 28 | **10** |
| 32 | **11** |
| 36 | **16** |
| 40 | **15** |
| 48 | **22** |

**87 of 232 transfers.** The measurement that turned this from a mystery into a
diagnosis is on the host side: `curlpeer.py` logs every connection it accepts,
and it accepted **213** across the sweep while only **145** transfers completed
on the Amiga. Sixty-eight connections were therefore established at the far end
and never used — the SYN went out, the peer answered, and the answer was never
seen. Thirteen seconds is our own connect giving up, retransmitting a SYN into
a connection the other end already considers open.

The cause is one constant, and its own header comment describes the mechanism
without drawing the conclusion:

> `CMD_READ` is per packet type and the device has no buffers of its own: every
> frame that arrives with no matching read outstanding is dropped on the floor.

`AMI_SANA2_RX_DEPTH_IPV4` was **4**. That is not a queue length, it is the
receive window measured in frames — and sixteen sockets opening at once produce
sixteen SYN/ACKs inside a few hundred microseconds, which a 14 MHz 68020 cannot
re-post a read between. Twelve of them hit a device with nothing outstanding
and were discarded by `a2065.device` before any of our code ran.

Rebuilt with the depth at 8 and nothing else changed — same build directory,
same flags, one `-D` — the same sweep loses **nothing** up to forty.

**And the eight-way case, which never failed, was 2.5× slower than it had to
be.** That is the part worth noticing: loss was not a cliff that only appeared
at sixteen, it was there all along and TCP was papering over it with
retransmissions.

| concurrent | depth 4 | depth 32 |
|---|---|---|
| 8 | 8.66 s | **3.52 s** |
| 16 | failed | **4.94 s** |
| 24 | failed | **5.38 s** |
| 32 | failed | **6.08 s** |
| 40 | failed | **8.04 s** |
| 48 | failed | **8.68 s** |

**The fix is not "8".** A fixed number here is what caused the problem, and the
right bound is memory: each outstanding read pins an `NX_PACKET` for its whole
life, and the pool is already sized from `AvailMem()`.
`ami_sana2_rx_ipv4_depth()` therefore takes a fixed share of the pool —
`AMI_SANA2_RX_POOL_SHARE`, one in eight — with the old 4 as the floor and 32 as
the ceiling. On the 8 MB profile the pool is 256 packets and the depth comes
out at the ceiling; on the 4 MB / 68020 floor the pool is
`AMI_POOL_MIN_PACKETS` (16), one eighth of which is 2 — below the old floor —
so such a machine keeps its four and **cannot** absorb the burst. That limit is
real and is stated rather than hidden. ARP and IPv6 ND are low-rate and stay
shallow.

It costs about 4 KB of new memory (24 more `AmiRxSlot` in each of two readers)
and holds up to 28 more of the 256 packets already in the pool.

#### And where the new ceiling is

Raising the depth moved the limit rather than removing it, and a user who hits
the new one should get an explanation instead of a mystery.

| `--parallel-max` | result |
|---|---|
| 8, 16, 24, 32, 40, 48 | every transfer completes, every body byte-identical |
| **56** | **stalls**: 16 of 56 complete, the host reports 40 connections open, and nothing moves again |

So the ceiling is somewhere between 40 concurrent sockets and 56. Three things
govern it and the suite cannot say which binds first: the packet pool
(`AMI_POOL_MAX_PACKETS` is 256 and the IPv4 reader now pins 32 of them), our
`BSD_DEFAULT_DTABLESIZE` of 64, and curl's own `FD_SETSIZE` of 64 — 56
transfers plus the multi handle's wakeup socketpair is 58 descriptors, which is
close enough to both 64s to be suspicious. It is **not diagnosed and not
fixed**: nobody runs 56 simultaneous transfers on a 14 MHz 68020 in earnest,
and `d03_parallel_40` keeps the default suite comfortably under it.

**Nothing else in the tree could have found this.** The conformance suite tests
one vector at a time, `tests/clients` replays one client sequence at a time,
and `nc`, `ftp` and `fetch` open one connection. A burst of sixteen
simultaneous connects is what a multi-handle client does on its first line, and
until curl ran there was no such client here.

### 14.3 The second finding: fifteen of the sixteen seconds were the SHUTDOWN

The suite runs `curl --version` twice, once either side of `AddNetInterface`,
because the first one took sixteen seconds and the reason is not what it looks
like:

```
--- SYS:curl --version                 (nobody else holding the library)
--- rc 0, 16.22 s
--- SYS:AddNetInterface eth0
--- rc 0, 1.46 s
--- SYS:curl --version                 (afterwards)
--- rc 0, 0.32 s
```

It is not the executable loading: a second `--version` before the interface
also took **15.92 s**, and every curl after `AddNetInterface` loads the same
939 KB binary in a second or two.

**It is not the stack coming up either, and that is the useful part.**
`AddNetInterface` brings the whole thing up — `NX_IP`, the ThreadX threads, the
SANA-II device, ARP, DHCP — in **1.46 s**. What it does not do is close its
handle again; `tool_stack_start()` says in as many words that the leaked
reference is how the interface stays up. curl closes, and closing is what cost
fifteen seconds.

The serial log named the culprit and it can be counted:

```
[WARN] sana2: reader 0 did not stop
[WARN] sana2: reader 1 did not stop
```

**Exactly one pair per shutdown** — one pre-interface `curl --version` logs one
pair, two log two. `ami_sana2_rx_stop()` waited `5 * NX_IP_PERIODIC_RATE` for
each reader's exit semaphore and gave up, so ten of the sixteen seconds were
two five-second timeouts, on every last close of `bsdsocket.library`.

#### Why the readers did not stop: the driver does not honour AbortIO()

The answer was already in this tree, at the top of `CMakeLists.txt`, written by
somebody looking at the other end of the same lifecycle:

> The SANA-II raw-framing probe posts a `CMD_READ` with `SANA2IOF_RAW` and takes
> it straight back with `AbortIO()`. Commodore's `a2065.device` 2.16 does not
> abort it, so `ami_sana2_open()` never returns — verified under FS-UAE.

That was worked around by turning the probe off, which left every other
`AbortIO()` in the shim still assuming it works. `ami_sana2_rx_teardown()`
aborted its queued reads and then called **`WaitIO()`, which has no deadline**,
on requests the device was never going to return.

#### And the fix is an ordering, not a workaround

`S2_OFFLINE` returns every queued `CMD_READ` with `S2ERR_OUTOFSERVICE`.
`ami_sana2_rx_drain()` has said so in a comment since it was written, and it is
the SANA-II documented behaviour — it does not depend on `AbortIO()` at all.

**It was being issued too late.** `ami_sana2_close()`, `NX_LINK_DISABLE` and
`NX_LINK_UNINITIALIZE` all read `rx_stop(); tx_drain(); offline();`. The one
command that would have freed the readers came ten seconds after they had given
up waiting for something else. `ami_sana2_rx_stop()` now takes the interface
offline **first**, and does it itself rather than at each of the four call
sites, so the next caller cannot get the order wrong.

Three further things were wrong underneath it, and each is worse than the delay:

- **`WaitIO()` is gone.** `ami_sana2_rx_reap()` collects replies with a bound
  (25 × 2 ticks, one second) and answers how many the device still owns.
- **`CMD_FLUSH` is the escalation.** Exec defines it as "abort all queued
  requests for this unit" and SANA-II carries it forward; it is what a driver
  that ignores `AbortIO()` is supposed to implement. Unit-wide, so it is second
  and not first.
- **Nothing the device still owns is freed any more.** The old path freed the
  reply port, released the pinned `NX_PACKET`s, terminated the reader thread,
  freed the stack it was running on and let `ami_sana2_close()` free the whole
  interface — with reads still queued into all of it. On a machine with no
  memory protection that is not a leak, it is a corruption waiting for the next
  matching frame. It now refuses to free any of it, says so at `AMI_ERROR`, and
  marks the interface unrestartable. **A 32 KB leak is recoverable and a thread
  executing freed memory is not.**

Measured after the change, same profile, same binaries, nothing else touched:

| `curl --version`, sole holder of the library | before | after |
|---|---|---|
| elapsed | **16.22 s** | **2.20 s** |
| `reader did not stop` warnings | 2 | **0** |

The 2.20 s that is left is a whole stack brought up and taken down again for a
command that prints a version string — consistent with the 1.46 s
`AddNetInterface` pays for the bring-up alone — and `curl --version` with
`AddNetInterface` already run is unchanged at 0.30 s. Fifteen seconds of it
were a driver's refusal to honour `AbortIO()`, waited out twice, on every
command a user typed.

### 14.4 The third finding: TLS

#### A resumed handshake ignores the trust store, and the cache is keyed on the host name

Two TLS cases went green when they should have gone red, and the pair of them
is a security defect rather than a test bug:

| case | trust store offered | expected | got |
|---|---|---|---|
| `--cacert DH0:otherstore` | a valid store holding a root that signed **nothing** in the chain | refused (60) | **HTTP 200** |
| no `--cacert` at all | `DEVS:Internet/certificates`, the real Mozilla set, which has never heard of our test root | refused (60) | **HTTP 200** |

`%{time_appconnect}` says why, and says it unambiguously:

```
rsa2.test, correct store, cold      5.68 s     full handshake
rsa2.test, WRONG store              1.64 s     resumed
rsa2.test, no store at all          0.72 s     resumed
```

A full RSA handshake on this machine is 5.7 s and a resumed one is under two.
Both of those connections **resumed a session cached from an earlier case in
the same group** and therefore verified no certificate at all — which is what
resumption is for, and exactly the problem: `tls.library` keys its cache on
`TLSA_HostName` alone, so the trust decision is cached with the session and
silently reused under a different `--cacert`, or under none.

Cold verification is fine, and the suite proves that separately: an expired
leaf, a self-signed leaf and a certificate issued to another host are all
refused with curl's own exit 60 on a cold handshake. It is only the second
connection that stops checking.

Two things make it worth taking seriously rather than filing as a curiosity.
The cache is **mirrored to `DEVS:Internet/tlssessions`**, so it survives a
reboot — §13's headline is that a session seeded once carries across one. And
`--cacert` is the switch a user reaches for precisely when they do not trust
the default store, which is the case where being ignored matters most.

**This is not fixed here: `src/tlslib/` is not this work's to change.** The
suite now asks the question in both orders — `e01`/`e02` before anything has
talked to that host, `e23`/`e24` after everything has — so a fix can be
checked without having to remember which case ran first.

### 14.5 What curl could not break

Everything below passed with the body checked byte for byte against the host's
copy. It is worth listing because a suite that finds one bug and reports
nothing else is not saying much:

- **Sizes.** 1 byte, 1 KB, 64 KB, 65,537 bytes (one past a power of two),
  1,200,000 bytes, and an empty body — all byte-identical.
- **Framing.** `Content-Length`, chunked, chunked with a trailer, chunked in
  4,096 writes of ONE byte, 256 KB in 1,024 writes, and a body with no length
  at all that ends when the peer closes.
- **The header parser's buffers.** 62 KB of response header across 200 header
  lines; a single 60 KB header line; a response header delivered one line at a
  time with 400 ms between them; a 2.4 KB request line; and a ~2.5 KB `Cookie`
  header built from 53 cookies curl stored and sent back.
- **Ranges.** 206 from the middle, from the tail, and open-ended.
- **Methods and status.** GET, HEAD, POST, PUT, DELETE, OPTIONS, PATCH; fifteen
  status codes including 204 and 418; `--fail`'s exit 22.
- **Redirects.** 1, 3, 5 and 20 deep, 301/302/307, the `--max-redirs` cap (47)
  and a redirect loop (47).
- **Uploads.** 4 KB and 200 KB by POST and by PUT, each verified by the server
  hashing what it received and curl saving the answer — with a 100-continue
  round trip in front of the large ones.
- **Connection behaviour.** Four transfers on one connection (`conns_total=1`),
  four with the peer closing each time (`conns_total=4`), reuse after a 404 and
  after a HEAD, 60 transfers on one handle and one connection, and 20
  connect/transfer/close cycles with descriptors coming back.
- **The multi interface.** 4, 8 and 40 concurrent transfers, a slow drip
  alongside fast ones, and chunked in parallel — which is `WaitSelect()` over
  the whole set plus the `bind`/`listen`/`connect`/`accept` socketpair
  `lib/socketpair.c` builds for the wakeup.
- **Failure paths, every one of them an error rather than a crash.** Connection
  refused (7); an unresolvable name (6); a peer that promises 400 KB and closes
  after 100 KB (18); the same with a RESET (56); an accepted connection closed
  with nothing sent (52); an answer that is not HTTP (1); a peer that accepts
  and never speaks, ended by `--max-time` (28); `--max-time` firing mid-body
  with data still arriving (28); `--connect-timeout` to a black hole (28);
  `--max-filesize` aborting a transfer (63); and curl walking away from a
  1.9 MB download after three seconds with the connection full — the
  Ctrl-C-equivalent. An ordinary transfer immediately after all of them
  succeeds, which is the case that would have caught a stack left damaged.
- **Memory.** `AvailMem(MEMF_ANY)` was **9,563,984 bytes at the first case and
  9,563,776 at the last**, across 124 cases, ~250 transfers, 20 separate curl
  processes and every failure path above — a drift of 208 bytes. Not "roughly
  stable": the same number.

The score, on the A1200 profile with both fixes in:

```
groups A-D and F (hermetic)      122 passed, 2 failed, 124 cases
group E (TLS, hermetic)           23 passed, 2 failed,  25 cases
```

**All four of those failures are real and none of them is a false alarm**, so
they are listed rather than explained away:

- `e01`/`e02` (or `e23`/`e24` depending on order) — the trust-store defect of
  §14.4, which is `src/tlslib/`'s to fix.
- `a44_cookies_send` — **curl does not write its cookie jar on AmigaOS**, and
  this one is neither ours nor the stack's. `-c DH0:cj.txt` leaves a zero-byte
  file with no temporary beside it: `Curl_fopen()` truncates the target, fstats
  it, and then fails somewhere in the write-to-temp-and-rename path. The
  attribution is settled by the third-party binary of §14.7, which **fails
  identically** — two curls built independently, one newlib and one clib2,
  sharing no libc between them, so what they share is curl's own code and an
  AmigaOS path. `dirslash("DH0:cj.txt")` finds no `/` and yields an empty
  directory, so the temporary is created relative to a current directory a
  `System()` child does not usefully have. `open()` and `rename()` both link
  and are real implementations here, so it is not the §11.2 missing-wrapper
  problem. It costs the cookie jar, the alt-svc cache and the HSTS cache, all
  three of which go through that one function.
- `f07_ftp_active` — see §14.8; the guest listens correctly and the host cannot
  reach it, because FS-UAE never opens the forward.

### 14.6 Regression cover for the fixes

Both fixes are in `src/sana2/`, so both existing tiers were re-run against the
library carrying both:

```
tests/conformance  LOOPBACK   125 passed, 1 failed, 16 skipped   (unchanged)
tests/clients                 94 checks, 0 failures              (unchanged)
```

TCP loopback throughput is 356 KB/s before and after, so the deeper read queue
costs nothing on the path that was already working.

The failing-before / passing-after test is in the suite itself:
`d03_parallel_40`, and `tests/curl/run-curlverify.sh -p 8,16,24,32,40,48` is
the sweep that produced the table above. Note that it only fails on a machine
whose pool is large enough for the fix to raise the depth — on the 4 MB floor
the depth stays at 4 and so does the loss.

### 14.7 Somebody else's curl, on our stack

The strongest ABI evidence available is a binary nobody here built, because it
cannot have been shaped around our quirks. There is one, and it is newer than
ours:

| | |
|---|---|
| Aminet | `comm/tcp/curl-8.22.0-DEV-210726.lha`, 2026-07-23, 4,704,106 bytes |
| Port | Darren Banfi (`boingball`), source at `github.com/boingball/curl` branch `amigaos`, commit `ffec7145` |
| Upstream | curl **8.22.0-DEV**, one release newer than the 8.21.0 of §11 |
| Built with | `m68k-amigaos-gcc 13.2.0`, **clib2**, soft-float, `-O0`, `--with-amissl --with-zlib` |
| Ships | five executables (68000/020/030/040/060) and five matching `libcurl.a` |

**The second-hand "crashes most of the time on Roadshow" report is stale and
its diagnosis was wrong.** There is no current 8.11.1-DEV package; that was the
first release (2024-11-24) and has been superseded three times. The port's own
`docs/AMIGAOS.md` says the 8.18 release "increased the default stack size from
16384 to 32768. This fixes crashes during TLS handshakes, certificate
validation, compressed downloads, and large HTTPS transfers" — and the 8.11.2
binary does carry a literal `$STACK:16384` cookie. So the crashes were stack
exhaustion in the port, not an incompatibility with Roadshow, and the buggy
release's own readme already claimed Roadshow worked. Both current readmes say
"Roadshow TCP and WinUAE networking have been tested."

**The stack cookie is worth stealing.** At file offset `0x1c3ec` their binary
contains the plain string `$STACK:32768`, NUL-terminated, sitting in the text
segment as a dead constant. That is the entire mechanism — no `__stack` symbol
— and AmigaOS 3.1.4+ scans a loaded executable for it. §11.5 records that this
toolchain's `crt0.o` exports no `__stack` hook and that ours therefore needs
`stack 200000` typed by hand; a `$STACK:` string constant would remove that
step on 3.1.4 and newer, and would be ignored harmlessly on 3.1. Kickstart 3.1
is what this harness boots, so `CurlCheck` still supplies the stack itself.

**What it demands of the ABI that ours does not.** `curl_setup.h` gives OS3 the
same bsdsocket path either way — `select()` is `WaitSelect()`, `HAVE_FCNTL` is
off, sockets close with `CloseSocket()` — but `lib/amigaos.c`'s
`Curl_amiga_init()` is compiled `#elif !defined(USE_AMISSL)`, so **their build
never makes the `SocketBaseTags` call at all**. clib2's own networking startup
opens the library and installs the errno pointer instead, and it also opens
`usergroup.library`, which ours never touches. The strings name a hard
`bsdsocket.library v4` requirement and `amisslmaster.library v5`, plus
`mathieeedoubbas`, `mathieeedoubtrans` and `mathieeesingbas`.

**AmiSSL has no 68000 build**, which contradicts the readme's "68000 … no FPU
required" for anything over HTTPS: `util/libs/AmiSSL-v5-OS3.lha` (v5.27) ships
only `os3-68020` and `os3-68060` copies of `amissl_v362.library`.

#### What it took to get it to the starting line, and what that says

Nothing that stopped it was ours, and both blockers are worth writing down
because the next ported binary will hit the same two.

**`mathieeedoubtrans.library`.** clib2's startup opens it before `main()` and
prints `mathieeedoubtrans.library could not be opened.` if it cannot; there is
no partial mode. Kickstart 3.1's ROM has `mathieeesingbas` and nothing else,
and this harness boots a bare directory hard drive, so the pair has to be
staged. They must also be a MATCHED pair — the `.ld.strip` builds out of the
AmigaOS source tree work together; a `doubbas` from one build and a `doubtrans`
from another do not. Our own curl is newlib and never asks for `doubtrans`,
which is exactly why nothing in this tree had needed it until somebody else's
binary was run.

**A requester with nobody at the keyboard.** AmiSSL is configured
`OPENSSLDIR=AmiSSL:`, so a binary linked against it asks AmigaDOS for a volume
that a test rig does not have, and AmigaDOS puts up "Please insert volume
AmiSSL:" and waits for ever. `CurlCheck` now sets `pr_WindowPtr = -1` on itself
and passes `NP_WindowPtr` to every command it starts, so a missing assign fails
instead of hanging. It also makes the `AmiSSL:` assign itself when the
directory is staged, since a bare boot has no `C:assign` to type it with.

#### What a different entry into the ABI proved

The reason a foreign binary is worth the trouble is that it does not reach our
library the way ours does. `lib/amigaos.c`'s `Curl_amiga_init()` — the
`OpenLibrary("bsdsocket.library", 4)` plus `SocketBaseTags(SBTC_ERRNOPTR)` that
§12.1 calls "the one call that gates the port" — is compiled `#elif
!defined(USE_AMISSL)`, and their build defines `USE_AMISSL`. **So that function
is not in their binary at all.** clib2's own networking startup opens the
library and installs the errno pointer instead, and it opens
`usergroup.library` besides, which nothing of ours has ever called at runtime.
Everything above that is identical — `select()` is still `WaitSelect()`,
`HAVE_FCNTL` is still off, sockets still close with `CloseSocket()` — so this
is the same ABI entered through a different door, by code neither written nor
tuned here.

#### And it scores exactly what ours does

```
curl 8.22.0-DEV (m68k-unknown-amigaos) libcurl/8.22.0-DEV OpenSSL/3.6.2 zlib/1.3.1
Protocols: dict file ftp ftps gopher gophers http https imap imaps ipfs ipns
           mqtt mqtts pop3 pop3s rtsp smtp smtps telnet tftp ws wss
Features: alt-svc HSTS HTTPS-proxy libz SSL threadsafe
```

| groups A–D and F, 124 cases | passed | failed |
|---|---|---|
| our curl 8.21.0 + `amitls` (newlib) | **122** | 2 |
| Aminet curl 8.22.0-DEV (clib2, AmiSSL) | **122** | 2 |

**The same two, and neither of them is the stack**: `a44_cookies_send`, which
fails identically on both and is therefore curl's own AmigaOS path handling
rather than anybody's libc; and `f07_ftp_active`, which FS-UAE cannot deliver
(§14.8). Every other case — 1,200,000-byte bodies checked byte for byte,
4,096 one-byte chunked writes, a 60 KB header line, 40-way concurrency through
the multi interface, twenty separate processes, a peer that RESETs mid-body,
another that accepts and never speaks, an FTP upload read back and compared —
passes on a binary nobody here built.

`AvailMem` over the 124 cases went 6,114,048 → 6,062,448. That is not a trend:
it is two steps of about 25 KB in the first two dozen cases and then a flat
line for the remaining hundred, which is AmiSSL's 3.5 MB library and its
allocations settling, not a leak. (Ours drifts 208 bytes over the same run,
because it has no AmiSSL to load.)

**So the "crashes most of the time with Roadshow TCP" report is not reproduced
and its diagnosis was wrong.** Nothing crashed. The version it referred to is
three releases old, the port's own changelog attributes those crashes to a
16 KB default stack that it raised to 32 KB, and both readmes of the era
already claimed Roadshow worked.

### 14.8 What the suite cannot see, said plainly

- **It cannot distinguish our stack from FS-UAE's SLIRP** except by inference.
  The 40-way finding was pinned down because the host's accept count and the
  guest's completion count disagreed, which puts the loss on the inbound leg;
  raising a constant in `src/sana2/` then fixed it, which puts it on ours. No
  other case has that kind of corroboration.
- **Every measurement is on a 68EC020 at 14 MHz with 8 MB of Zorro II Fast
  RAM.** The packet pool is 256 packets there. On the 4 MB floor the pool is
  16 and several of these numbers would be different -- the RX depth for one,
  which is exactly why it is now computed rather than fixed.
- **`--max-time` cannot fire inside a TLS handshake**, because `TLSOpen()`
  blocks (§11.8). `e16_tls_silent` accepts either exit code for that reason and
  is a documentation case, not a pass/fail one.
- **Nothing here tests IPv6**, because `AMINETXDUO_IPV6` is off in the shipping
  stack and curl is built with `ENABLE_IPV6=OFF` to match.
- **Active-mode FTP cannot pass under FS-UAE 3.2.35, and it is the emulator
  rather than us.** `f07_ftp_active` fails with curl's exit 10 and the server's
  425. The Amiga does its half correctly — the peer log records
  `PORT 10,0,2,15,27,249`, so the guest bound port 7161, listened, and
  advertised its own address — and the host then cannot reach it. Checked from
  the side rather than inferred: with the emulator running and
  `uae_slirp_redir = tcp:7260:7260` through `tcp:7263:7263` all accepted by the
  config parser (`set option "slirp_redir" ... (result: 1)`, four times, in
  FS-UAE's own log), **`lsof` shows nothing listening on any of those ports**.
  This is the same wall §12 hit with `uae_slirp_ports` and conformance test 41:
  SLIRP is a NAT and this build of FS-UAE opens no inbound path. The capability
  itself is covered on loopback by `tests/clients` groups D, E, I and M.

## 15. crypto68k against AmiSSL, on the same machine (2026-07-25)

Everything `src/crypto68k/` has ever been measured against is the vendored `nx_crypto`
it replaces. That is the right baseline for *did the change work* and the wrong one for
*is this stack worth having*, because this machine already has an OpenSSL: **AmiSSL**,
which is what the Aminet curl links against and what every other TLS client on a classic
Amiga uses. §11.6 guessed that a well-maintained library would land 1.5–2.5× slower than
`crypto68k` rather than 3–4×, and said plainly that nobody had measured it.

`tests/crypto68k/crypto68k_amissl` is the measurement: both implementations on identical
inputs, back to back, **in one process**, each answer checked against the other's before
it is timed, and a MULU.L-corrected figure beside every measured one.
`tools/amissl-run.sh` stages the library and runs it.

### 15.1 What AmiSSL is on this target, and why the CPU build decides the answer

AmiSSL **5.27** is OpenSSL **3.6.2** — read from the running library, not from a header:
`OpenSSL 3.6.2 7 Apr 2026`, `amissl_v362.library 5.27 (8.4.2026) os3-68020 version`.

The OS3 release ships exactly two m68k builds and one CPU-neutral master:

| file | bytes | built |
|---|---:|---|
| `Libs/AmigaOS3/amisslmaster.library` | 4,976 | CPU-neutral |
| `Libs/AmigaOS3/AmiSSL/68020-40/amissl_v362.library` | 3,587,424 | `-m68020-40 -msoft-float` |
| `Libs/AmigaOS3/AmiSSL/68060/amissl_v362.library` | 3,591,992 | `-m68060 -msoft-float` |

`amisslmaster` resolves the second by a literal path — `LIBS:AmiSSL/amissl_v%ld.library`,
visible in `strings` — so the directory layout is not negotiable. **The 68020-40 build is
the one used here, and it is the one that gets the assembly:**

- `Configurations/15-amissl.conf` gives `amiga-os3-68020` `asm_arch => 'm68k'` and
  `bn_ops => add("BN_LLONG")`. `amiga-os3-68060` gets neither.
- `crypto/bn/build.info:90` sets `$BNASM_m68k=asm/bn_m68k.s` and the dispatch below it
  **overwrites** `$BNASM`, so on this target `bn_asm.c` is not compiled at all and the
  ten bignum primitives come from **Howard Chu's 2002 68020 assembly** — 1604 lines,
  never merged upstream, carried by AmiSSL. §9 recorded that it "survives in AmiSSL";
  it is now confirmed that it is built and that it is what runs.
- The 68060 build has none of it, for the reason §9 already gave: `MULU.L`'s 32×32→64
  form is not implemented on a 68060 and traps.

So this is not our assembly against OpenSSL's C. It is **our assembly against OpenSSL's
assembly**, on a target where OpenSSL got the same idea twenty-four years earlier.
`BN_ULONG` is `unsigned int` — 32 bits, the same limb width we use.

### 15.2 The harness, and the one thing it does not link

`c68k_amissl_bench.c` owns the clock, the vectors and the reporting; `c68k_amissl_ossl.c`
is the only file that sees an OpenSSL header. Two translation units, because `nx_crypto`'s
headers and OpenSSL's both arrive through `<exec/types.h>` and both want names like
`SHA256_CTX`.

**Nothing of AmiSSL's is linked.** Every OpenSSL call is a macro from
`<inline/amissl.h>` that expands to a `jsr` through `AmiSSLBase` or `AmiSSLExtBase` —
AmiSSL v5 spans two library bases because OpenSSL has more entry points than one LVO
table holds. Verified in the disassembly: `BN_new()` is `jsr a6@(-2196)`. That sidesteps
whether an archive built by adtools GCC 2.95.3 links against GCC 15.2 output, and it
guarantees every measured cycle ran inside `amissl_v362.library`.

The vectors are the existing `c68k_vectors.h` and `c68k_ec_vectors.h` — the same
throwaway RSA-2048 key and the same RFC 6979 A.2.5 signatures the crypto68k tests use —
converted from HN_UBASE limb order to big-endian bytes at the boundary.

### 15.3 The measurement trap, handled rather than discovered

FS-UAE's A1200 model charges `MULU.L` **32.14 cycles where an MC68020 charges 45**
(`Dn,Dh:Dl`; 43 for the 32-bit form) — measured by `tests/perf/cpucal`, and reproduced by
this program's own inline-assembly calibration kernel in the same run. Every other
instruction the model charges is faithful to under 2%.

That is a 29% discount on the single instruction multi-precision arithmetic is made of,
and **it does not cancel between two implementations that issue different numbers of it.**
So each row carries a statically derived count of 32×32→64 multiplies and the report
prints

```
corrected = measured + count * t_mulu * (45 - 32.14) / 32.14
```

with `t_mulu` measured in-process. Working from a measured `t_mulu` rather than an assumed
clock is what makes the correction independent of `-k`; the kernel's implied clock came
out 56.53 MHz on a `-k 56` run, against `cpucal`'s 56 MHz, which is the check that it is
measuring the instruction and not the compiler.

The counts are derived, not sampled, because both sides are deterministic given the
operands.

**One Montgomery step, in 32×32→64 multiplies:**

| limbs | ours × | ours ² | AmiSSL × | AmiSSL ² |
|---|---:|---:|---:|---:|
| 32 (1024-bit) | 2s²+s = **2,080** | 1.5s²+1.5s = **1,584** | 576+1,056 = **1,632** | 324+1,056 = **1,380** |
| 64 (2048-bit) | **8,256** | **6,240** | 1,728+4,160 = **5,888** | 972+4,160 = **5,132** |

Ours is SOS with a schoolbook product; Karatsuba was costed and rejected at ~5% (§9).
AmiSSL's differs in a way that matters: because `OPENSSL_BN_ASM_MONT` is **not** defined
for this target, `bn_mul_mont` does not exist, and `bn_mul_mont_fixed_top()` falls through
to `bn_mul_fixed_top()`/`bn_sqr_fixed_top()` plus `bn_from_montgomery_word()`. Two
consequences, both in OpenSSL's favour: a genuine squaring shortcut, and **Karatsuba above
sixteen limbs** (M(8·2ᵏ) = 64·3ᵏ, S(8·2ᵏ) = 36·3ᵏ). At the 32 limbs an RSA-2048 CRT half
runs, OpenSSL issues about 22% fewer multiplies per Montgomery step than we do.

**Per operation:**

| operation | ours | AmiSSL | ours ÷ theirs |
|---|---:|---:|---:|
| RSA-2048 public, e = 65537 | 126,688 | 103,936 | 1.22 |
| RSA-2048 private, CRT | 3,978,832 | 3,688,408 | 1.08 |
| ECDSA P-256 verify | 164,476 | 186,280 | 0.88 |
| ECDH P-256 shared secret | 125,204 | **277,504** | 0.45 |
| k·G, an ECDHE key generation | 39,272 | **277,504** | 0.14 |
| AES-128-CBC, HMAC-SHA256 | 0 | 0 | — |

Read the last two rows before any timing: they are the whole story, and §15.5 says why.

### 15.4 What OpenSSL actually runs here

Traced through the AmiSSL 5.27 tree, not assumed.

| | what runs on `amiga-os3-68020` |
|---|---|
| limb primitives | `bn_m68k.s` — `bn_mul_add_words`, `bn_mul_words`, `bn_sqr_words`, `bn_mul_comba4/8`, `bn_sqr_comba4/8`, `bn_add_words`, `bn_sub_words`, `bn_div_words`. One `mulu.l` per limb, unrolled 4× |
| Montgomery | no `bn_mul_mont`; `BN_mul`/`BN_sqr` + `bn_from_montgomery_word` |
| big multiply | `bn_mul_comba8` at 8 limbs, `bn_mul_recursive` (Karatsuba) at 16 and above |
| RSA public | `BN_mod_exp_mont`, **no** `BN_FLG_CONSTTIME`, window 1 for a 17-bit exponent, leading zeros skipped — the same algorithm we use |
| RSA private | `BN_mod_exp_mont_consttime` on both CRT halves, fixed window 6, no zero skipping |
| P-256 field | `EC_GFp_nist_method` with `BN_nist_mod_256` — a limb-domain Solinas reduction, the same idea as ours, and because `BN_LLONG` is set it takes the `NIST_INT64` path: ~200 32-bit ALU operations, no multiplies |
| P-256 scalar | `ossl_ec_wNAF_mul` → **Montgomery ladder** for ECDH and k·G, wNAF (width 3) for ECDSA verify |
| generator table | none — `ossl_ec_wNAF_precompute_mult` is reached only from the deprecated `EC_GROUP_precompute_mult`, which nothing in OpenSSL's own ECDSA or ECDH calls |
| AES | no `AES_ASM`; `aes_core.c`'s table-driven `AES_encrypt`, four 1 KB T-tables, `FULL_UNROLL` off |
| SHA-256 | no `SHA256_ASM`; generic C, with a big-endian fast path that skips the byte swap for rounds 0–15 |

**The field layer is well matched and the scalar layer is not.** One P-256 field multiply
costs 64 multiplies on both sides (ours: eight `c68k_addmul_1` rows; theirs:
`bn_mul_comba8`), and a field square costs 36 on both. The entire difference is how many
field operations each scalar multiplication needs.

### 15.5 Constant time, and what it costs

**We are not constant time and OpenSSL is.** That is the mechanism behind the two largest
gaps, and it is a trade rather than a win.

- **ECDH and k·G take a Montgomery ladder.** `ec_mult.c` routes any scalar that *could*
  be secret to `ossl_ec_scalar_mul_ladder`, **ignoring `BN_FLG_CONSTTIME`** — the comment
  says so in as many words. That is 256 ladder steps, one per bit of the group order with
  no skipping, each a fused differential add-and-double costing 13 field multiplies and 7
  field squarings: **5,120 field operations, every time, whatever the scalar.** Ours is a
  width-5 wNAF (≈256 doublings, ≈43 additions) for a generic point and a Lim–Lee comb (26
  doublings, ≈50 additions) for the generator — **≈2,570 and ≈760**. Both of ours leak:
  wNAF and comb digits select table entries by value, and every conditional field
  correction is a branch.
- **RSA private is a fixed window with a full-table gather.** 1,021 squarings and 232
  multiplies per 1024-bit half, exactly, and each of the 171 multiplies is preceded by a
  constant-time read of the *whole* 64-entry table: `top × 2^window` = 2,048 volatile
  `BN_ULONG` loads, ≈350,000 per half. There is no `bn_gather5` on m68k, so this is the
  generic masked loop. Ours is a sliding window with leading-zero skipping and a direct
  indexed read.
- **Blinding is on by default** and we have none: two Montgomery squarings and two
  multiplies mod *n* per private operation, plus a full re-derivation (including an
  `A^e mod n`) every 32nd call. `RSA_blinding_off()` turns it off; the benchmark measures
  both.
- **`rsa_ossl_private_decrypt` ends with an unconditional verification** — it recomputes
  `r₀^e mod n` and compares. That is a whole RSA public operation, about 3% of a CRT
  private operation, and it cannot be disabled.
- **RSA public is the one place the two agree.** OpenSSL sets no `BN_FLG_CONSTTIME` for a
  public operation, picks window 1 for a 17-bit exponent from the same crossover table we
  copied, and skips the leading zeros exactly as we do. Sixteen squarings and two
  multiplies on both sides.

If we win the elliptic-curve operations, that is what buys it, and it is security we are
not paying for rather than arithmetic we do better. The threat model in §9 — a vintage
machine on a LAN, no remote timing attacker — is what makes that acceptable here; it would
not be acceptable for a server.

### 15.6 What it costs to open AmiSSL at all, and the trap that cost a day

Measured on the way in, because a program that opens AmiSSL pays this before it does any
crypto (`-k 56`, so divide the clock into it as you like — these are I/O and setup, not
arithmetic):

| | |
|---|---:|
| reading all 3,587,424 bytes of `amissl_v362.library` off DH0: | **55 ms** |
| `OpenLibrary("amisslmaster.library")` | **3 ms** |
| `OpenAmiSSL()` — `LoadSeg` of the 3.5 MB library | **139 ms** |
| `InitAmiSSLA()` — the per-process init | **0 ms** |
| the first OpenSSL call — its lazy provider/DRBG setup | **0 ms** |

**Opening AmiSSL is cheap — about a fifth of a second, once, and the library then stays
resident for the next program.** That is worth stating because the first four attempts at
this measurement looked like the opposite: the benchmark sat with the CPU busy and no
output for between eight and twenty minutes of emulated time, four times, at three
different clocks. Neither cause was AmiSSL and neither was the emulator.

**The first was a missing math library.** AmiSSL is built against clib2, whose library
initialisation opens `mathieeedoubbas.library` *and* `mathieeedoubtrans.library`
(`src/amissl_libinit.c:780-782`), and Kickstart 3.1's ROM contains `mathieeesingbas` and
nothing else — verified against the 40.68 A1200 image in §11.2. A bare directory hard
drive has neither. With them missing, `OpenAmiSSL()` never returns; with both staged it
returns in 139 ms. What happens inside clib2 in between was not isolated and is not
claimed here — the observation is the one that matters to anyone staging AmiSSL.

What found it was AmiSSL's *own* `OpenSSL` command, staged and run under the same harness:
it prints `mathieeedoubtrans.library could not be opened.` and exits 20 — the loud version
of the same fault. Two further facts fell out of that probe:

- **They have to be a matched pair.** A stock `mathieeedoubbas.library` beside the AROS
  `mathieeedoubtrans.library` still reports the trans library as unopenable. Both from the
  AROS m68k boot ISO and it works — the `OpenSSL` command then gets all the way to its own
  `Couldn't open bsdsocket.library v4!`, which is that command's requirement and not
  AmiSSL's.
- **A real machine is fine and a test rig is not**, which is the same shape of finding as
  §11.2's `mathieeedoubbas` note about curl. Every Workbench install has all four in
  `LIBS:`. Anyone benchmarking AmigaOS software against a bare boot needs to stage them,
  and `tools/amissl-run.sh` now does, with the reason in a comment so the next person does
  not spend the afternoon.

OpenSSL 3.x also initialises itself lazily, on first API use rather than at
`InitAmiSSL` — the default provider, the property cache and the DRBG chain are all built
behind whichever call happens to be first — so the benchmark times a bare
`BN_new()`/`BN_free()` immediately afterwards to put that where it belongs. **It is 0 ms.**
The whole of OpenSSL 3.x's startup on a 68020 is below the E-Clock's millisecond.

**And then the same trap again, one layer down, and this one is the better story.** With
the math libraries staged the benchmark still sat inside its first OpenSSL call for
**twenty minutes** with no output. AmiSSL is configured `OPENSSLDIR=AmiSSL:
ENGINESDIR=AmiSSL:engines MODULESDIR=AmiSSL:modules` (its own `Makefile:492`), so OpenSSL
3.x's configuration and provider loading opens `AmiSSL:openssl.cnf` on the first API call
anyone makes. There was no `AmiSSL:` assign. AmigaDOS does not return an error for an
unknown volume — **it puts up "Please insert volume AmiSSL: in any drive"**, and on a bare
boot with no Workbench and no user there is nobody to cancel it.

Two lines fix it and both belong in any AmigaOS test harness:

```c
((struct Process *)FindTask(NULL)) -> pr_WindowPtr = (APTR)-1;  /* fail, do not ask */
AssignLock("AmiSSL", Lock("DH0:AmiSSL", SHARED_LOCK));
```

The second is what the release's own installer does. The first is what turns *the next*
missing assign from a twenty-minute hang into an error in a second, and it is the one
worth copying.

### 15.7 The measurement

`-k 56`, so the CPU is a cycle-exact 68020 at an implied **56.53 MHz** — measured
in-process by the MULU.L kernel, against `cpucal`'s 56.0. **Every operation agreed with
the other side and with the published vector: 0 failures, 0 mismatches.** That covers the
RSA-2048 public and CRT private results byte for byte against Python-derived known answers
*and* against each other, the ECDH shared secret against the published one, both sides
accepting the RFC 6979 A.2.5 signature, the `k·G` point identical in all 65 bytes, 16 KiB
of AES-128-CBC ciphertext identical, and the HMAC-SHA256 tag identical.

**The harness reproduces §9 to under 0.5%**, which is the check that it is timing the same
computation. Scaling our column by 56.53/13.95 = 4.052 gives 683 ms for the RSA public
operation against §9's 681, 1,965 ms for the ECDSA verify against 1,961, 1,371 for the
ECDH against 1,368, 381 for `k·G` against 381, and 20,150 for the CRT private against
20,050.

| operation | ours | AmiSSL | measured | corrected |
|---|---:|---:|---|---|
| RSA-2048 public, e=65537 | 168.5 ms | 142.7 ms | **AmiSSL 1.18×** | **AmiSSL 1.19×** |
| RSA-2048 private CRT, blinding off | 4.97 s | 5.30 s | ours 1.07× | **ours 1.22×** |
| RSA-2048 private CRT, OpenSSL's default | 4.97 s | 6.88 s | **ours 1.38×** | ours 1.54× |
| ECDSA P-256 verify | 484.9 ms | 840.2 ms | ours 1.73× | **ours 1.69×** |
| ECDH P-256 shared secret | 338.3 ms | 1,049.7 ms | ours 3.10× | **ours 3.03×** |
| k·G, an ECDHE key generation | 94.1 ms | 1,047.4 ms | ours 11.1× | **ours 10.76×** |
| AES-128-CBC, 16 KiB | 85.3 ms | 84.2 ms | AmiSSL 1.01× | — |
| HMAC-SHA256, 16 KiB | 86.9 ms | 111.2 ms | **ours 1.28×** | — |

**It is mixed, and the split is exactly where the code said it would be.**

**AmiSSL wins the RSA public operation, and Karatsuba is why.** Same algorithm on both
sides — no constant-time flag, window 1, sixteen squarings and two multiplies, leading
zeros skipped — and the only difference in the arithmetic is that `bn_mul_recursive` and
`bn_sqr_recursive` turn 126,688 limb multiplies into 103,936. That is 18% fewer multiplies
for 16% less time, which is about as clean an attribution as this kind of measurement
gets. §9 costed Karatsuba for `crypto68k` at ~5% *at the 32 limbs a CRT half runs* and
rejected it; at the 64 limbs a public operation runs, OpenSSL's own numbers say it is
worth about a fifth of the multiplies. **That is the actionable finding for us**, and it
is worth more than it looks: three RSA public operations is what a client does per
handshake.

A second, smaller one from the same row: OpenSSL caches its `BN_MONT_CTX` and we rebuild
R² mod m on every call. With the context cached AmiSSL drops from 142.7 ms to 126.7 ms —
**16.0 ms of setup per operation** — and ours pays a comparable amount inside its 168.5.
Caching it is cheap and nobody has.

**We win the private operation, and constant time is why.** OpenSSL issues 7% *fewer*
multiplies than we do and is still slower, because a fixed window with no zero-skipping
reads the whole 64-entry table before every one of its 171 multiplies — 2,048 volatile
`BN_ULONG` loads a time, about 350,000 per 1024-bit half — where our sliding window
indexes straight into it. And that is before blinding, which is OpenSSL's default and
which the benchmark priced separately: **1.59 s, 30% of AmiSSL's unblinded operation.**
Against the default configuration we are 1.38× faster; against the arithmetic alone,
1.07× measured and 1.22× corrected.

**We win the elliptic curve, and constant time is why again — but much harder.**
`ossl_ec_wNAF_mul` forces a Montgomery ladder for any scalar that could be secret, and
the benchmark proves the source reading rather than citing it: **setting
`BN_FLG_CONSTTIME` on the scalar changed `k·G` from 1,047,399 µs to 1,046,628 µs, 0.07%.**
The flag is ignored because the ladder was already running. 256 steps, one per bit of the
group order, 13 field multiplies and 7 squarings each, 5,120 field operations whatever the
scalar — against our comb's 26 doublings and 50 additions. Eleven times.
`EC_GROUP_have_precompute_mult()` answers **no**, as the source said it would, and it
would not help if it were yes: the generator case is routed to the ladder before
`pre_comp` is ever consulted.

ECDSA verify is the honest middle. Both sides run a variable-time wNAF because both
scalars are public, we issue 12% *fewer* multiplies, and we are 1.69× faster — so about
half of that gap is not multiplies at all. It is the same diagnosis §9 made about
`nx_crypto`, one level less severe: OpenSSL's field elements are `BIGNUM`s with a size
field, a sign, a `bn_correct_top()` after every operation and a `BN_CTX` allocation around
it, where ours are eight limbs in a fixed array. `BN_nist_mod_256` is a good Solinas
reduction and `bn_mul_comba8` is good assembly; the wrapper around them is what costs.

**The bulk path is a dead heat, and that is the most consequential row.** AES-128-CBC is
187 KB/s against 189 — both are the same table-driven C, and neither has a byte of m68k
assembly. HMAC-SHA256 is 183 KB/s against 143, ours ahead by 28%. Encrypting and MACing
one 16 KiB TLS record costs 172 ms our way and 195 ms AmiSSL's: **92 KB/s against
81 KB/s** of application data, 13% in our favour.

That row is where §11's `https` figure comes from. `https` measured 16,464 B/s against
`http`'s 114,598 — and 92 KB/s of record processing at 56 MHz is about 23 KB/s at 14 MHz,
so the record path accounts for most of that ceiling and everything else in the stack
shares what is left. Swapping our bulk crypto for AmiSSL's would move it about a tenth,
in the wrong direction. **Nothing in AmiSSL rescues the bulk path, because nobody has
written AES or SHA-256 assembly for m68k in either tree** — and the
1.28× on HMAC says the plainest thing in this whole section: the largest single lever
available to `https://` on a classic Amiga is still an unwritten 68020 SHA-256.

### 15.8 What this says about the recommendation

§11.6 guessed a well-maintained library would be "1.5–2.5× slower than `crypto68k` rather
than 3–4×". For the handshake that guess was right and slightly conservative:

| | ours | AmiSSL | |
|---|---:|---:|---|
| ECDHE_RSA, 2-cert chain (3 verify + keygen + ECDH) | 938 ms | 2,525 ms | ours 2.6× |
| ECDHE_ECDSA, 2-cert chain (3 verify + keygen + ECDH) | 1,887 ms | 4,617 ms | ours 2.4× |

corrected, 1,062 / 2,722 and 2,037 / 4,871 — 2.5× and 2.3×. At 14 MHz that is 3.8 s
against 10.2 s of arithmetic for an RSA chain and 7.6 s against 18.7 s for an ECDSA one,
which is the difference between a handshake Cloudflare tolerates and one it abandons
(§11.6, §13).

**So the recommendation in §11.6 stands, but for a narrower reason than it was given.**
It was argued on the size of the gap against `nx_crypto`; the real gap against the
ecosystem's actual TLS library is 2.4×, not 8×, and 2.4× is a difference of degree. What
does not change is that the handshake is where a classic Amiga lives or dies against a
front-end timeout, that 2.4× is worth several seconds there, and that `tls.library`
already has the trust store, the host-name check and session resumption (§13) which are
worth far more than any of this.

What *should* change is the direction of the next optimisation. Three things fall out of
the measurement, in order of value:

1. **A 68020 SHA-256.** The bulk path is 92 KB/s and neither implementation has any
   assembly in it at all. This is the only lever in the section that moves `https://`
   throughput rather than handshake latency.
2. **Karatsuba at 64 limbs.** OpenSSL's own numbers say ~18% of the multiplies on an RSA
   public operation, three of which happen per handshake. §9's rejection was measured at
   32 limbs and does not carry.
3. **Cache R² mod m.** 16 ms per RSA operation on AmiSSL's side of the same fence; ours
   rebuilds it every call.

And one thing that should *not* change: `crypto68k` stays variable-time. AmiSSL is
constant-time on the private and ephemeral paths and that is most of what it costs — the
ladder, the fixed window, the full-table gather, the blinding. For a vintage machine on a
LAN (§9's threat model) that is a defence with no attacker, and we already say so in the
headers of both modules. It is a trade, and it is the trade this project made on purpose.

### 15.9 Sources, and how to run it again

- [jens-maus/amissl](https://github.com/jens-maus/amissl) — tag `5.27`; the OS3 runtime
  and the SDK are separate release assets, and `crypto/bn/asm/bn_m68k.s` exists only in
  the source tree, not in either archive.
- Howard Chu, *M68020 bn_asm*, openssl-dev, 2002 —
  [marc.info/?l=openssl-dev&m=101407286200398](https://marc.info/?l=openssl-dev&m=101407286200398).
  The same 1604 lines AmiSSL builds today.
- MC68020UM / MC68030UM instruction-timing appendices, for the 43/45-cycle `MUL.L` the
  correction is anchored to.

Reproducing it, from a clean tree:

```
curl -LO https://github.com/jens-maus/amissl/releases/download/5.27/AmiSSL-5.27-SDK.lha
curl -LO https://github.com/jens-maus/amissl/releases/download/5.27/AmiSSL-5.27-OS3.lha
lha x AmiSSL-5.27-SDK.lha ; lha x AmiSSL-5.27-OS3.lha
bsdtar xf aros-amiga-m68k.iso Libs/mathieeedoub{bas,trans}.library   # into build/amissl-mathlibs/

cmake -S . -B build/cm-tls -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
      -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_TLS=ON \
      -DAMINETXDUO_AMISSL_SDK=<where>/AmiSSL/Developer
cmake --build build/cm-tls --parallel --target crypto68k_amissl

AMINETXDUO_AMISSL_OS3=<where>/AmiSSL ./tools/amissl-run.sh -t 2400 -k 56
```

`-k 56` is a shakedown clock; the ratios are clock-independent and the corrected column
is derived from a `t_mulu` measured in the same run, so it holds at 14 MHz too. Drop the
`-k` for A1200 absolutes and budget roughly four times the wall clock.
