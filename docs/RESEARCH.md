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
| `tx_timer_interrupt.c` | driven by a dedicated tick task on `timer.device` (or a VBlank server) at `TX_TIMER_TICKS_PER_SECOND` = 100; `NX_IP_PERIODIC_RATE` follows it |
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

### Still open (lower stakes, decide during implementation)

- Does `usergroup.library` ship as a real user database or as the usual single-user stub
  (`root`/uid 0)? Most Amiga software only needs the calls to succeed.
- `bpf_*`: full BPF VM, or the common-case filter subset (`ether proto`, host/port
  matching) with a documented gap?
- IPv6 default: built and off, or built and on when router advertisements appear?

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
