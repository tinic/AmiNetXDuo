# AmiNetXDuo — Feasibility Research

*An AmiTCP/Roadshow-compatible `bsdsocket.library` for AmigaOS built on Eclipse ThreadX NetX Duo.*

Status: no longer research only — this began as a feasibility study and the thing now
builds, runs, passes 125/142 of an independent conformance suite and carries upstream
curl. The early sections are kept as they were written, because how a conclusion was
reached matters when a later section overturns it, and several have been overturned.
Where that has happened the later section says so.

Empirical results in [§5.4](#54-empirical-build-spike-m68k) were produced on 2026-07-24
with the local `m68k-amigaos-gcc 15.2.0` toolchain, against throwaway port headers that
no longer exist; the real port is in `port/threadx-amiga/` and `port/netxduo-amiga/`.

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

**Measure the cold column on a quiet host.** These runs reach a real server over SLIRP, so
the figure is not purely emulated cycles, and repeats taken while two other FS-UAE
instances shared the machine read 12.1 s and 28.7 s for the same two hosts. The resumed
column is unmoved by that — 589 to 620 ms across every run in this work, contended or not,
because there is almost nothing in it to contend for.

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

**Entries are keyed by host, port and a fingerprint of the whole trust decision** — see
§13.6.1, because getting that wrong was a security defect and not a detail.

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

#### 13.6.1 Keying on the trust decision, and the defect that came of not doing it

The first version keyed on host, port and a **boolean** saying verification had happened.
That is a trap with a name on it: it records *that* a chain was checked, not *what it was
checked against*. Found by the curl verification suite, RSA host, cold handshake 5.68 s:

| trust store offered | expected | got |
|---|---|---|
| the correct store, cold | 200 | 200 |
| a valid store holding a root that signed **nothing** in the chain | 60 | **200** |

The second row is the defect. The session cached by the first case was resumed under a
different store, and **a resumed handshake verifies nothing** — no certificate is sent, no
signature is checked, no host name is compared. So the library returned a connection it
called verified, against roots the caller never offered. Cold verification was never
broken: expired, self-signed and wrong-host were all still refused. It was only the second
connection that stopped checking, and it survived a reboot through the disk mirror.

**The key now names the decision completely.** What went in, and why:

| in the key | why |
|---|---|
| the trust store's **identity** | the whole point: FNV-1a over the index's count and every (subject-name hash, offset, length) record |
| `TLSA_NoVerify` | two populations that must never mix |
| whether validity **dates** were checked | skipped on a clockless machine, so setting your clock must not silently fail to start checking expiry |
| `TLSA_MaxChain` | the cautious reading — a session verified over an eight-deep chain is not one a caller limiting itself to two would have established |
| host name, port | already the primary key |

What stayed out, because a key that includes things which do not affect trust only costs
resumptions: `TLSA_Error` is an output pointer, `TLSA_Timeout` is liveness,
`TLSA_RecordBuffer` is a buffer size, `TLSA_NoResume` turns the machinery off rather than
parameterising it, `TLSA_SessionFile` selects *which* cache and is handled by the library
base reloading when it changes.

**Cost.** The fingerprint is computed in `tls_store_open()`, where the whole index is
already in memory because it was just read off the disk — one pass over 1,428 bytes for
the Mozilla set, once per connection, against a handshake that spends seconds on
arithmetic. Hashing the 126 KB of DER instead would have cost more than the resumption
saves, which is the trade that made the index the right object to hash.

**What that does not protect against, stated rather than implied.** Two stores whose
indexes agree record for record but whose certificate DER differs — someone rewriting a
root in place, at the same offset and length, under the same subject Name. That is an
attacker who can already write the trust store, and such an attacker owns verification
outright: they would simply add a root of their own. It is not a new exposure. It also
does not distinguish two different *files* holding the same roots, which is correct rather
than a gap — the same root set is the same trust, and keying on the path would lose
resumptions to an assign or a copy without buying anything.

**The disk format moved with it, `ATS1` → `ATS2`.** An `ATS1` record holds a key this code
must not trust, and the new layout would misread every field after the master secret. An
unrecognised magic is ignored rather than reinterpreted: every connection becomes a full
handshake and the next session written replaces the file.

The regression test is in `tests/tls/`, and it tests both directions.
`run-resume.sh` stages a **second, valid** trust store holding one unrelated self-signed
root — real enough to open, wrong enough to be useless — and `tls_resume.c` checks that a
cached verified session is refused under it, with an `UNTRUSTED` reason, and then that the
**correct** store still resumes and still transfers data. A fix that simply stops resuming
is not a fix.

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

**A different trust store cannot inherit a verification, proved** — §13.6.1, and the check
runs in both directions in the same test so a pass cannot come from having simply stopped
resuming.

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

The suite asks the same question twice, once before anything has spoken to the
host and once after everything has, and the two answers differ:

| case | trust store offered | order | rc | `time_appconnect` |
|---|---|---|---|---|
| `e01` | a valid store holding a root that signed **nothing** in the chain | cold | **60 refused** | — |
| `e02` | none at all — `DEVS:Internet/certificates`, the real Mozilla set | cold | **60 refused** | — |
| `rsa2.test` | the correct store | cold | 0 | **5.68 s** |
| `e23` | the same wrong store as `e01`, byte for byte | warm | **0, HTTP 200** | **0.70 s** |
| `e24` | the same absence as `e02` | warm | **0, HTTP 200** | **0.70 s** |

**Identical commands. Opposite outcomes. One variable: whether a session was
already in the cache.** A full RSA handshake here is 5.68 s and a resumed one
is 0.70 s, so `e23` and `e24` did no public-key work and verified no
certificate — which is what resumption is *for*, and precisely the problem.
`tls.library` keys its cache on `TLSA_HostName` alone, so the trust decision
is cached alongside the session and reused under a different `--cacert`, or
under none.

Cold verification is in good order and the same run proves it: an expired leaf,
a self-signed leaf, a certificate issued to another host, a store with the
wrong root and no store at all are all refused with curl's own exit 60. It is
only the second connection that stops looking.

Two things make it worth more than a curiosity. The cache is **mirrored to
`DEVS:Internet/tlssessions`**, so it survives a reboot — §13's headline result
depends on exactly that. And `--cacert` is the switch a user reaches for when
they do *not* trust the default store, which is the case where being ignored
matters most.

**Fixed while this was being written**, in `src/tlslib/` rather than here
(292f391): the cache key now covers the identity of the trust store's root set,
whether the chain and host name were verified, whether validity dates were
checked, and the accepted chain depth. Re-run against it, the both-orders
design is the acceptance test and it passes:

| case | before | after |
|---|---|---|
| `e01` wrong store, cold | 60 | **60** |
| `e02` no store, cold | 60 | **60** |
| `e23` wrong store, warm | **0, HTTP 200** | **60** |
| `e24` no store, warm | **0, HTTP 200** | **60** |
| `e25` `-k` cached, then without `-k` | — | **60**, `appconnect` 0.0 — not resumed at all |

**One of those expectations needed settling rather than assuming, and it is
worth recording which way it went.** `e24` passes no `--cacert`, and if that
resolved to the same store which cached the session then resuming would be
*correct* and 200 would be the right answer. Read from the source rather than
guessed: `amitls.c:391` passes `TLSA_TrustStore` only when `CAfile` is set, and
`tls_conn.c:392` falls back to `TLS_DEFAULT_STORE` when it is not — and the
build bakes `CURL_CA_BUNDLE` with that same path, so both routes name
`DEVS:Internet/certificates`. Every earlier case to that host passed
`--cacert DH0:teststore`. **Different root sets, so 60 is right**, and the case
now says so in a comment. A suite that encodes a wrong expectation is worse
than one case short, because the next person to touch resumption would "fix" a
non-bug to make it pass.

`e25` is the same question for a different bit of the key: `e08` reaches
`wrong.test` with `-k` and caches a session nothing verified, and coming back
without `-k` must not resume into acceptance. It does not.

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
group E (TLS, hermetic)           28 passed, 0 failed,  28 cases
```

**Both of those failures are real and neither is a false alarm**, so they are
listed rather than explained away (group E's two are gone: the trust-store
defect of §14.4 was fixed and the cases now pass):

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

#### Its HTTPS did not complete a case, and that is AmiSSL rather than us

Group E was run against the same binary with AmiSSL staged and **not one case
finished**. The host peer logged sixteen handshake attempts in nine minutes —
one about every thirty-two seconds, each ending with the guest closing the
connection mid-handshake — and the Amiga side never got as far as writing an
exit code. Not diagnosed, and deliberately not chased: OpenSSL 3.6.2's generic
bignum on a 14 MHz 68020 is the subject of §15, `--cacert` was being handed a
PEM rather than the indexed store our own backend takes, and none of it is the
socket layer. **The stack carried all sixteen attempts without incident**,
which is the only part of it this suite is entitled to claim.

Our own curl over `tls.library` does the same group in 25 cases and a few
minutes, which is the comparison worth remembering when anyone proposes
importing a TLS library instead of writing a backend (§11.6).

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

Ours is SOS, with the product split by Karatsuba at 64 limbs and schoolbook below —
§15.9 is how that threshold was arrived at, and why it is one level and not three.
AmiSSL's differs in a way that matters: because `OPENSSL_BN_ASM_MONT` is **not** defined
for this target, `bn_mul_mont` does not exist, and `bn_mul_mont_fixed_top()` falls through
to `bn_mul_fixed_top()`/`bn_sqr_fixed_top()` plus `bn_from_montgomery_word()`. Two
consequences, both in OpenSSL's favour: a genuine squaring shortcut, and **Karatsuba above
sixteen limbs** (M(8·2ᵏ) = 64·3ᵏ, S(8·2ᵏ) = 36·3ᵏ). At the 32 limbs an RSA-2048 CRT half
runs, OpenSSL issues about 22% fewer multiplies per Montgomery step than we do.

**Per operation:**

| operation | ours | AmiSSL | ours ÷ theirs |
|---|---:|---:|---:|
| RSA-2048 public, e = 65537 | 120,104 | 103,936 | 1.16 |
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
| RSA-2048 public, e=65537 | 139.5 ms | 142.7 ms | **ours 1.02×** | AmiSSL 1.004× |
| RSA-2048 private CRT, blinding off | 4.97 s | 5.30 s | ours 1.07× | **ours 1.22×** |
| RSA-2048 private CRT, OpenSSL's default | 4.97 s | 6.88 s | **ours 1.38×** | ours 1.54× |
| ECDSA P-256 verify | 484.9 ms | 840.2 ms | ours 1.73× | **ours 1.69×** |
| ECDH P-256 shared secret | 338.3 ms | 1,049.7 ms | ours 3.10× | **ours 3.03×** |
| k·G, an ECDHE key generation | 94.1 ms | 1,047.4 ms | ours 11.1× | **ours 10.76×** |
| AES-128-CBC, 16 KiB | 85.3 ms | 84.2 ms | AmiSSL 1.01× | — |
| HMAC-SHA256, 16 KiB | 86.9 ms | 111.2 ms | **ours 1.28×** | — |

**It is mixed, and the split is exactly where the code said it would be.**

**AmiSSL wins the RSA public operation, and it is NOT the multiplies — it is the setup.**
That was this section's first conclusion and it was wrong; §15.9 records adopting
Karatsuba on the strength of it and then measuring what it actually bought. The
decomposition, all of it measured in the same run:

| | ours | AmiSSL |
|---|---:|---:|
| exponentiation, 16 squarings + 3 multiplies | 127.3 ms | 126.9 ms |
| setup — R² mod m, built per call by both | **11.9 ms** | 16.0 ms |
| total | **139.5 ms** | 142.7 ms |

Both figures moved after this section was first written, and §15.9 and §15.10 are the two
pieces of work that moved them. The exponentiation was 131.6 ms and is level with
OpenSSL's after Karatsuba; the setup was **36.6 ms** and is now 11.9 after the divider was
rewritten. Both sides rebuild R² mod m on every call here, so this is like for like, and
OpenSSL's own cached-context figure of 126.9 ms is what isolates the two halves.

**The measured result is ours by 1.02× and the corrected one is theirs by 1.004×.** That
is a dead heat and should be read as one: on this operation the two implementations are
now the same speed to within the correction's own uncertainty.

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
| ECDHE_RSA, 2-cert chain (3 verify + keygen + ECDH) | 850 ms | 2,525 ms | ours 2.9× |
| ECDHE_ECDSA, 2-cert chain (3 verify + keygen + ECDH) | 1,887 ms | 4,617 ms | ours 2.4× |

corrected, 971 / 2,723 and 2,036 / 4,871 — 2.8× and 2.3×. At 14 MHz that is 3.8 s
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
2. **A 32-bit long division — done, and it is what reversed the RSA public result.**
   See §15.10. Worth 24.4 ms of a 164 ms operation, 3.05× on the setup itself.
3. **Karatsuba — done, and worth 2.7%.** See §15.9. Kept, but it is not the lever the
   multiply count made it look like.

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

### 15.9 Adopting Karatsuba, and what it was actually worth

§15.7 originally blamed the RSA public gap on OpenSSL's Karatsuba, so `src/crypto68k/`
grew one. It works, it is correct, and it is worth **2.7%** — an order of magnitude less
than the limb-product count predicts. Both halves of that are worth writing down.

**The prize was smaller than it looked before a line was written.** `c68k_sqr()` has
always been a dedicated symmetric squarer — n(n−1)/2 off-diagonal products accumulated
once, doubled, plus n diagonal squares — so the free ~2× that a naive "squaring by
calling multiply" would have left on the table was already banked. And a Montgomery step
is a product *and* a reduction, where the reduction is a chain of scalar-by-vector
`c68k_addmul_1` calls that Karatsuba cannot touch at all. So 2.14× on the raw squaring of
64 limbs dilutes to 1.22× on the Montgomery square before any code runs.

**The crossover had to be measured, and it is shallow.** The benchmark makes the
threshold a runtime variable and sweeps it, because a number copied from another project
would have been wrong in both directions here (T means: split while the operand is ≥ T
limbs, so T = n is one level and T = 8 is four):

| 64 limbs | schoolbook | T=64 | T=32 | T=16 | T=8 |
|---|---:|---:|---:|---:|---:|
| Montgomery square | 6,651 µs | **6,506 (1.02×)** | 6,714 (0.99×) | 7,416 (0.89×) | 9,144 (0.72×) |
| Montgomery multiply | 8,456 µs | 7,733 (1.09×) | **7,444 (1.13×)** | 7,745 (1.09×) | 9,057 (0.93×) |

| 32 limbs | schoolbook | T=32 | T=16 | T=8 |
|---|---:|---:|---:|---:|
| Montgomery square | 1,852 µs | 1,912 (0.96×) | 2,157 (0.85×) | 2,733 (0.67×) |
| Montgomery multiply | 2,256 µs | **2,160 (1.04×)** | 2,245 (1.00×) | 2,670 (0.84×) |

**One level, and only at 64 limbs.** Every level past the first costs more than it saves,
and at four levels the split is 28% *slower* than schoolbook — the recombination is O(n)
per level with a real constant, and by the time the operands are 8 limbs it dwarfs the
products it removes. §9's rejection at 32 limbs was right and remains right: the square
loses there and the multiply gains 4%. OpenSSL reaches 103,936 limb products by recursing
three levels to `bn_sqr_comba8`; we stop at one and reach 115,680, and stopping is the
faster choice on this machine.

The multiply's arithmetic is worth the whole of its theoretical saving and the square's is
worth about half: at 64 limbs two levels removes 1,792 limb products from a multiply, which
at the emulator's 32.14 cycles is 1.02 ms, and the measured saving is 1.01 ms. One level
removes 496 from a square, predicting 0.28 ms, and the measured saving is 0.145 ms.

**Correctness.** The vendored `_nx_crypto_huge_number_mont()` could not be the oracle for
this, and finding that out was most of the work. It is **wrong** for operands within a
whisker of the modulus — with m = 2⁶⁴−1 and x = m−1, `mont(x,x)` must be 1 and it returns
0; at 32 limbs with m nearly all ones it gets the top limb one too low. Both checked
against an independently computed answer, not against either implementation. Random
operands never come that close to m, which is why a 400-trial sweep never caught it and
why no RSA or EC path can reach it — but those are exactly the operands Karatsuba's carry
and borrow handling most needs testing on. So `c68k_karatsuba_limbs` doubles as the test
hook: the suite computes each result twice in the same process, once schoolbook and once
with the split forced down to 2-limb leaves, and compares. Six widths including odd ones,
eight operand shapes chosen to drive the carries (m−1, x1 == x0, either half zero,
alternating limbs, 0 and 1), multiply and square: **0 mismatches**, and 400 random trials
at 1..64 limbs additionally agree with the vendored routine. On target, 0 mismatches
against AmiSSL across the whole suite.

One thing fixed on the way past: the host test's own `addmul` section swept n to 70 while
writing into 64-limb arrays, and had been reporting 316 of its own 4,000 trials as
failures — 7.9%, against the 6/71 = 8.45% of draws that overrun. `ctest` had been red for
this reason and not for a real one.

**The shape of the answer, which is the point.** Karatsuba took our exponentiation from
131.6 ms to 127.3 ms and put it level with OpenSSL's 126.9. It did not move the headline,
because the headline was never the multiplies: 98% of what remains is a 16-bit long
division in the setup. Adopting it was right — it is 2.7% and it is now the reason our
exponentiation is not behind — but the instruction to reverse the RSA public gap is
answered by `_nx_crypto_huge_number_modulus()`, not by this.

### 15.10 The R² mod m setup: a 16-bit long division, and 3.05× for fixing it

§15.9 ended by saying the RSA public gap was not the multiplies but a 16-bit long
division in the setup, and that `_nx_crypto_huge_number_modulus()` was the thing to fix.
It was. `src/crypto68k/c68k_div.c` is the same algorithm — Knuth's algorithm D, a
two-digit quotient estimate, normalisation, add-back — at the machine's own word size:

| R² mod m | vendored, 16-bit digits | ours, 32-bit digits | |
|---|---:|---:|---|
| 64-limb modulus (RSA-2048 public) | 36,336 µs | **11,901 µs** | **3.05×** |
| 32-limb modulus (a CRT half) | 9,115 µs | **3,307 µs** | 2.75× |

**And that reverses the headline.** RSA-2048 public goes 163.9 → **139.5 ms** against
AmiSSL's 142.7 — ours by 1.02× measured, theirs by 1.004× corrected, which is a dead
heat. An ECDHE_RSA handshake's arithmetic goes 938 → **850 ms** against 2,525, ours by
2.9×.

**Why it is worth 3×, and it is not the divide instruction.** Halving the digit size
doubles the number of quotient digits *and* doubles the length of the multiply-subtract
pass under each one, so 16-bit digits are about four times the inner work of the same
algorithm over 32-bit limbs. The quotient estimate itself barely matters: a 64-limb setup
issues **67** `DIVU.L` in total, which at the emulator's 921 ns is 61 µs of an 11,901 µs
setup — **0.5%**. Anyone reading this expecting `bn_div_words` to be the secret should
know that it is not; the limb width is.

**`DIVU.L` was calibrated before being trusted, and it is discounted harder than
`MULU.L`.** The benchmark times it in-process with an inline-assembly kernel, the same
way it does the multiply: **921.9 ns**, which against `MULU.L`'s 568.6 ns and its known
32.14 cycles works out at **51.8 cycles where the MC68020UM says 78** — a 34% discount,
larger than `MULU.L`'s 29%. It is folded into the same correction as an equivalent
multiply count rather than given a column of its own, because at 0.5% of the setup it is
below the noise; the calibration is reported so that the next person does not have to
assume it.

**The 68060 rule is unchanged and now covers one more instruction.** `DIVU.L` is in the
same class as `MULU.L` 32×32→64: real on a 68020, 68030 and 68040, **not implemented on a
68060**, where it traps to the emulator. So it lives in `c68k_prim.S` under the existing
`AMINETXDUO_CRYPTO68K_ASM` guard with a portable C fallback beside it, and that option
must still never be enabled for a 68060 build. `c68k_submul_1`, the multiply-subtract
inner loop, is deliberately left to the compiler — GCC emits `MULU.L` for it, the same
finding that kept `c68k_addmul_1_c` within 1.4× of hand-written assembly, and the
measurement above says the estimate is not where the time goes anyway.

**Caching R² was costed first and rejected.** It would have avoided the division rather
than speeding it up, and it buys nothing for a client: the three RSA public operations in
a handshake verify the leaf with the intermediate's key, the intermediate with the root's,
and the ServerKeyExchange with the leaf's — **three different moduli**
(`_nx_secure_x509_certificate_chain_verify` walks "each issuer back to" the root), so a
cache keyed on the modulus never hits inside a handshake. Across handshakes to one host,
session resumption (§13) does no public-key work at all, so it would never be consulted
there either. Two other division-free routes were priced and are worse than what they
replace: R² by repeated modular doubling from R mod m is 2,048 shift-and-subtract passes
over 64 limbs, and by a Montgomery-squaring ladder is eleven Montgomery squares — ~42 ms
and ~72 ms against the 36.6 being replaced.

**The CRT private operation barely moves**, and the earlier guess that this would "pay
again" there was wrong: 4,976 → 4,951 ms, about 0.5%. Two 32-limb setups are a rounding
error next to two 1,024-bit exponentiations. The setup was 22% of the *public* operation
because that operation is short.

**Correctness.** 600 trials against the vendored divider — which, unlike the vendored
Montgomery, has no known defect — across moduli of 1..32 limbs and dividends up to twice
that, in six shapes chosen to reach the two paths random operands almost never do: the
`B−1` clamp, where the partial remainder's top limb equals the divisor's and a `DIVU.L`
would **trap** rather than saturate, so the code must test before dividing; and the
add-back, where the estimate is one too large. Plus unnormalised divisors so the shift
path runs, single-limb divisors, all-ones, and dividends shorter than the modulus. **0
mismatches**, 5,660 host checks total, and 0 mismatches against AmiSSL on target.


## 16. Packet capture, and what TCP is actually doing (2026-07-26)

Three defects found in the week before this one all presented as *slowness* while
being something structurally worse underneath: a SANA-II receive queue four frames
deep that lost 87 of 232 concurrent transfers while TCP hid it in retransmissions;
a teardown freeing a reply port, pinned packets and the stack of a thread still
running on it, visible only as a 15-second pause; and a resumed TLS handshake that
verified nothing, visible only as being fast. The common cause is not carelessness,
it is that **every instrument in this tree measured outcomes rather than mechanism**,
so anything TCP can paper over was invisible.

This section builds the instrument that ends that, and then uses it.

### 16.1 The first finding is the instrument itself: `bpf_*` was never called

`src/bpf/` is 46 KB of capture channels, a BPF filter VM and a validator, with 201
unit-test checks. It was reachable from nothing:

- **No tap call in `src/sana2/`, in either direction.** The two call sites are named
  and located, with the exact arguments, in a block comment in
  `include/aminetxduo/bpf.h` — "THE TWO CALLS `src/sana2/` MUST ADD" — and neither
  existed.
- **`ami_bpf_attach_interface()` had no caller**, so no interface was ever registered
  and `BIOCSETIF` could not have succeeded against any name.
- **All eight `bpf_*` LVOs** (0x16e–0x198) pointed at `bsd_enosys()`.
- **The `aminetxduo_bpf` archive was linked by `tests/mbuf_bpf` and by nothing that
  ships** — not by `bsdsocket.library`, not by the netstack.

So the answer to "can `bpf_*` do this?" was *no*, for the most ordinary reason: it
had been written, tested against synthetic input, and never connected. Both ends are
connected now and there is a consumer, which is what turned 201 green checks into a
subsystem that has met a real workload.

### 16.2 Two capture points, and why the second is not optional

| | where | sees |
|---|---|---|
| `eth0` | the SANA-II taps, at the top of `ami_sana2_rx_deliver()` and after the raw block in `ami_sana2_tx_send()` | every frame that crosses a wire, in the shape the device saw it, ARP included |
| `lo0` | NetX Duo's IP packet filter, installed by `src/netstack/netstack_capture.c` | loopback |

The second one is the interesting one. NetX Duo's loopback interface has
`nx_interface_link_driver_entry == NX_NULL` (`nx_ip_create.c:157`), and
`_nx_ip_driver_packet_send()` shortcuts a loopback destination straight into
`_nx_ip_packet_deferred_receive()`. **No driver is called at all**, so no tap on a
driver can ever see loopback traffic — and loopback is the path every throughput
figure in §11 was measured on. Without `NX_ENABLE_IP_PACKET_FILTER` the fastest path
in the stack would have been the one path with no instrument on it.

The `lo0` tap fires on `NX_IP_PACKET_OUT` only. A loopback datagram is sent once and
received once; capturing both directions would put two identical records in the file
and every analyser downstream would call the second one a retransmission.

Both write **DLT_EN10MB**, so one pcap writer, one filter program and one set of eyes
in Wireshark serve both. `lo0`'s fourteen bytes are synthesised with zeroed addresses.

`NetTrace` is the consumer: it runs a workload and captures it to a **classic pcap
that opens in Wireshark and tcpdump with no conversion**. It links no part of `src/`
— every call is a published `bsdsocket.library` LVO, which is the point, because a
tool that linked the archive would get its own copy of the channel table and capture
nothing. It is one program rather than a daemon plus a workload because the trace
exists to explain a throughput number, and a number and a trace from two separate
runs are two experiments.

**The snap length is the filter's return value.** The bpf ABI has no `BIOCSSNAPLEN`
and 4.4BSD never needed one: a filter program answers with the number of bytes to
keep, so `BPF_RET|BPF_K, 96` accepts every packet and truncates it to 96 bytes.

### 16.3 What is capturable on the host side of FS-UAE's SLIRP

Stated plainly, because it decides what an independent view can be:

- **FS-UAE 3.2.35 has no packet-dump option.** Not on the command line (`--help`
  prints a copyright banner and a URL), not as a `uae_*_pcap` config key, and libpcap
  is not linked. The only `pcap` strings in the binary are three Windows-only winpcap
  failure messages.
- **`tcpdump` on the host is not an alternative.** SLIRP is user-mode NAT *inside* the
  emulator process, so none of the guest's own framing — no Ethernet header, no ARP,
  no DHCP, and not the guest's TCP headers — ever reaches a host interface. What the
  host would see on `lo0` is SLIRP's re-originated connection, which is a different
  TCP conversation. And `/dev/bpf` on this machine needs a password.
- **What does exist, and is better:** the emulated A2065 writes every frame it
  handles, both directions, complete, as hex into `<base_dir>/Cache/Logs/
  fs-uae.log.txt` — **unconditionally** whenever `network_card = a2065`, with no
  option to request it and none to suppress it. `tests/trace/a2065pcap.py` converts
  it. That output is produced inside the emulated hardware, below every line of our
  code, so it is independent by construction: a frame that appears there and not in
  the guest's own pcap was lost between the card and NetX Duo, which is the exact
  shape of the `AMI_SANA2_RX_DEPTH_IPV4` defect.
- **What it cannot tell you: there are no timestamps.** Not coarse — absent. The
  converter stamps records with a counter so the file opens and the ORDER is right,
  and `tests/trace/tcpaudit.py` detects that synthetic clock and suppresses every
  timing rather than printing percentiles computed from a fiction. Take timing from
  the guest pcap, which has real `GetSysTime()` microseconds; take loss and ordering
  from the host one.
- One more caveat, checked rather than assumed: the `SRC:` field of the emulator's
  own `A2065<-`/`A2065<*` header lines is printed from the wrong offset. The hex
  dumps are right; the header lines are not, and the converter ignores them.

**The two views agree packet for packet.** On a 524,288-byte transfer over the wire,
both report 365 segments carrying 524,393 bytes, the same MSS both ways, the same
window minima and maxima, and **zero retransmissions**. That agreement is the
strongest single statement in this section: nothing is being lost between SLIRP, the
A2065, `a2065.device`, the SANA-II shim and NetX Duo.

### 16.4 Retransmissions and duplicate ACKs: there are none

Every trace taken, in both directions, on both paths, in both views:

| | segments | retransmitted | duplicate ACKs |
|---|---|---|---|
| loopback, 524288 B | 128 | **0** | 1 |
| wire, 524288 B, guest view | 365 | **0** | 0 |
| wire, 524288 B, host view | 365 | **0** | 0 |

`bs_drop` on the capture channel was 0 in every run, so the traces have no holes.
**The RX-depth fix is complete and there is no residual loss.** The IPv4 read queue
comes out 32 deep on the 8 MB profile (`sana2: IPv4 read queue 32 deep (pool 256
packets)` in the serial log), and at that depth a 365-segment bulk receive loses
nothing.

This is stated as a result and not as an absence of news: it is the direct successor
to the defect that started all of this, and it is now measured rather than assumed.

### 16.5 The window, and what actually governs it

We advertise **8192 bytes** (`BSD_TCP_WINDOW`, `src/bsdsocket/bsdsocket_internal.h`),
passed to every `nx_tcp_socket_create()`. It is not adaptive; `SO_RCVBUF` moves the
receive *queue* depth but not this number.

`tcpaudit.py` measures the thing that settles it: **unacknowledged bytes at the
moment each segment left, against the window the other side had advertised.**

| A1200, 14 MHz, 524288 B | max in flight | advertised | |
|---|---|---|---|
| **loopback** | 4096 | 4096 | **100% — window-limited** |
| wire | 7200 | 8192 | 88% |

- **Loopback is window-limited, flatly.** Exactly one 4096-byte segment in flight,
  and a **14.9 ms median gap** between segments in which the sender has nothing it is
  allowed to do. Raising the window to 32 KB moves loopback from **297 to 352 KB/s
  (+18%)** and drops occupancy to 33%.
- **The wire is not.** With prompt acknowledgements the peer never has more than
  **2880 bytes** outstanding — 9% of a 32 KB window, 35% of an 8 KB one. At 179 KB/s
  and a 1440-byte segment that is 8 ms per segment, which is the receive pipeline's
  own cost, not a window stall. So the ~117 KB/s that §11 measured through curl, and
  the 161–179 KB/s `NetTrace` measures without curl's copies, are **not** capped by
  the advertised window.

**The window never reached zero on the shipped configuration.** It does now, on
loopback, as a consequence of §16.6 — see there.

### 16.6 Delayed ACK, Nagle, the tick — and the defect

**The tick first, from the running system rather than from a comment.** The serial
log prints it at startup:

```
[INFO] tick: 50 Hz from timer.device unit 1 (48.00 Hz wakeups), E-Clock 709379 Hz
```

**50 Hz**, one tick every 20 ms, matching `NX_IP_PERIODIC_RATE` in `nx_user.h` and
`TX_TIMER_TICKS_PER_SECOND`. The 100 Hz in the README diagram is stale. NetX Duo
derives its own rates from it: the fast periodic timer is `50/10` = 5 ticks = 100 ms,
and the delayed-ACK timer `50/5` = 10 ticks = **200 ms**.

**Nagle does not exist.** Not "is disabled" — the string `nagle` does not appear
anywhere in the vendored NetX Duo tree, and `setsockopt(TCP_NODELAY)` returns success
without doing anything, which is honest because the behaviour is always no-delay. The
trace agrees: 32 separate 4096-byte segments went out on loopback with an ACK
outstanding, which a Nagle implementation would have coalesced.

**And the periodic tick does not pace round trips.** That was §11's explanation for
the wire path scaling sub-linearly with clock, offered as a hypothesis and never
confirmed. It is now disconfirmed: with the fix below, the median ACK delay on the
wire is **2.0 ms** and 151 of 289 acknowledgements go out inside **2 ms**. Nothing is
quantised at 20 ms. Whatever the residual ceiling above ~15 MHz is, it is not the
50 Hz tick.

#### The defect: RFC 1122's ACK rule is not implemented, so ACK latency scales with the window

`NX_TCP_ACK_EVERY_N_PACKETS` is defined nowhere in the vendored tree, so the whole
`need_ack` block in `nx_tcp_socket_state_data_check.c` is compiled out and
**4.2.3.2 — "acknowledge at least every second full-sized segment" — is simply
absent.** What remains acknowledges on two triggers, and neither is per-segment:

1. a **window update**, sent only once the receive window has re-opened by **half of
   `nx_tcp_socket_rx_window_default`** (`nx_tcp_socket_state_data_check.c:1135`,
   `nx_tcp_socket_receive.c:212`);
2. the **200 ms delayed-ACK timer**.

So **the interval between acknowledgements is proportional to the window**, and when
the application cannot consume half a window inside 200 ms the timer becomes the
pacer. The consequence is worse than the delay: it makes the obvious remedy for a
small window — enlarging it — actively harmful. Measured over the wire, 524288 bytes,
changing `BSD_TCP_WINDOW` from 8192 to 32768 and **nothing else**:

| | 8 KB | 32 KB |
|---|---:|---:|
| throughput | 161 KB/s | **89 KB/s** |
| ACK delay p50 | 6.7 ms | **71.4 ms** |
| ACK delay p90 | 8.7 ms | **187.4 ms** |
| ACKs in the 200 ms bucket | 1 of 122 | **26 of 59** |
| longest duplicate-ACK run | 0 | **14** |
| longest gap between data segments | 141 ms | **1361 ms** |
| retransmissions | **0** | **0** |

**Zero retransmissions in both columns.** Nothing was lost; the sender was waiting,
and no instrument in this tree before now could have told those two apart. A
45% throughput regression from raising a window, with no loss anywhere and a
fourteen-deep run of duplicate ACKs, is precisely the class of thing the last three
defects were.

#### The fix, and what it is and is not worth

`#define NX_TCP_ACK_EVERY_N_PACKETS 2` in `nx_user.h`. One ULONG per socket that the
struct already carries and `nx_tcp_socket_create.c:154` already initialises, plus one
comparison per received data segment.

| wire, 8 KB window, 524288 B | before | after |
|---|---|---|
| ACK delay p50 / p90 / max | 6.7 / 8.7 / 137.8 ms | **2.0 / 9.9 / 14.7 ms** |
| ACKs inside 2 ms | 1 of 122 | **151 of 289** |
| peer bytes in flight | 7200 of 8192 (88%) | **2880 of 8192 (35%)** |
| retransmissions | 0 | 0 |
| throughput | 161–174 KB/s | 163–174 KB/s |

#### Does this explain the sub-linear wire scaling? No — and that is worth as much

§11 fitted a fixed ceiling against three honest clock points (105 / 174 / 239 KB/s at
6.80 / 13.95 / 24.48 MHz) and concluded that **above about 15 MHz roughly half of what
is left is not CPU at all**, attributing it to "the 50 Hz IP periodic tick and the
round trips paced by it" — offered as a hypothesis and never confirmed. The obvious
successor hypothesis, once the ACK rule above was found, is that the missing rule was
that ceiling.

**It is not.** Both are now disconfirmed by the same traces:

- **The tick does not pace round trips.** With the fix, 151 of 289 acknowledgements go
  out inside 2 ms and the median is 2.0 ms. Nothing is quantised at 20 ms, and the
  200 ms delayed-ACK timer fires once in a 365-segment transfer, not repeatedly.
- **The ACK rule was not the ceiling either.** Fixing it moves wire throughput by
  **~0%** at the shipped window (161–174 KB/s before, 163–174 KB/s after, on runs
  whose own spread is wider than the difference).
- **And the wire is not window-limited**, which was the third candidate: with prompt
  ACKs the peer holds 2880 bytes outstanding, 9% of a 32 KB window.

What the trace says the wire ceiling actually is, arithmetically: 524,288 bytes in
2.92 s is **8.0 ms per 1440-byte segment** at 14 MHz. Nothing in the trace is idle for
that 8 ms — no window stall, no ACK wait, no timer — so it is the per-segment cost of
the receive pipeline itself: the a2065 interrupt, the SANA-II copy hook, the deferred
receive, the checksum, the TCP reassembly and the copy out through `recv()`. That is
CPU work, and it is the same work §11 already ranked (`bsdsocket.library`'s per-call
overhead first at ~1010 ms/MB, the copies second).

So the honest position on §11's fit is that **the fixed-ceiling component is still
unexplained**, and three named candidates have now been eliminated rather than one
added. The next place to look is the per-segment cost above, which is measurable
directly (segments per second against clock) rather than by fitting.

Two figures should not be conflated when reading this against §11 and §14: `NetTrace`
measures **161–179 KB/s** over the wire where curl measures ~117 KB/s on the same
path. The difference is curl's own buffering and HTTP handling, not the stack.

#### What the fix is and is not worth

**Bulk throughput does not move**, and that is stated rather than buried: at an 8 KB
window, half the window is already about three segments, so the window update was
already firing often enough. What moves is **latency** — a 3.3× cut in the median and
a 9× cut in the worst case — and that is what every request/response exchange pays:
each HTTP round trip, each DNS query, each leg of a TLS handshake. It is also the
prerequisite for ever raising the window: with it, the 32 KB build returns to
179 KB/s and 208 acknowledgements instead of 59.

**One thing got worse and it is worth naming.** On loopback the receiver now
advertises a **zero window 64 times** in a 128-segment transfer, where before it never
did, and loopback bulk throughput falls about 3% (297 → 287 KB/s). That is not the
ACK rule misbehaving — it is the 8192-byte window being too small for a 4096-byte
application write, exposed rather than caused. Two 4096-byte segments fill the window
exactly; acknowledging on the second one therefore advertises zero, honestly. The
window reopens on the next `recv()` with no persist-timer stall, which is why the
cost is 3% and not a cliff. At a 32 KB window with the same fix there are **no** zero
windows at all, in-flight occupancy is 33%, and loopback runs at 352 KB/s.

**So the window should be raised, and it has not been.** The reason is stated rather
than hidden: the receive queue is drawn from the same `NX_PACKET` pool the SANA-II
readers pin 32 of, the pool is 256 packets on the 8 MB profile and
`AMI_POOL_MIN_PACKETS` (16) on the 4 MB floor, and 32 KB of window per socket times
forty concurrent sockets is several times the whole pool. That is a **functional
limit, not a memory budget** — exhausting the pool drops frames — and it needs the
same treatment `AMI_SANA2_RX_DEPTH_IPV4` got: derived from the pool, with the
concurrency case (`tests/curl` `d03_parallel_40`) as the acceptance test. Until that
measurement exists, `AMINETXDUO_TCP_WINDOW` is the knob that made this section
possible — two libraries out of one tree, differing in one constant — and the default
stays at 8192.

### 16.7 MSS, fragmentation and segment sizes

Nothing wrong here, and the numbers are worth recording because "we are sending
undersized segments" was a live hypothesis:

- **MSS offered: 1460** in our SYN on the wire, 65495 on loopback. Both are
  interface-MTU derived and both are right.
- **The peer sends 1440-byte segments** (364 of them, plus one 233-byte tail). 1440
  rather than 1460 is SLIRP's re-origination, not ours.
- **We never fragment.** `nx_ip_fragment_enable()` is not called and no fragment
  appeared in any trace.
- **Our own segments are the application's write size**, unmodified: 32 writes of
  4096 became 32 segments of 4096 on loopback. With no Nagle that is correct
  behaviour and not a defect, but it does mean an application that writes small
  writes small on the wire.

**The SYN options are worth reading, because two things are absent.** `tcpdump -r`
on the guest capture shows

```
10.0.2.15.56478 > 10.0.2.2.7300: Flags [S], seq ..., win 8192,
    options [mss 1460,nop,nop,nop,eol], length 0
```

— MSS and then NetX Duo's fixed eight-byte option block padded out. **No window
scale, and no SACK.** `NX_ENABLE_TCP_WINDOW_SCALING` exists in NetX Duo and is not
defined here, which puts a hard **64 KB ceiling on the receive window** whatever
§16.6's pool-derived sizing eventually decides; SACK is not implemented in the
vendored tree at all, so a burst loss costs a full go-back-N. Neither binds today —
the window is 8 KB and nothing is being lost — but both bound where this can go, and
the window-scale option is bilateral, so not offering it also stops the *peer*
scaling.

Two other numbers in that transcript belong to the host and not to us, and are noted
so nobody attributes them here later: the SYN to SYN/ACK took **353 ms** and the
request to the first data segment **494 ms**. That is `curlpeer.py` and SLIRP; our
own ACK of the SYN/ACK went out **1.6 ms** after it arrived.

### 16.8 The regression cover, and the concurrency sweep

The suite that found the four-frame receive window is the gate that matters here,
because an ACK change can move it either way and every body it fetches is hashed
against the server's copy — a correctness regression cannot pass quietly.

| | |
|---|---|
| conformance, loopback tier | **128 passed, 0 failed, 14 skipped** |
| `tests/clients` | **94 checks, 0 failures** |
| `tests/curl` groups A–F | **147 passed, 2 failed, 149 cases** |
| host `ctest` | **6/6** |

The two curl failures are the two §14 already names and neither is ours:
`a44_cookies_send` (curl does not write its cookie jar on AmigaOS — settled against
a third-party binary in §14.7) and `f07_ftp_active` (FS-UAE 3.2.35's SLIRP opens no
inbound path, §12). The loopback tier reads 128/0/14 rather than §12's 125/1/16
because `SOCK_RAW` landed alongside this work, not because of anything here.

`AvailMem` is flat at **9,541,304 bytes** across every case of groups A to D, drops
once to 9,275,720 when `tls.library` loads, and is flat again to the last case. A
one-time load, not a per-socket leak.

**The concurrency sweep, against §14.2's own numbers on the same profile:**

| `--parallel-max` | §14.2, RX depth 32 | with the ACK fix |
|---:|---:|---:|
| 8 | 3.52 s | **2.66 s** |
| 16 | 4.94 s | **4.86 s** |
| 24 | 5.38 s | **4.80 s** |
| 32 | 6.08 s | **6.22 s** |
| 40 | 8.04 s | **7.42 s** |
| 48 | 8.68 s | **8.40 s** |

Every transfer completes, every body byte-identical, `AvailMem` delta **+0**. Five of
six points faster, one marginally slower, about 7% in the mean — which is the shape
the mechanism predicts: with forty sockets sharing one packet pool each socket's
window is small relative to what it wants, so the interval between acknowledgements
is exactly what a concurrent client spends its time waiting on. It is a modest win,
and it is stated as one; the point of running it was that it could have been a loss.

### 16.9 Things nobody predicted

- **`CloseSocket()` sending a RESET is visible in every trace.** §12.3 lists it third
  and calls it "a risk that has not been reproduced". It is now *observed*: every
  completed flow in every capture ends `RST 1` from the Amiga side. Still not
  reproduced as data loss — on these paths everything was acknowledged before the
  close — but it is no longer an inference from source code.
- **The capture costs about 10% on loopback and nothing measurable on the wire.**
  Loopback 297 → 266 KB/s with a channel bound; wire 161 → 174 KB/s, i.e. inside
  run-to-run variance. Both are reported rather than one, because the honest
  statement is that the instrument perturbs the fast path and not the slow one.
- **`NetTrace` itself found a class of bug the harness could not report.** Its 16 KB
  capture buffer started life as a local in `main()`, and an AmigaDOS Shell command
  runs on a 4 KB stack. The result was an F-line trap (`#8000000B`) and a **reboot
  loop** — the machine ran the command list, crashed, reset, and ran it again, four
  times, while `DH0:` kept only what had been flushed before the last reset. The
  harness reported it as a timeout. Two lessons went into the tool: the control block
  is static, and every line of output is flushed as it is written, because a
  diagnostic tool that loses its last twenty lines when the machine has to be killed
  is not a diagnostic tool.
- **The emulator log is a capture nobody knew they had.** 41 MB of it was already
  sitting in `build/fsuae-base-*/Cache/Logs/` from earlier curl runs, and converting
  a two-week-old log reproduced §14's traffic — 834 segments, 1,200,106 bytes, window
  minima of 5312 and 3780 — without re-running anything.


## 17. Closing the gap with Roadshow: SOCK_RAW and urgent data (2026-07-26)

§12 named seventeen results that were not green and classified each one. This section
does the arithmetic §12 did not, and then closes the part of it that is ours.

### 17.1 The thirteen, named — and which tier the number belongs to

**Roadshow 4.364 scores 138 passed, 4 known deviations, 0 unexpected failures — and no
skips at all.** The four are in the suite's own `src/known_failures.c`, which is the
authority rather than a claim we are making about a stack we do not have:

| Test | Roadshow's behaviour |
|---:|---|
| 27 | `recv(MSG_OOB)` returns `EINVAL` |
| 35 | loopback does not generate RST for a closed peer |
| 76 | `SBTC_ERRNOLONGPTR` GET not supported (SET only) |
| 77 | `SBTC_HERRNOLONGPTR` GET not supported (SET only) |

`138 + 4 = 142` with nothing skipped, so **that run had a host helper connected and a
working `SOCK_RAW`**. It is a network-tier number. Our comparable number was 133, not
125, and the thirteen decompose as

* **sixteen** Roadshow passes and we did not — the seventeen of §12 less test 27, which
  Roadshow fails as well; minus
* **three** we pass and Roadshow does not — 35, 76 and 77.

As a work list, thirteen of our seventeen have to turn green, and they are:

| Count | Tests | Class |
|---:|---|---|
| 6 | 3 `socket_create_raw`, 132–136 the ICMP family | **(a)** real gap — §17.2 |
| 2 | 27 `recv(MSG_OOB)`, 64 `ws_exceptfds_oob` | **(a)** real gap — §17.3 |
| 5 | five of the nine helper-gated | **(c)** environment |

**A correction to §12.3**, which said `SOCK_RAW` was worth six results. It is worth six on
the network tier and **three** on the loopback tier: `run_icmp_tests()` gates 133, 134 and
135 on `helper_is_connected()` *after* test 132 (`test_icmp.c`), so on loopback only 3,
132 and 136 can ever run.

**And the loopback tier cannot reach 138 at all.** Nine of the 142 need a remote peer by
construction, so 133 is that tier's ceiling. Comparing our loopback figure with Roadshow's
138 was never an apples-to-apples comparison; the network tier is.

### 17.2 `SOCK_RAW`: a tee, not an interception

NetX Duo has no raw socket object. It has an IP-level raw *service*: one queue per
`NX_IP`, fed by `_nx_ip_raw_packet_processing()` and drained by whoever calls
`nx_ip_raw_packet_receive()` first. Two things rule it out as the back end for a BSD
descriptor, and the second is the one that decides the whole design.

1. **ICMP never reaches it.** `_nx_ip_dispatch_process()` sends ICMPv4 to
   `nx_ip_icmp_packet_receive` and consults the raw hook only in the "protocol I do not
   recognise" branch, so a raw socket opened with `IPPROTO_ICMP` — the one every `ping`
   and `traceroute` opens — never sees a byte. `NX_ENABLE_IP_RAW_PACKET_ALL_STACK` moves
   the hook to the top of the dispatch, ahead of TCP, UDP, ICMP and IGMP. It is not
   documented in `nx_user_sample.h`; it appears only in `nx_ip_dispatch_process.c`, and
   it does nothing unless `NX_ENABLE_IP_RAW_PACKET_FILTER` is on too.

2. **That hook is the filter, and installing a filter turns the queue off.**
   `_nx_ip_raw_packet_processing()` returns as soon as it sees one
   (`nx_ip_raw_packet_processing.c`) and never touches
   `nx_ip_raw_received_packet_head`. So `nx_ip_raw_packet_receive()` becomes dead code and
   the queue, the per-protocol demultiplex and the wakeup are ours.

`src/bsdsocket/raw.c` owns all three. The filter's return value decides ownership:
`NX_SUCCESS` means "I took it" and the stack stops processing the packet, anything else
means "not mine". **Ours copies a packet for each interested descriptor and always
declines.** Consuming would be one packet copy cheaper and would break the machine — an
echo request claimed by a raw socket is a request nobody answers, and a claimed reply is
one `nx_icmp_ping()` never wakes on. Test 132 depends on exactly that: it pings 127.0.0.1
*from a raw socket* and waits for the reply the ICMP layer still generates from the
request it still sees.

A reader gets a whole IP datagram, header included — the suite parses `(buf[0] & 0x0F) * 4`
to find the ICMP header, and so does every `ping` ever written. The header is still
physically in the buffer when the filter runs (`nx_ipv4_packet_receive.c` only advances
`nx_packet_prepend_ptr` past it and leaves `nx_packet_ip_header` pointing at it), so the
copy is taken with the pointer wound back and the original restored before declining.
Transmit is the mirror image and is *not* header-included: the caller writes the protocol
payload and `nxd_ip_raw_packet_send()` prepends the header from the socket's protocol, TTL
and TOS. That is BSD's default; `IP_HDRINCL` is the opt-in and NetX Duo's core has no
equivalent (it exists only in the `addons/BSD` layer this port does not use).

The filter is installed only while at least one raw descriptor is open, so a machine with
none pays a single NULL test per inbound packet inside a branch NetX Duo already had.
Blocking `recv()` suspends on a per-socket ThreadX semaphore rather than an Exec signal:
the filter runs on the IP thread and must not touch Exec, and the reader is already inside
a `bsd_nx_enter()` bracket where ThreadX suspension is the correct way to wait. The
registry and the queues are guarded by `nx_ip_protection`, which the IP thread already
holds for the whole of its event loop.

**The deliberate divergence §12 asked to keep is now moot and was still kept.**
`test_socket.c:52` skips test 3 only on `EACCES`, and "you lack privilege" is meaningless
on an OS with no privilege model, so we answer `EPROTONOSUPPORT` or `EAFNOSUPPORT` as the
case warrants and never `EACCES`. The test passes rather than being skipped.

### 17.3 `MSG_OOB`: the receive half was already there

§12.3 said urgent data "is not a bsdsocket change: NetX Duo's TCP neither sets the `URG`
bit nor parses it". **Half of that is wrong, and it is the expensive half that was
already done.** `_nx_tcp_socket_packet_process()` tests `NX_TCP_URG_BIT` and calls the
socket's `nx_tcp_urgent_data_callback`; the segment is still on
`nx_tcp_socket_receive_queue_head` *with its TCP header*, because the header is stripped
only at delivery (`nx_tcp_socket_receive.c`); and the urgent pointer is in the low half of
`nx_tcp_header_word_4` in host order. Everything needed was reachable.
`bsdsocket.library` was passing `NX_NULL` for that callback.

Transmit is the half that genuinely is not there, and it cannot be added by preparing a
header, because **both** senders finish with

```c
header_ptr -> nx_tcp_header_word_4 = (checksum << NX_SHIFT_BY_16);
```

— a plain assignment, *after* the checksum has been computed over that word
(`nx_tcp_socket_send_internal.c`, `nx_tcp_packet_send_control.c`). Any urgent pointer
planted beforehand is destroyed and the checksum is wrong as well.

The obvious route was to open-code an urgent segment the way `bsd_tcp_send_fin()`
open-codes the graceful FIN. **It is not the same problem.** A FIN is a control packet:
fire-and-forget, never retransmitted. An urgent byte is *data*, it consumes a sequence
number, and a copy of `_nx_tcp_socket_send_internal()` would have to reproduce the window
arithmetic, the transmit-queue linking, the outstanding-byte accounting and the
mutex-drop race check around the checksum — or skip the retransmit queue and leave a hole
in the sequence space that stalls the connection permanently the first time the segment is
lost.

So the byte goes out through `nx_tcp_socket_send()` like any other: queued, retransmitted,
accounted, with NetX Duo owning all of it. The `URG` bit and the urgent pointer are
written into that one segment on its way past `nx_ip_packet_filter`, which
`_nx_ip_packet_send()` consults *after* `_nx_ip_header_add()` and *before* the driver —
the last point at which the bytes are still ours. Two 16-bit words change, so the TCP
checksum is repaired incrementally by RFC 1624 equation 3 rather than recomputed. The
filter is installed for the duration of that one send and removed immediately, so the
steady-state cost on the packet path is zero.

`nx_ip_packet_filter` is the plain hook and was free; `src/netstack/` uses
`nx_ip_packet_filter_extended` for capture, which is a different slot and a different
call. The previous value is saved and restored regardless.

**Nothing in `third_party/` is patched, and no vendored symbol is overridden.** The
receive half is a callback argument `nx_tcp_socket_create()` already takes; the transmit
half is a hook `NX_ENABLE_IP_PACKET_FILTER` already installs for us.

**Two deliberate divergences, both (b):**

1. **The urgent byte is delivered in the normal stream as well**, as though
   `SO_OOBINLINE` were always set; `recv(MSG_OOB)` returns a copy. Taking a byte back out
   of the middle of a queued `NX_PACKET` would mean rewriting a segment the TCP state
   machine still owns and still counts in its sequence space, to hide one byte that both
   real callers — `ftp`'s `ABOR` and `telnet`'s interrupt — send inline anyway.
2. **A retransmission carries the byte but not the `URG` bit.**
   `_nx_tcp_socket_retransmit()` rebuilds `nx_tcp_header_word_3` without it in any case.
   That is the right failure mode: the data is always reliable, only the urgency marking
   is best effort.

`recv(MSG_OOB)` with nothing marked answers `EINVAL`, which is 4.4BSD's answer and, as it
happens, Roadshow's unconditional one. `IoctlSocket(SIOCATMARK)` answered `ENOSYS` and now
answers whether an unread urgent byte is outstanding; the suite does not test it.

### 17.4 Where it lands

| | before | after |
|---|---|---|
| loopback tier | 125 passed, 1 failed, 16 skipped | **130 passed, 0 failed, 12 skipped** |
| network tier | 133 passed, 2 failed, 7 skipped | **141 passed, 1 failed, 0 skipped** |
| Roadshow 4.364 | | 138 passed, 4 known, 0 skipped |

The loopback tier is at its structural ceiling less one: 130 of the 133 it can reach.

Everything still not green, named:

| # | Name | Class | Why |
|---|---|---|---|
| 39, 40, 42 | `tcp_network_64k`, `udp_network_datagram`, `tcp_network_large` | **(c)** | `helper_is_connected()` gate; green on the network tier |
| 103, 104 | `gethostbyname_external`, `gethostbyaddr_external` | **(c)** | same gate; green on the network tier |
| 133, 134, 135 | `icmp_network`, `icmp_large_payload`, `icmp_multi_ping` | **(c)** | same gate; green on the network tier — 2.5 ms, 9.9 ms and 5/5 replies |
| 138, 140, 142 | `tp_tcp_network`, `tp_udp_network`, `tp_tcp_sustained_network` | **(c)** | same gate; green on the network tier |
| 41 | `accept(): incoming connection from remote host` | **(c)** | the one result neither tier reaches — see below |

Nothing is left in class **(a)**.

### 17.5 Test 41, re-checked rather than inherited

§12 called it an environment artefact on the strength of the helper log
(`CONNECT to 127.0.0.1:7861 failed: [Errno 61] Connection refused`) and of
`uae_slirp_ports` reaching the FS-UAE config and doing nothing. That is circumstantial,
so it was checked again with the option UAE actually documents for the job.

`uae_slirp_redir = T:7861:7861`, dropped into the run's private `Host.fs-uae` (the same
route that turns FS-UAE's own bsdsocket emulation off), **reaches the emulator** — it is
echoed in `fs-uae.log.txt` right after `bsdsocket_library = 0` — and test 41 still fails.
The decisive measurement is on the host side: with the guest booted and the suite running,

```
lsof -nP -iTCP -sTCP:LISTEN -a -c fs-uae      →  nothing
lsof -nP -iTCP:7861                            →  nothing
```

FS-UAE 3.2.35 opens **no inbound TCP socket at all**, for 7861 or for anything else, so
there is no port on the host that could carry a connection into the guest whatever the
configuration says. The suite derives the port as `base + 161` and cannot be pointed
elsewhere. This is not reachable from here and it is not ours; the capability itself is
covered on loopback by `tests/clients` groups D, E, I and M.

### 17.6 The regression cover, and what a filter on every inbound packet costs

`SOCK_RAW` puts a callback on the dispatch path of **every inbound IP packet**, which is
the hot path for every transfer in the tree. A correctness win there is cheap to pay for
with throughput, so it was measured rather than argued.

| | |
|---|---|
| conformance, loopback tier | **130 passed, 0 failed, 12 skipped** |
| conformance, network tier | **141 passed, 1 failed, 0 skipped** |
| `tests/clients` | **94 checks, 0 failures** |
| `tests/curl` groups A–F, our curl | **147 passed, 2 failed, 149 cases** |
| `tests/curl` groups A–D and F, the Aminet binary | **122 passed, 2 failed, 124 cases** |
| host `ctest` | **6/6** |
| `tools/ci.sh` on playhouse2, NDK 3.9 | host + all four cross configs + conformance, **all green** |

The two curl failures are §16.8's two and §14's two — `a44_cookies_send` and
`f07_ftp_active` — unchanged in identity and in count.

**The concurrency sweep, against §16.8's own numbers on the same profile:**

| `--parallel-max` | §16.8 | with `SOCK_RAW` and urgent data |
|---:|---:|---:|
| 8 | 2.66 s | **2.66 s** |
| 16 | 4.86 s | **3.30 s** |
| 24 | 4.80 s | **4.76 s** |
| 32 | 6.22 s | **6.94 s** |
| 40 | 7.42 s | **7.16 s** |
| 48 | 8.40 s | **8.52 s** |

Every transfer completes, every body byte-identical, `AvailMem` delta **+0**. Three
points faster, two slower, one identical, about 3% either way — which is run-to-run
noise on this profile and is stated as noise, not as a win.

**And the reason it is noise is structural, which is why the sweep was expected to be
flat rather than hoped to be.** Neither filter is installed in the steady state:

* `raw.c` installs `nx_ip_raw_packet_filter` on the first `SOCK_RAW` descriptor and
  removes it with the last. Nothing in the curl suite opens one, so what remains is the
  two loads and two branches `NX_ENABLE_IP_RAW_PACKET_ALL_STACK` adds at the top of
  `_nx_ip_dispatch_process`, inside a branch NetX Duo already had.
* `oob.c` installs `nx_ip_packet_filter` for the duration of one `nx_tcp_socket_send()`
  and takes it out again on the next line.
* The urgent-data callback is now non-NULL on every TCP socket, but
  `_nx_tcp_socket_packet_process()` already tested `NX_TCP_URG_BIT` before deciding
  whether to call it. No segment in 149 curl cases has that bit set.

## 18. The record path: AES-128-CBC and SHA-256 on the 68020 (2026-07-26)

§15 ended by naming this: the bulk path was the one row where neither this tree
nor AmiSSL had a single byte of m68k assembly, and it is the row that decides
`https://` throughput, because it is paid on every byte of every transfer
rather than once per connection. The handshake is now level with OpenSSL and is
further reduced by session resumption (§13); the per-byte cost is forever.

The ciphersuites this client actually negotiates are `0xC027`
(ECDHE_RSA_WITH_AES_128_CBC_SHA256) and `0xC023` (ECDHE_ECDSA_…), so the record
path is AES-128-CBC plus HMAC-SHA256, both directions, per record. GCM is
compiled in and no negotiated suite reaches it.

**The result, in one line: `https` went from 15,801 B/s to 20,057 B/s on the
same machine at the same commit, 1.27×, with only the record path's
implementation differing.** Everything below is how, and what did not work.

### 18.1 What the machine charges, measured before anything was written

`tests/crypto68k/crypto68k_bulk` runs instruction kernels whose mix is known
because they are assembly (`c68k_bulk_kernels.S`), for the same reason
`tests/perf/cpucal` does: a C loop that "should" compile to a rotate is a
measurement of the compiler. A1200 profile, `-k 56`, implied 56.4 MHz.

| | measured | implied cycles | MC68020UM |
|---|---:|---:|---:|
| `ADD.L Dn,Dm` | 35.936 ns | 2.00 | 2 |
| `EOR.L Dn,Dm` | 35.592 ns | 1.98 | 2 |
| `AND.L Dn,Dm` | 35.597 ns | 1.98 | 2 |
| `MOVE.B Dn,Dm` | 35.967 ns | 2.00 | 2 |
| `SWAP Dn` | 71.050 ns | 3.95 | 4 |
| `LSR.L #3,Dn` | 70.834 ns | 3.94 | 8 |
| `ROR.L #1,Dn` | 106.798 ns | 5.94 | 8 |
| `ROR.L #8,Dn` | 106.801 ns | 5.94 | 8 |
| `ROR.L Dm,Dn`, count 13 | 142.217 ns | 7.91 | 8 |
| `MOVE.L d16(An),Dm` | 89.011 ns | 4.95 | |
| `MOVE.B d16(An),Dm` | 88.678 ns | 4.93 | |
| `MOVE.L Dm,d16(An)` | 156.534 ns | 8.71 | |
| `MOVE.L (An,Dn.W*4),Dm`, **1 KB** table | **159.845 ns** | 8.89 | |
| `MOVE.L (An,Dn.W*4),Dm`, **4 KB** table | **159.847 ns** | 8.89 | |
| `MOVE.B (An,Dn.W),Dm`, 256 B table | 159.842 ns | 8.89 | |

Four rows decide the whole section.

**A 4 KB table and a 1 KB table cost the same to the picosecond.** 159.845 ns
against 159.847. That is what "no data cache" means in practice, and
`cpucal`'s 32 KB / 64 B read ratio of 0.88× says the same thing from the other
direction. Every argument for the one-table AES layout is an argument about
cache footprint; on this part footprint is free, so the rotates that layout
spends to save 3 KB are pure loss.

**A byte read from a table costs exactly what a longword read costs**, 159.842
against 159.845. The addressing mode dominates and the operand size does not
appear at all. So the byte-oriented S-box variant — which trades 4-byte reads
for 1-byte reads and pays for MixColumns in the ALU — buys nothing on the side
it was supposed to win on, and pays full price on the other.

**Rotates are not the bottleneck they look like, and SWAP is a trap.** A rotate
by an immediate is 5.94 cycles and by a register 7.91. So `SWAP` + `ROR.L #n`
(3.95 + 5.94 = 9.89) and `MOVEQ` + `ROR.L Dm,Dn` (2 + 7.91 = 9.91) are the same
price to within the measurement. The SWAP idiom is a **68000** habit, where a
rotate cost 8 + 2n and rotating by eleven meant thirty cycles; a 68020 has a
flat shifter and the trick is worth nothing. §18.4 is what that did to a
SHA-256 written around it.

**The instruction cache is not modelled here, and that has to be said plainly.**
Sweeping a straight-line body of `ADD.L` from 32 bytes to 2 KB:

```
   32 B body  46.742 ns per ADD.L      512 B body  36.321 ns
  128 B body  38.251 ns                1024 B body 35.899 ns
  256 B body  37.237 ns                2048 B body 35.906 ns
```

That is monotonically *decreasing* and it flattens at exactly `ADD.L`'s 35.9 ns
— it is the loop's `SUBQ`/`BNE` being amortised over more instructions and
nothing else. A real 68EC020 has 256 bytes of direct-mapped instruction cache
and a 2 KB straight-line body would fetch every instruction from the bus;
FS-UAE's A1200 model charges nothing for that. **So this emulator would reward
unrolling that a real machine would punish**, and both round loops below are
deliberately left rolled on the strength of the MC68020UM rather than of a
measurement we cannot make. The AES round body is 176 bytes and the SHA-256
round body 96, both inside the real cache; an eight-round SHA unroll would be
about 770 bytes and is exactly the trade that cannot be evaluated here.

### 18.2 The AES question, answered

Three implementations of the same cipher, in one process, on one buffer, each
checked against FIPS-197 and against every other before a time is believed.
16 KiB, `-k 56`, and the two directions measured separately because a download
decrypts and §15 only ever measured encryption.

| | encrypt | decrypt |
|---|---:|---:|
| `nx_crypto`, what we shipped | 85,307 µs (187 KB/s) | 110,139 µs (144 KB/s) |
| **T4** four 1 KB tables, C | 73,058 µs (218 KB/s) | 74,387 µs (214 KB/s) |
| **T1** one 1 KB table + rotates, C | 79,799 µs (200 KB/s) | 86,674 µs (184 KB/s) |
| **SBOX** 256 B S-box, MixColumns in the ALU, C | 131,856 µs (121 KB/s) | 188,659 µs (83 KB/s) |
| **T4**, 68020 assembly | **67,298 µs (237 KB/s)** | **67,731 µs (235 KB/s)** |
| **T1**, 68020 assembly | 78,195 µs (204 KB/s) | 78,809 µs (202 KB/s)|

**Four tables win, and they win by more in assembly than in C: 13.9% on encrypt
and 14.1% on decrypt.** This is the answer to "what is the right AES for a
machine with no data cache", and it is the opposite of the folklore. The
one-table layout exists to keep the working set small enough to stay cached;
with no cache to stay in, its three rotates per column are twelve rotates per
round bought for nothing. The three rotations
per column are `ROR.L #8`, `SWAP` and `ROL.L #8`: 15.8 cycles a column by the
table above, 63 a round, 570 a block. The measured gap between the two
assembly rows is 10,897 µs over 1,024 blocks, which is 600 cycles a block. The
rotates are the whole of it.

**The byte-oriented S-box loses by 1.96× encrypting and 2.79× decrypting**
against the four-table assembly, and 1.55×/1.71× against the implementation it
would have replaced, and
the instruction table says why before the cipher is written: its reads are the
same price as the T-table's, so its entire MixColumns is added cost. It is kept
in the tree, and so is the one-table form, because a measurement nobody can
reproduce is an assertion.

The assembly is worth **8.0% encrypting and 8.9% decrypting** over the best C,
and 1.27×/1.63× over what we shipped. What it does differently:

- **The state lives in memory, not in registers.** A round needs four state
  words, four accumulators, an index and a temporary; that is ten values and
  the 68020 has eight data registers. Reading an index byte out of a register
  costs `MOVE.B` plus a `ROL.L` to bring the next byte down, because only the
  low byte of a register can be moved out — two instructions and 7.94 cycles.
  Reading it out of a sixteen-byte buffer costs one `MOVE.B d16(An),Dn` and
  4.93, *and* leaves four registers free to hold the accumulators, so the round
  ends in a single `MOVEM.L`. On a part with a data cache the sixteen byte reads
  would be nearly free and it would not be close; on this one it is still ahead.
- **The last round uses the 256-byte S-box** and byte reads, rather than masking
  four T-table entries the way `aes_core.c` does.
- **The big-endian longword load is inline assembly**, in the C wrapper: one
  `MOVE.L` at any alignment, because the 68020 does misaligned accesses in
  hardware and a TLS record's payload starts 21 bytes into the packet buffer.
  GCC compiles the portable form into four byte loads, three shifts and three
  ORs, and it is 300 cycles a block of load and store around a 3,700 cycle
  cipher.

What is **not** taken, with its price, because a measured option declined is
worth more than one not noticed: the CBC loop is still C, and it costs one
`JSR`, one `MOVEM` of eleven registers each way and four longword loads, four
`EOR`s and four stores a block — about 600 cycles of a 3,725 cycle block, 16%,
priced from the instruction table above rather than measured on its own.
Fusing CBC into the assembly would recover perhaps two thirds of it — 11% of
AES, 5% of the record path, and about 4% on the wire, which is below what the
wire measurement resolves.

### 18.3 SHA-256: 1.29× on the compression function, and none of it assembly

| | aligned | on an odd address |
|---|---:|---:|
| SHA-256, `nx_crypto` | 85,739 µs (186 KB/s) | |
| SHA-256, ours | **66,198 µs (241 KB/s)** | 66,453 µs |
| HMAC-SHA256, `nx_crypto`'s hash | 86,730 µs (183 KB/s) | |
| HMAC-SHA256, ours | **66,869 µs (239 KB/s)** | |

**1.30× on the compression function and 1.30× through HMAC**, and the
misaligned case — which is the one a TLS record actually presents — costs 0.4%
rather than the 6.3% it cost before the `MOVE.L` went in.

Two changes produced all of it and neither is assembly:

1. **The sixteen message words are loaded, not assembled.** This is a
   big-endian machine, so `W[t]` for t < 16 is the longword at `data + 4t`.
   `nx_crypto_sha2.c`'s `W0()` macro builds each one from four byte loads,
   three shifts and three ORs — 128 instructions a block that need not exist.
   OpenSSL has had a big-endian fast path here for decades; the vendored code
   does not.
2. **The message schedule is computed up front** rather than interleaved with
   the rounds. Interleaved, the round's two scratch registers have to serve both
   and everything spills.

### 18.4 The 68020 SHA-256 that was written, measured, and removed

It was written. `d` and `h` in address registers because they are pure addends
in any round, which is what makes eight state variables fit in eight data
registers with two to spare; `g` moved to its address register the instant `Ch`
finished with it, freeing a third scratch; the two big sigmas factored so that
`Sigma0(x) = ROTR2(x ^ ROTR11(x) ^ ROTR20(x))`; every rotation expressed as
`SWAP` plus an immediate rotate. It passed FIPS 180-4, RFC 4231 and the
million-`a` vector.

| | aligned | one byte in |
|---|---:|---:|
| portable C | 66,687 µs | 70,241 µs |
| 68020 assembly | 67,656 µs | 67,653 µs |

**The C wins on the buffer a benchmark hands it and the assembly wins on the
one a TLS record actually is, and the whole spread is 5%.** §18.1's instruction
table is the explanation: `SWAP` + immediate rotate is 9.89 cycles and the
`MOVEQ` + register rotate a compiler is forced into is 9.91. The trick the
whole file was built on does not exist on this part.

So the assembly's one genuine advantage was the misaligned `MOVE.L` for the
message words — and that is three lines of inline assembly, not 230 lines of
hand-written rounds. It moved into the C, which is now ahead on both
alignments, and `c68k_sha256.S` was deleted.

**This is a real result and it is worth stating without hedging: for SHA-256 on
a 68020, GCC 15.2 is not leaving anything on the table.** The 1.29× came from
knowing the machine is big-endian, which is an algorithm question, not from
instruction selection. The AES assembly earns its place — 8-9% over the best C
and a question about table layout that could not have been settled any other
way — and the SHA-256 assembly did not.

### 18.5 §15's table, re-run

Same harness, same process, same `-k 56`, every result checked against
AmiSSL's before it is timed. **0 failures, 0 mismatches** — 16 KiB of AES
ciphertext identical, 16 KiB of recovered plaintext identical, the HMAC tag
identical. The handshake rows are unchanged and are reproduced for context.

| operation | ours | AmiSSL | measured | corrected |
|---|---:|---:|---|---|
| RSA-2048 public, e=65537 | 139.5 ms | 142.7 ms | ours 1.02× | AmiSSL 1.004× |
| RSA-2048 private CRT, blinding off | 4.97 s | 5.30 s | ours 1.07× | ours 1.22× |
| RSA-2048 private CRT, OpenSSL's default | 4.97 s | 6.88 s | ours 1.38× | ours 1.54× |
| ECDSA P-256 verify | 484.9 ms | 840.2 ms | ours 1.73× | ours 1.69× |
| ECDH P-256 shared secret | 338.3 ms | 1,049.7 ms | ours 3.10× | ours 3.03× |
| k·G, an ECDHE key generation | 94.1 ms | 1,047.4 ms | ours 11.1× | ours 10.76× |
| **AES-128-CBC encrypt, 16 KiB** | **67.4 ms** | 84.2 ms | **ours 1.25×** | — |
| **AES-128-CBC decrypt, 16 KiB** | **67.9 ms** | 85.5 ms | **ours 1.26×** | — |
| **HMAC-SHA256, 16 KiB** | **68.5 ms** | 111.2 ms | **ours 1.62×** | — |

No MULU.L correction applies to the last three rows: neither implementation
contains a single multiply, which was true in §15 and is still true — the
inline-assembly `MOVE.L` and the byte-parallel `xtime` were both chosen partly
so that it stayed true. `tests/perf/cpucal` reports MULU.L at 32.06 cycles
against the part's 45 in the same run, and it is irrelevant here.

**The bulk row was a dead heat and is now ours by a quarter to two thirds.**
One 16 KiB TLS record encrypted and MACed: **135.9 ms our way against
195.4 ms AmiSSL's, 117 KB/s against 81 KB/s of application data.** §15 had
92 KB/s against 81; the gap went from 13% to 44%.

Read the decrypt row twice, because §15 did not have it. `nx_crypto`'s AES
decrypts 25% *slower* than it encrypts (110,139 µs against 85,307) and AmiSSL's
is nearly symmetric; ours is symmetric to within 0.6%. A download decrypts
every byte it receives, so the asymmetric implementation was losing on the
direction that matters.

### 18.6 The number that actually matters: the wire

The primitive is not the deliverable. `tests/curl/run-curlverify.sh` is, and
the case is `e18_tls_large` — half a megabyte through the record layer against
the suite's own host peer and its own PKI, with the body hashed against the
server's copy, so a fast wrong AES cannot pass.

Two `tls.library` binaries built from **one commit**, differing only in
`AMINETXDUO_TLS_STOCK_BULK`, which puts `nx_crypto`'s AES and SHA-256 back in
the ciphersuite table and changes nothing else. That is not fussiness: the TCP
layer grew delayed ACKs (`78b4ed9`) and the input path grew a per-packet
`SOCK_RAW` filter (`026c348`) in the same window, and a comparison against a
figure from before those would have landed somebody else's work in this result.

| | bytes | seconds | B/s |
|---|---:|---:|---:|
| `https` `e18_tls_large`, `nx_crypto` bulk | 524,288 | 33.18 | **15,801** |
| `https` `e18_tls_large`, `crypto68k` bulk | 524,288 | 26.14 | **20,057** |
| `http` `a04_get_1m2`, `nx_crypto` build | 1,200,000 | 6.54 | 183,486 |
| `http` `a04_get_1m2`, `crypto68k` build | 1,200,000 | 7.44 | 161,290 |

**`https` is 1.27×.** The TLS penalty on the wire, in this hermetic setting,
goes from about 11.6× to about 8.0×.

**The `http` control needs saying carefully rather than quoting.** The two
`http` rows differ by 12%, and they differ in the direction *opposite* to the
`https` gain — which is the tell that it is not a property of either build.
`bsdsocket.library` is byte-identical between them (`md5` 6386710…, both), only
`tls.library` differs, and the ciphersuite table is consulted for nothing but
TLS. What moves is the measurement: every one of these numbers goes through
FS-UAE's SLIRP, whose packet delivery is scheduled by the *host* and is not
part of the cycle-exact model, so a contended host puts variance into any wire
figure however deterministic the CPU is. The control says what a control can
say — the `http` path is untouched by construction, and its noise does not
favour the result.

What does corroborate the `https` row is the arithmetic below, measured on the
E-Clock with no network in it at all: the primitive benchmark predicts an 8.0 s
difference over half a megabyte and the wire shows 7.04 s.

The arithmetic. Receiving
costs HMAC plus AES-decrypt: 135.9 ms per 16 KiB at 56.4 MHz is 548 ms at
14 MHz, and 524,288 bytes is 32 records, so 17.5 s of the measured 26.14. The
stock path is 197.4 ms per 16 KiB, 795 ms at 14 MHz, 25.5 s of the measured
33.18. The predicted difference is 8.0 s and the measured one is 7.04 — the
rest is TCP, the handshake and the file writes, and they are the same on both
sides.

**On §11's figures.** That section measured 16,464 B/s over `https` against
114,598 over `http`, and those are not directly comparable to the rows above:
they were fetched from real hosts across the real internet at `-k 28`, where
`http` is limited by the far end and the round trip as much as by this machine.
The hermetic peer makes `http` faster (161 KB/s) and leaves `https` CPU-bound,
which is why the ratio here starts higher. The controlled number is the A/B,
and the A/B says 1.27×.

**A primitive-level speedup that did not move the wire number would be a
finding too. This one moved it, by about what the arithmetic predicted, and the
remaining ceiling is still the record path**: at 20,057 B/s the record layer is
about two thirds of the cost and everything else in the stack shares the rest.

### 18.7 Where it is wired, and what is checked

`src/tls/ami_tls_crypto.c` — private `NX_CRYPTO_METHOD` entries, the same
mechanism the RSA and P-256 methods use, no vendored source touched. HMAC is
`nx_crypto`'s own framing with the hash swapped underneath it through
`_nx_crypto_hmac_metadata_set()`, which takes the three hash entry points as
function pointers; `c68k_sha256_initialize`/`_update`/`_digest_calculate` have
exactly those signatures on purpose, so none of the ipad/opad, key-shortening
or padding logic is reimplemented.

`tests/crypto68k/crypto68k_bulk` is the emulator tier and
`tests/crypto68k/host/test_c68k_host.c` the host one, which is where the
vectors run on every push. Both check FIPS-197 (AES-128 and AES-256, both
directions), FIPS 180-4 (`"abc"`, the empty string, the 56-byte message, one
million `a`), RFC 4231, and the shapes that break CBC implementations: zero
blocks, a single block, a chaining value carried across calls, a decrypt in
place, and a buffer on an odd address. Every variant is checked against every
other and against `nx_crypto` before a single time is printed.

The host tier caught one bug and it is the one worth recording: the SHA-256
fast path reads `W[0..15]` as longwords, which is right on the m68k and wrong
on the build machine, and the first version had no endianness guard on it.

And the wire is the last check, because a cipher that corrupts one byte in a
million looks exactly like a network problem. `run-curlverify.sh -g E` on the
new build: **21 of 28 cases run and every one of them passes** — six chain
fetches at depths 2, 3 and 4 in RSA and ECDSA with every body hashed against
the server's copy, the half-megabyte transfer, TLS reuse across processes, and
the five negative cases (wrong CA, wrong host, expired, self-signed, TLS on a
plain port) all refused for the right reason. The seven that did not pass are
all `no result line -- the run did not reach this case`: three emulators were
contending for `build/.fsuae.lock` and the run spent most of its budget
queueing. Nothing failed; the tail was not reached.

## 19. `sntp`, and the clock that turns certificate checking back on (2026-07-26)

`sntp` looks like a convenience and is not one. §13 and §14 built a TLS stack that
verifies a chain and checks a host name; what it does **not** do, on most of the machines
it will run on, is check whether the certificate has expired. This section is what it took
to change that, and what the attempt found out about the command set on the way.

### 19.1 The thing a clock is actually worth

`src/tlslib/tls_time.c` returns 0 — NetX Duo's "do not check" sentinel — whenever
`DateStamp()` lands outside a fifty-year window starting 2026-01-01. The reasoning is in
the file and it is right: an Amiga with a dead battery starts at 1978, every certificate
on the internet was issued after 1978, and a library that refused them all would be a
library nobody could use. The price is exact, and is paid silently:

> **an expired certificate is accepted.**

Measured under FS-UAE, one command apart, against the same expired leaf:

```
    ClockSet 0
    fetch https://expired.test:7206/bytes/16
      expired.test: TLS 0x303, ciphersuite 0xC027, 2 certificate(s)
        chain verified, validity dates NOT checked (the clock is unset)
      HTTP/1.1 200 OK                                     <-- ACCEPTED

    sntp time.apple.com
      time.apple.com (17.253.4.45): stratum 1
      This machine's clock was 17738 days 10 hours slow.
      Clock set to Sunday 26-Jul-26 10:22:49, UTC.
      The battery-backed clock was set too, so this survives a reboot.

    fetch https://expired.test:7206/bytes/16
      fetch: expired.test: the certificate is expired or not yet valid
                                                          <-- REFUSED
```

and the host end saw the refusal independently, as an alert rather than a dropped
connection:

```
    [141.01] https-expired TLS handshake from 127.0.0.1:57552 failed:
             [SSL: SSLV3_ALERT_CERTIFICATE_EXPIRED]
```

A certificate that is genuinely valid (`rsa2.test`) was fetched on **both** sides of the
same run and succeeded on both, reporting `validity dates NOT checked` before and
`validity dates checked` after. So the refusal is demonstrably about the expiry date, and
not about the clock having broken TLS in general.

One further thing fell out of that run, and it is §13's design working rather than luck.
The *after* fetches did **not** resume the sessions the *before* fetches cached: they
presented two certificates again and re-verified from scratch. `tls_resume_flags()` folds
`tc_ExpiryChecked` into the resumption trust key, so a ticket cached while the clock was
unset cannot be reused once the clock is set. Without that, setting the clock would have
changed nothing until the cache aged out — the second `fetch` would have resumed, no
certificate would have been sent, and nothing would have been checked.

### 19.2 Three epochs, and the subtraction that spans two of them

NTP counts seconds from 1900-01-01. UNIX counts from 1970-01-01. AmigaOS —
`DateStamp()`, `timer.device` and `battclock.resource` alike — counts from 1978-01-01.
Only the first and the last matter, and the gap is

```
  2208988800    1900 -> 1970   the number everyone knows
+  252460800    1970 -> 1978   the one tls_time.c already carries
= 2461449600
```

NTP's seconds field is 32 bits and wraps in February 2036, which is usually a special case
and here is not one. A plain 32-bit **unsigned** subtraction of that constant gets the next
era right by itself, because the wrap in the server's field and the wrap in our arithmetic
are the same wrap: `(t + 2^32) - K ≡ t - K  (mod 2^32)`. The AmigaOS epoch itself runs out
in 2114, which is when this stops being true.

The fraction field is converted without 64-bit arithmetic. The exact answer is
`frac * 1000000 / 2^32`; dropping the bottom sixteen bits first leaves
`(frac >> 16) * 15625 / 1024`, which is exact in 32 bits, resolves to about 15 µs, and is
four orders of magnitude finer than the round trip it is added to.

The offset is **not** computed with RFC 4330's four-timestamp formula. That formula is for
disciplining a clock that is already close, and it overflows 32 bits outright when the
local clock is 48 years out — which is precisely the machine this command exists for. The
server's transmit timestamp plus half the locally-measured round trip is the answer, and
both ends of that measurement are read from `timer.device` before anything is changed, so
however wrong the clock is it cancels exactly.

### 19.3 AmigaOS keeps local time, and the offset is not ours to invent

NTP is UTC. The Amiga clock is local: `DateStamp()` has no timezone concept at all, and
neither has the battery clock. `tests/curl/mkpki.sh` already had to pin certificate dates
because of exactly this (host 04:09 UTC, guest 21:09, every leaf refused as "not yet
valid"). So writing the clock needs an offset, and the only real question is where it
comes from.

**Not from a new configuration file.** Nothing in `DEVS:Internet` or `DEVS:NetInterfaces`
has ever known about time, and `NetSetup` has never asked. Adding an eleventh place to
configure the machine, to serve one command, would be the wrong answer when the machine
already knows: `locale.library` has carried `loc_GMTOffset` — minutes west of Greenwich —
since AmigaOS 2.1, the Locale preferences editor is where a user sets it, and every other
program that cares reads it there. So `sntp` reads it there.

Two consequences, both stated in the command's own output rather than buried:

* when `locale.library` is absent — a bare 3.1 install may well not have it — the clock is
  set to **UTC** and the command says *"This machine has no locale.library, so nothing here
  knows its timezone"*, because a machine three hours out is worth mentioning;
* AmigaOS has no daylight-saving rules of any kind. The offset in the preferences is the
  whole answer, summer and winter alike. Doing better would mean shipping a timezone
  database, which is a much larger thing than this command.

The emulator cannot exercise the non-zero case, and that is a real gap rather than a
choice: `tools/fsuae-run.sh` stages no `LIBS:` beyond what the test puts there,
`locale.library` is disk-based rather than in ROM, and there is no copy of it in the tree
to stage. The UTC path is what the run proves.

`locale.library` and `battclock.resource` are both reached through the NDK's own inlines
rather than hand-coded LVOs, and the first draft is why that is written down: it called
`OpenLocale` at -60 and `CloseLocale` at -66, which are `ConvToLower` and `ConvToUpper`.
The real offsets are -156 and -42. Nothing caught it, because the harness has no
`locale.library` for `OpenLibrary()` to find and the bad call was never made. Hand-coded
offsets belong to `bsdsocket.library`'s vectors and our own private ones, where there is no
header; everywhere else the NDK has one and it should be used.

### 19.4 Both clocks, because one of them is the one that survives

`timer.device`'s `TR_SETSYSTIME` sets the running system, and that is all it sets.
`SetClock SAVE` exists because AmigaOS keeps the durable copy somewhere else, and on a
machine that has just been given a correct clock for the first time, losing it at the next
reboot is the one outcome that must not happen. So `sntp` writes both: `TR_SETSYSTIME`,
and then `battclock.resource`'s `WriteBattClock()`.

A machine with no real-time chip — a bare A500, a bare A1200 — has no `battclock.resource`
at all; `OpenResource()` returns NULL, and the command says the time will be lost at the
next reboot rather than pretending it saved it.

### 19.5 Unicast, and why broadcast was never a candidate

RFC 4330 has both. Broadcast means waiting for a server on the LAN to announce the time
whenever it feels like it — NetX Duo's own client allows two hours between announcements —
which is not something a command you type can do; and it means believing whatever on the
LAN claims to be a time server, which is a security decision the user did not make.
Unicast asks a server the user named, and gets an answer or a timeout.

The client checks RFC 4330 §5's list and nothing beyond it: mode 4, `LI != 3`, stratum
1–15, a non-zero transmit timestamp, and an originate timestamp equal to the one sent. The
source address needs no check of its own, because the socket is `connect()`ed and the stack
has already dropped every datagram that did not come from the server.

### 19.6 The vendored SNTP add-on cannot be used from a Shell command — and neither can most of the command set

NetX Duo vendors an SNTP client at `third_party/netxduo/addons/sntp` and it is the obvious
thing to build this on. It cannot be used, for a reason that has nothing to do with SNTP,
and the reason rules out considerably more than SNTP.

**A Shell command links its own copy of ThreadX and NetX Duo, and that copy's kernel is not
running.** The one that is running lives inside `bsdsocket.library`, in that library's own
copy of the same archives, with its own scheduler globals. `nx_sntp_client_create()` calls
`tx_thread_create()`, `tx_timer_create()` and `tx_mutex_create()`;
`nx_sntp_client_run_unicast()` calls `nx_udp_socket_bind()`, which suspends the calling
thread. Every one of those reaches for ThreadX's scheduler state, and in a Shell command
that state belongs to a kernel that was never entered.

The same fact is already visible in three other places in the tree, and they are worth
naming together because they are one fact and not three:

* `src/tools/netstack_weak.c` supplies **weak** `netstack_get()` / `netstack_ip()` /
  `netstack_pool()` that return NULL, and no tool links `aminetxduo_netstack` — check any
  tool's `link.txt`. In a shipped build the weak stubs *are* the implementation, so
  `netstat`, `ping` and `ShowNetStatus`'s live path reach `tool_require_stack()`, get NULL,
  and say so. Measured in the same run, with the stack up and an address leased:

  ```
      netstat -r
        netstat: the network is up, but this command cannot read it
        ...
        The stack lives inside bsdsocket.library and there is no
        call yet that lets a separate command look inside it
  ```

  The `build/testhd-doclive/` capture that shows those commands printing live interface
  counters was produced by a purpose-built `LiveTools` binary that links the netstack and
  runs each command's `main()` in-process; the shipped executables cannot do it.
* `src/tools/onoff.c` says it outright: *"Individual interfaces cannot be taken up and down
  while the stack runs; that needs a call the library does not have yet."*
* `src/tools/nettrace.c` is the one command that solved it, and how it solved it is the
  shape of the answer: it reaches the capture engine through the eight published `bpf_*`
  LVOs, *"because a tool that linked the archive would get its OWN copy of the channel
  table and capture nothing at all."*

**What this cost.** `arp`, `AddNetRoute` and `DeleteNetRoute` were to be written in the
same batch as `sntp`. All three are behind this wall and none was shipped:

| Command | What it needs | Where that is |
|---|---|---|
| `arp` list | read `ip->nx_ip_arp_table[]` | inside `bsdsocket.library`'s `NX_IP` — unreachable |
| `arp` delete / flush | `nx_arp_static_entry_delete()`, `nx_arp_dynamic_entries_invalidate()` | ditto, and they suspend the caller |
| `AddNetRoute DEFAULT=` | `nx_ip_gateway_address_set()` | ditto |
| `AddNetRoute DST=/VIA=` | `nx_ip_static_route_add()` | ditto — **and** `NX_ENABLE_IP_STATIC_ROUTING` is not defined in `port/netxduo-amiga/inc/nx_user.h`, so the routing table is not in the build at all. `NX_IP_ROUTING_TABLE_SIZE` *is* set there, which reads as though it were, and is inert without the enable. |

`ObtainNetXDuoContext` (LVO -0x360, `src/bsdsocket/nxcontext.c`) is the nearest thing to a
way in: it hands out `netstack_ip()`, `netstack_pool()` and the stack's own adopt/orphan
hooks, which between them would be enough to *read* the ARP and routing tables — those are
plain memory reads once the caller holds the ThreadX baton. It is not the answer as it
stands, for two reasons. It is compiled only under `AMINETXDUO_TLS_CONTEXT`, so it does not
exist in the `-DAMINETXDUO_TLS=OFF` configuration at all, and a network command that works
only in a TLS build is not a command; and its function table carries the six NetX Duo entry
points `tls.library` needs and no UDP, no ARP and no routing, so it could not back the
modifying half of either command however the reading half were done.

**The shape of the fix**, if these commands are wanted: a small set of published LVOs on
`bsdsocket.library`, in exactly the idiom `bpf_*` already established — the vector table
has reserved slots at [124], [125] and [137]–[142]. `Online`/`Offline` against a running
stack, `netstat`, `ping`, `arp`, `AddNetRoute` and `DeleteNetRoute` all land on the same
handful of calls, so it is one piece of work rather than six. It belongs in
`src/bsdsocket/`, which is why it is written down here rather than done.

### 19.7 What the run does, and the one thing it borrows from the internet

`tests/tools/run-sntp.sh`, on an A1200 with the A2065 on SLIRP:

1. `ClockSet 0` — `tests/tools/clockset.c`, a test-only helper that puts the guest where a
   real Amiga with a dead battery is. It exists because FS-UAE hands its guest the host's
   wall clock, so under the emulator every run would otherwise start with the clock already
   right and the interesting half would never execute; and because the harness disk has no
   `C:Date` to do it with. `tests/tls/tls_api.c` does the same thing inline, for the same
   reason.
2. `AddNetInterface eth0`, DHCP, and the two `fetch`es *before*.
3. `sntp 10.0.2.2 TIMEOUT 5` against something that is not a time server, which must fail
   legibly; then `sntp <server> SHOW`, which asks and changes nothing; then `sntp <server>`.
4. The same two `fetch`es *after*.

**The time server is a real one on the internet, and that is a considered choice rather
than laziness.** SLIRP is a NAT and forwards outbound UDP perfectly well, so a real server
is reachable — measured: `time.apple.com` answers stratum 1 through it, from the guest. A
local one is not possible, because SNTP is UDP port 123 and ports below 1024 need root on
macOS and on Linux alike, and this suite does not ask for root. Giving `sntp` a `PORT`
argument would have made the run hermetic at the cost of a knob that exists only for the
test, which is the wrong trade. Everything else in the run is hermetic, against
`tests/curl/curlpeer.py`, and the script asks the time server **from the host, before
starting the emulator**, so that an unreachable one fails in ten seconds and looks like
what it is rather than like a bug in `sntp`.

One bug was found this way and is worth recording, because the symptom named the wrong
thing. The resolver can return a `hostent` it could not fill: the pointers are all
non-NULL, `h_length` is not 4, and the address is four bytes of nothing. `sntp` accepted
it, `connect()` recorded 0.0.0.0 quite happily — a connected UDP socket is only a stored
destination — and the failure surfaced two calls later as *"could not send the request"*,
which is an error about the wrong subsystem entirely. `fetch.c` already checked
`h_length != 4`; `sntp` now does too.
## 20. `traceroute`, `tftp` and `whois` — and what SLIRP will not let anyone test (2026-07-26)

Three commands, and one measurement that decided the design of the first.

### 20.1 The measurement: `IP_TTL` reaches the wire on a raw socket and not on a UDP one

A traceroute is a TTL and a listener. The classic Unix tool sends UDP datagrams to
ports nothing listens on and reads ICMP PORT_UNREACHABLE to know it has arrived; the
Windows one sends ICMP echo the whole way and reads the echo reply. Both need
`SOCK_RAW` to receive TIME_EXCEEDED, so §17's raw socket makes either available on
paper, and the choice looks like a matter of taste.

It is not. `setsockopt(IPPROTO_IP, IP_TTL)` at `src/bsdsocket/options.c:227` stores the
value on the socket, and **exactly one send path reads it**: `bsd_raw_send_packet()`
hands `sock->as_Ttl` to `nxd_ip_raw_packet_send()`. `bsd_send_udp()` calls
`nxd_udp_socket_send()`, and a NetX Duo UDP socket carries the TTL it was created with
— `NX_IP_TIME_TO_LIVE`, fixed at `nx_udp_socket_create()` time in `socket.c:821`.

A case label that compiles is not a TTL that reaches the wire, so this was measured
rather than read. `tests/tools/ttlprobe.c` sets `IP_TTL` on a raw socket and on a UDP
socket, reads it back, and sends one datagram from each; the A2065 frame dump —
`tests/trace/a2065pcap.py`, the view from inside the emulated hardware and below every
line of this stack — says what actually left:

```
   5 TX ttl=1   10.0.2.15 -> 8.8.8.8   ICMP echo-request  id=16705 seq=1
   7 TX ttl=5   10.0.2.15 -> 8.8.8.8   ICMP echo-request  id=16706 seq=5
   9 TX ttl=128 10.0.2.15 -> 8.8.8.8   UDP
  10 TX ttl=128 10.0.2.15 -> 8.8.8.8   UDP
```

The probe's own output for those same four sends:

```
raw ICMP: asked for TTL 1, getsockopt says 1, sendto returned 16 (errno 0)
raw ICMP: asked for TTL 5, getsockopt says 5, sendto returned 16 (errno 0)
UDP     : asked for TTL 1, getsockopt says 1, sendto returned 16 (errno 0)
UDP     : asked for TTL 5, getsockopt says 5, sendto returned 16 (errno 0)
```

**`getsockopt` reads back 1 and 5 on both sockets and the UDP datagrams leave with 128
anyway.** The library is not lying — it stored what it was told — but the value never
reaches the header. Frame 45 of the same capture, an ordinary DNS query, is `ttl=128`
as well, which is the default and confirms 128 is simply what UDP always sends.

So a UDP-probe traceroute on this stack would send every probe with a TTL of 128 and
report the destination as the first hop. `src/tools/traceroute.c` is **ICMP echo
throughout, with no switch to select the other**, because a switch that selects a
broken behaviour is not an option, it is a trap.

**What would be needed to offer the UDP form**, if it is ever wanted: `bsd_send_udp()`
would have to apply `sock->as_Ttl` to the datagram. NetX Duo has no per-send TTL
argument for UDP, so it means either `nx_udp_socket_create()` taking the current value
and being re-created when it changes, or the `nx_ip_packet_filter` trick §17.3 already
uses for the URG bit — write the byte on the way past and repair the header checksum
by RFC 1624. That is a `src/bsdsocket/` change and is not made here.

### 20.2 What FS-UAE's SLIRP does with a TTL: nothing at all

**SLIRP is a user-mode NAT, not a router. It does not decrement the TTL, and it never
generates ICMP TIME_EXCEEDED.** That was established before anything was concluded from
a traceroute's output, because a correct traceroute against a NAT that ignores TTL
looks exactly like a broken one.

The evidence is the same capture. Six probes to 8.8.8.8 with TTL 1, 2, 3, 4, 5 and 6:

```
  15 TX ttl=1 10.0.2.15 -> 8.8.8.8   ICMP echo-request  id=40490 seq=1
  16 RX ttl=255 8.8.8.8 -> 10.0.2.15 ICMP echo-reply    id=40490 seq=0
  17 TX ttl=2 10.0.2.15 -> 8.8.8.8   ICMP echo-request  id=40490 seq=2
  18 RX ttl=255 8.8.8.8 -> 10.0.2.15 ICMP echo-reply    id=40490 seq=0
  ...
  25 TX ttl=6 10.0.2.15 -> 8.8.8.8   ICMP echo-request  id=40490 seq=6
  26 RX ttl=255 8.8.8.8 -> 10.0.2.15 ICMP echo-reply    id=40490 seq=0
```

Every probe, whatever its TTL, is answered by an echo reply purporting to come from the
destination. Not one ICMP type 11 appears anywhere in any run, and neither does a type
3: `192.0.2.1` (TEST-NET-1) and `10.11.12.13`, addresses the *host* cannot reach, were
tried specifically to provoke an unreachable that quotes the probe, and SLIRP answered
one with silence and the other with a forged echo reply.

**And SLIRP zeroes the ICMP sequence number on a proxied reply while preserving the
identifier** — `seq=1..6` goes out, `seq=0` comes back every time. That is why
`traceroute 8.8.8.8` prints stars: the replies cannot be attributed to a probe, and
attributing them anyway would mean matching on the identifier alone, which with three
queries per hop would credit a stale reply to the wrong probe. The command is right to
reject them, and `-v` says so rather than leaving it looking like silence:

```
traceroute to 8.8.8.8 (8.8.8.8), 3 hops max, 60 byte packets
 1  (8.8.8.8 sent ICMP type 0) *
 2  (8.8.8.8 sent ICMP type 0) *
 3  (8.8.8.8 sent ICMP type 0) *
```

Frames 11–14 are the control: to `10.0.2.2`, which is SLIRP's own alias and is answered
locally rather than proxied, **both** the identifier and the sequence survive, and the
trace completes.

### 20.3 Verified on the wire, and inferred — stated separately

**Verified, from the A2065 frame dump:**

| | |
|---|---|
| `IP_TTL` on a raw socket reaches the IP header | TTL 1, 2, 3, 4, 5, 6 sent and observed, in order |
| `IP_TTL` on a UDP socket does **not** | asked 1 and 5, read back 1 and 5, sent 128 and 128 |
| `IP_TOS` reaches the wire on a raw socket | `-t 16` observed in the header's TOS byte |
| the ICMP echo requests are well formed | SLIRP and the far side both answer them, so the checksum is right |
| a raw ICMP socket receives inbound ICMP it did not solicit | the unattributable echo replies above arrive and are reported |
| a complete trace, end to end | `traceroute 10.0.2.2` and `traceroute 10.0.2.15` — one hop each, with timings |

```
===== SYS:traceroute 10.0.2.2 -m 4 -q 2 -w 3 -n =====
traceroute to 10.0.2.2 (10.0.2.2), 4 hops max, 60 byte packets
 1  10.0.2.2  12.0 ms  11.6 ms
```

**Not verified, because FS-UAE cannot produce the input:**

| | |
|---|---|
| attribution of an ICMP TIME_EXCEEDED to the probe that caused it | SLIRP never sends one |
| the hop-by-hop walk past the first hop | same |
| the `!H` / `!N` / `!X` unreachable annotations | SLIRP never sends a type 3 either |

Those three are the quoted-datagram path in `tr_classify()` — parse the ICMP header,
step over its eight bytes, parse the quoted IP header, check that the quoted payload is
our own echo with our identifier and sequence. **It is written and it is not proven.**
What is proven is everything either side of it: the probe leaves with the TTL asked for,
and inbound ICMP of a type we did not solicit reaches the raw socket and is examined —
the filter in `raw.c` keys on the IP protocol number, not on the ICMP type, so a type 11
takes exactly the same path to the reader as the type 0s that demonstrably arrive.

**This is a limitation of the emulator and not of the stack**, and the distinction is
not rhetorical: the TTL half is the half that lives in our code, and it is the half that
is measured on the wire. What is missing is a router between the guest and the
destination, and FS-UAE does not contain one. Confirming the rest needs real hardware on
a real network with at least one router in the path — the same class of gap as test 41
in §17.5, and recorded the same way rather than papered over.

### 20.4 The option set is Roadshow's, less three it cannot honestly back

```
traceroute MAXTTL=-m/N/K,NUMERIC=-n/S,QUERIES=-q/N/K,TOS=-t/N/K,WAIT=-w/N/K,
           VERBOSE=-v/S,HOST/A,PACKETSIZE/N/K
```

Roadshow's names and short forms, so a habit carries over. Three of its options are
**absent rather than accepted and ignored**:

| | |
|---|---|
| `-p PORT` | the destination port of a UDP probe. There are no UDP probes here, so there is no port. |
| `-r DONTROUTE` | `SO_DONTROUTE` is not in `bsd_setsockopt()` and NetX Duo has nothing to implement it with. |
| `-s SOURCE` | `bind()` on a raw socket records an address and nothing more — NetX Duo binds sockets to ports, and a raw datagram's source address comes from the route (`socket.c:902`). |

A traceroute that accepts `-s` and ignores it is one that lies about which interface it
went out of, which is worse than not offering it.

`PACKETSIZE` is the whole IP datagram, which is what the name has always meant, floored
at 28 (a header and an echo header with nothing after them) and defaulting to 60.

### 20.5 `tftp`: a client, octet only, and the transfer identifier

`docs/RESEARCH.md` 5.4 recorded that NetX Duo's TFTP add-on compiles except for the
server, which wants FileX. The client is not used either: the protocol is a couple of
hundred lines over the socket API, and written that way the command runs on Roadshow and
AmiTCP as well.

**Octet only, deliberately.** RFC 1350's other mode rewrites line endings in transit,
which destroys every binary anybody actually moves with this protocol; a switch offering
it would be a switch whose only use is to corrupt a ROM image.

The part worth testing is the **transfer identifier**: the request goes to port 69, the
answer comes from a different port the server picked, and every later packet belongs to
that port alone. `tests/tools/netpeer.py` grew a TFTP server that gives each session its
own socket exactly as a real one does, and logs the TID it chose, so the client's
handling of it is demonstrated rather than assumed. Four transfers, against it:

```
===== SYS:tftp 10.0.2.2 PORT 7069 GET hello.txt AS DH0:tftp-hello.txt =====
49 bytes
===== SYS:tftp 10.0.2.2 PORT 7069 GET big.bin AS DH0:tftp-big.bin =====
100000 bytes in 2.8 seconds (50000 bytes/s)
===== SYS:tftp 10.0.2.2 PORT 7069 GET exact.bin AS DH0:tftp-exact.bin =====
2048 bytes
===== SYS:tftp 10.0.2.2 PORT 7069 PUT DH0:greeting.txt AS from-amiga.txt =====
21 bytes
===== SYS:tftp 10.0.2.2 PORT 7069 GET no.such.file =====
tftp: there is no such file on the server (no such file)
```

`exact.bin` is 2048 bytes, an exact multiple of the block size, which is the case a
client that stops at the first short block gets wrong: the transfer ends with an *empty*
data block and both directions have to expect it. The host log confirms the shape from
the other side — `sent 'exact.bin', 2048 bytes in 5 blocks`, four full and one empty.

The duplicate-block path is the sorcerer's-apprentice one: a data block that arrives
twice is acknowledged again *without* advancing, because advancing doubles every packet
on the wire for the rest of the transfer.

### 20.6 `whois`: the default server is the one that knows where to ask

Twenty lines of protocol — connect, send the query, read until the far end hangs up.
The only decision in it is the default server, and it is `whois.iana.org` rather than
the traditional `whois.internic.net`, which has known only `.com` and `.net` for twenty
years. IANA's knows one thing about everything: which registry to ask. So the default
answers usefully for any TLD, any address range and any AS number, and the answer names
the server with the detail.

That referral is handled two ways. Without `FOLLOW` the line to type next is printed
underneath the record, which is the minimum a command owes someone it has just given a
partial answer to. With it, the referral is chased — up to three hops, and a server that
refers you to itself is recognised as a loop rather than followed. Four spellings are
recognised, case-insensitively, at the start of a line **after any indentation**:
`refer:` and `whois:` (IANA), `Registrar WHOIS Server:` (the gTLD registries) and
`ReferralServer:` (the RIRs, whose value carries a `whois://` scheme that is stripped).

The indentation is not a detail, and the first version of this got it wrong. IANA writes
its fields hard against the left margin; the gTLD registries indent every one of theirs
by three spaces. A matcher anchored at column zero therefore finds IANA's referral and
silently misses the registry-to-registrar one — which is the referral anybody looking up
a domain actually needs. It was caught by running the chain rather than by reading it.

Against real registries over the real internet, through SLIRP — two hops, unedited apart
from the elision:

```
===== SYS:whois amiga.com FOLLOW =====
% IANA WHOIS server
refer:        whois.verisign-grs.com
domain:       COM
organisation: VeriSign Global Registry Services
...
--- whois.verisign-grs.com ---
   Domain Name: AMIGA.COM
   Registrar WHOIS Server: whois.godaddy.com
   Registrar: GoDaddy.com, LLC
   Creation Date: 1994-06-25T04:00:00Z
```

`refer:` is taken from IANA's answer and the later `whois:` line in the same record is
not, because only the first referral counts — a registry answer that mentions several
servers means the one nearest the top, and chasing the last would follow whatever
happened to be in the legal notice at the bottom.

### 20.7 Where the harness grew

`tests/tools/run-nettools.sh` and `tests/tools/netpeer.py` were extended rather than
duplicated: the peer gained a TFTP server (UDP, per-session TID, both directions) and a
whois server whose canned records exercise the referral, the self-referral loop and the
no-match answer, and the command list gained the traceroute, tftp and whois cases quoted
above. `ToolsSmoke`'s staged-command ceiling went from 40 to 96 in the same change —
it truncates silently, and a run that quietly stops reading at line 40 looks exactly
like a set of commands that were never written.

One harness defect worth recording because it produced a page of convincing false
failures. `netpeer.py` was given a lifetime of the run's own timeout plus two minutes,
and emulator runs serialise on `build/.fsuae.lock`. With three other runs ahead of it in
the queue, the peer exited **before the guest had exchanged a single byte** — and every
local-server case then reported `connection refused` or `the server stopped answering
after 0 bytes`, which is indistinguishable from a broken command until the peer's own log
is read and shows it shut down at 540 seconds. The peer now outlives the queue rather
than the run.

---

## 21. Ten times Roadshow: where a Shell command's size actually goes (2026-07-26)

`Online` reads one configuration file and calls into `bsdsocket.library`. Roadshow's is
5,064 bytes. Ours was 44,396 — nearly nine times the size for the same job, and `-Os`
was already on. The standing hypothesis was newlib: that the C runtime's startup, its
`stdio` or its floating-point formatting was being dragged in behind everyone's back.

**It is not newlib.** The linker map says so, and the number is not close.

### 21.1 What Online was made of

`m68k-amigaos-gcc 15.2.0`, Release, `-Os`, on the pinned toolchain. `.text` = 37,072
bytes, by input object, with the map's "included to satisfy reference by" column:

| bytes | % | object | pulled in by |
|---:|---:|---|---|
| 8,952 | 24.1% | `tool_diag.c` | named in `add_executable()` |
| 8,376 | 22.6% | `config_parse.c` | `config_file.c` (`ami_cfg_parse_interface`) |
| 4,376 | 11.8% | `netdb.c` | `config_file.c` (`ami_netdb_load`) |
| 3,868 | 10.4% | `config_file.c` | `onoff.c` (`ami_config_load_interface`) |
| 3,804 | 10.3% | `config_text.c` | `tool_diag.c` (`ami_config_set_reporter`) |
| 1,896 | 5.1% | `onoff.c` — **the command itself** | — |
| 1,580 | 4.3% | newlib `strstr` | `crt0.o` |
| 1,420 | 3.8% | `tool_util.c` | named in `add_executable()` |
| 772 | 2.1% | newlib `crt0.o` | — |
| 764 | 2.1% | `compat.c` | `tool_diag.c` (`ami_alloc`) |
| 1,236 | 3.3% | newlib `mem*`/`str*` | `crt0.o`, `config_file.c`, `strstr.o` |
| 48 | 0.1% | `netstack_weak.c` | named in `add_executable()` |

**All of newlib is 3,588 bytes — 9.7%.** No `stdio`, no `malloc`, no `printf`, no
floating point: `dos.library`'s `Printf`/`VPrintf` and `ReadArgs()` did their job exactly
as the comment at the top of `src/tools/CMakeLists.txt` claims. The largest single newlib
item is `strstr` at 1,580 bytes — the Two-Way algorithm, referenced by `crt0.o`, which
then drags in `memchr`, `memcmp` and `strchr` for another 540 between them. That is the
whole newlib story, and `strstr` alone is 43% of it.

The size is in two things we wrote:

- **The configuration parser, 20,424 bytes — 55%.** `Online` calls
  `ami_config_load_interface()`; that reaches `ami_cfg_parse_interface()`, which lives in
  the same object as the gateway and resolver parsers; `config_file.c` also references
  `ami_netdb_load`, so the whole `/etc/hosts`, `/etc/services` and `/etc/protocols` store
  arrives with its built-in fallback tables. `Online` never calls any of it.
- **The prose diagnostics, 8,952 bytes — 24%.** `tool_diag.c` is what prints
  *"No network interfaces are configured"* and the paragraph of advice under it. It is in
  `TOOLS_COMMON_SOURCES`, so it is named in `add_executable()` for every command — and an
  object named in `add_executable()` is not an archive member. The linker has no choice:
  it goes in whole, whether the command explains anything or not.

That is the answer to the headline question. Roadshow's `Online` is 5 KB because the
configuration parsing lives in Roadshow's library and the command asks for it. Ours
carries its own copy, because there is no LVO to ask — and `bsdsocket.library` links the
*same* `aminetxduo_config` archive, so the parser is in the tree twice over.

### 21.2 What was actually fixed

Not the duplication — that is an interface question, and the same one being answered for
`netstack_ip()`. What was fixed is that **none of it could be dropped even when
unreachable**, because nothing in the build was compiled at function granularity.

- `-ffunction-sections` on `aminetxduo_config`, `aminetxduo_common` and the commands' own
  sources.
- `-Wl,--gc-sections` on the commands, and only on the commands.

| command | before | after | |
|---|---:|---:|---:|
| `Online` | 44,396 | **32,388** | −27.0% |
| `Offline` | 43,620 | 29,832 | −31.6% |
| `host` | 23,720 | **14,660** | −38.2% |
| `AddNetInterface` | 45,796 | 33,788 | −26.2% |
| `NetSetup` | 33,348 | 25,100 | −24.7% |
| `fetch` | 29,064 | 20,104 | −30.8% |
| `nc` | 31,792 | 22,116 | −30.4% |
| `telnet` | 30,460 | 20,428 | −32.9% |
| `ftp` | 36,612 | 27,268 | −25.5% |
| `NetTrace` | 31,964 | 22,152 | −30.7% |
| `sntp` | 27,620 | 18,104 | −34.5% |
| `tftp` | 30,932 | 20,548 | −33.6% |
| `traceroute` | 30,176 | 19,780 | −34.5% |
| `whois` | 28,200 | 17,352 | −38.5% |
| `ping` | 94,204 | 49,256 | −47.7% |
| `netstat` | 89,320 | 80,264 | −10.1% |
| `ShowNetStatus` | 111,920 | 105,676 | −5.6% |

`Online`'s `.text` goes 37,072 → 26,832. `host` — which loads no interface at all — goes
18,936 → 11,980, because `--gc-sections` throws the entire parser and netdb store away.

### 21.3 Why the flag is not in the toolchain file

`-ffunction-sections` costs nothing on its own; only a link that passes `--gc-sections`
sweeps anything up. Two measurements decided where each half goes.

**A `.library` must never be collected.** Its romtag is found by Exec scanning the loaded
segment, and no relocation points at it — exactly the shape a garbage collector removes.
Putting `--gc-sections` in `CMAKE_EXE_LINKER_FLAGS` for the whole tree took **60 KB out of
`tls.library`** and 19 KB out of `bsdsocket.library`. So `--gc-sections` lives in
`src/tools/CMakeLists.txt` and nowhere else.

**`-ffunction-sections` is not free for the library either.** In the tree-wide form it
adds per-section padding to every archive the library links: +4,448 bytes on
`bsdsocket.library`, +3,552 on `tls.library`. Scoped to `aminetxduo_config` and
`aminetxduo_common` — the two archives a command takes a slice of — every config-only
command comes out **byte-for-byte identical**, and the cost falls to +1,848 bytes on
`bsdsocket.library` and **zero** on `tls.library`.

The NX-linked three (`ping`, `netstat`, `ShowNetStatus`) would gain another 12–51 KB from
extending the flag to `threadx`, `threadx_port`, `netxduo_port` and `aminetxduo_sana2` —
`netstat` reaches 29,276 that way. That is deliberately not done: those three link a
complete dead copy of the stack, which is a bug being fixed by publishing LVOs, and paying
`bsdsocket.library` +2,600 bytes to shrink code that is about to be deleted is the wrong
trade. When they become config-only they inherit the config-only numbers for nothing.

### 21.4 The one thing `--gc-sections` quietly took

Every command declares

```c
static const char version_tag[] __attribute__((used)) = "$VER: Online 2.0 (26.7.2026)";
```

and nothing ever reads it: AmigaDOS's `Version` finds it by scanning the file. `used`
stops the *compiler* discarding it and says nothing to the linker. The attribute that
would speak to the linker, `retain`, **this toolchain ignores** — gcc 15.2.0 answers
`warning: 'retain' attribute ignored`, because the m68k assembler has no
`SHF_GNU_RETAIN`.

So with `-fdata-sections` the tag gets a section of its own, nothing relocates to it, and
`--gc-sections` deletes it. Measured: **all seventeen commands lost their `$VER:` string**
and every test still passed. Without `-fdata-sections` the tag shares the object's
read-only section with the string literals `main()` prints, which are referenced, so it
survives. That costs 150–1,100 bytes per command against the `-fdata-sections` variant,
and it is the right 150–1,100 bytes to spend.

Beyond that one case, `--gc-sections` is sound here by construction: it removes an input
section only when no relocation from a live section points at it, and a Shell command has
no AmigaOS mechanism that reaches code without a relocation — the romtag scan belongs to
libraries, and the only inline assembly in `src/tools/` addresses registers and library
offsets, never a symbol. What it actually took out of `Online` is 46 symbols, and the list
reads exactly as it should: the gateway and resolver parsers, the whole `ami_netdb_*` API,
`tool_explain_resolve`, `tool_format_mac`, the `netstack_*` weak stubs. `ami_netdb_load()`
is called from `bsdsocket.library` and from `shownetstatus.c` and nowhere else — so
`ShowNetStatus` keeps the built-in `127.0.0.1 localhost loopback` table and `Online` and
`AddNetInterface` lose it, which is the correct answer in all three cases.

Checked on the machine as well as on paper. `ToolsSmoke` under FS-UAE, run twice from a
clean checkout — once as committed, once with these flags — produces **byte-identical**
output, down to the `rc 10`, the `NAME/A,QUIET/S` template echo and the harness timeout at
`AddNetInterface eth0` that both builds share.

That reasoning is correct but fragile — a future flag change breaks it silently, with no
test failing. `cmake/check-version-tag.cmake` therefore greps the linked binary after
every link and fails the build if the tag is gone. It has already caught one real case: a
stale `CMakeCache.txt` still carrying `-fdata-sections` from an earlier configure.

### 21.5 What is left, and what would move it

`Online` after the change, `.text` = 26,832:

| bytes | object |
|---:|---|
| 8,496 | `tool_diag.c` |
| 6,844 | `config_parse.c` |
| 3,132 | `config_text.c` |
| 1,896 | `onoff.c` |
| 1,636 | `config_file.c` |
| 1,580 | newlib `strstr` |
| 1,204 | `tool_util.c` |
| 772 | newlib `crt0.o` |
| 892 | newlib `mem*`/`str*` |
| 380 | `compat.c`, `netstack_weak.c` |

`netdb.c` is gone entirely — 4,376 bytes of `/etc/hosts`, `/etc/services` and
`/etc/protocols` handling that `Online` was carrying and could not reach.

Nothing here is waste in the sense that `--gc-sections` understands. `tool_diag.c` and
the parser are code the command genuinely reaches, and they are the difference between
our `Online` and Roadshow's: theirs prints an error number, ours prints a paragraph that
names the file, the device and the thing to fix. Closing the remaining 5× would mean
moving the parser and the diagnostics behind `bsdsocket.library`'s LVOs so a command asks
instead of carrying — the same shape as the `bpf_*` idiom `NetTrace` already uses, and
the same shape as the `netstack_ip()` fix. That is an architecture change, not a build
flag, and it is the only lever left that is worth anything.

### 21.6 Found on the way: the libraries are not built at `-O2`

`cmake/toolchain-m68k-amigaos.cmake` says

```cmake
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
```

and every library object is in fact compiled `-O3`. CMake's `Compiler/GNU` module
*appends* its own default to whatever a toolchain file put in `_INIT`, so the real flags
are

```
-m68020 -fomit-frame-pointer -fno-strict-aliasing -O2 -DNDEBUG -O3 -DNDEBUG
```

and the last `-O` wins. The commands are unaffected — `src/tools/` appends `-Os` after
that, which is exactly why the comment there says "appended after `CMAKE_C_FLAGS_RELEASE`
so it wins the duplicate `-O`" — but "`-O2` stays for the libraries" has never been true.
`bsdsocket.library` and `tls.library` are `-O3` builds, and every throughput and size
figure recorded for them was measured that way.

Deliberately not changed here: setting `CMAKE_C_FLAGS_RELEASE` as a cache variable rather
than `_INIT` would fix it in one line, but it would also re-optimise the whole stack and
invalidate the numbers in §13, §15 and §18. It is a decision, not a bug fix.

Dead ends, recorded so they are not retried: `-noixemul` still breaks `sys/reent.h`
(§5.4) and would not have helped anyway, since newlib is 9.7% of the problem;
`__attribute__((retain))` is ignored by this toolchain; and tree-wide `--gc-sections`
destroys every `.library` in the build.

## 22. The commands that could not see the stack (2026-07-26)

Three commands shipped in the v0.2.0 archive that cannot do what they claim, and the
reason is one sentence long: **a Shell command links its own copy of ThreadX and NetX
Duo, and that copy's kernel is not running.** §19.6 wrote that down and named the fix.
This is the fix.

### 22.1 What was actually broken, measured rather than reasoned

`src/tools/netstack_weak.c` supplies **weak** `netstack_get()` / `netstack_ip()` /
`netstack_config()` / `netstack_interface_is_up()` that answer NULL, NULL, NULL and
FALSE. Its own header comment says *"Delete this file once src/netstack exists and is
linked into the tools."* It was never deleted, and **no tool links
`aminetxduo_netstack`** — `src/tools/CMakeLists.txt` links `aminetxduo_sana2
aminetxduo_config threadx_port netxduo_port` and nothing else. So in every shipped
build the weak stubs *are* the implementation.

The consequences, one per call site:

| Call site | What it got | What the user saw |
|---|---|---|
| `ping.c:167` `ip = netstack_ip()` | NULL | `ping: the network is up, but this command cannot read it`, exit 5 |
| `netstat.c:381` `ip = netstack_ip()` | NULL | the same, for `-i`, `-r`, `-a` and `-s` alike |
| `shownetstatus.c:835` `ip = netstack_ip()` | NULL | the configuration, and no live column at all |
| `shownetstatus.c` `netstack_interface_is_up(i)` | FALSE | **`offline`** printed beside an interface carrying traffic |
| `onoff.c` `netstack_startup()` | `AMI_NET_ERR_STATE` | *"Individual interfaces cannot be taken up and down while the stack runs"* |
| `netstat.c:401` `netstack_config()` | NULL | every interface named `?` |

**Why nothing caught it.** Each of those is the *graceful* branch. The commands were
written to behave well when the network is not up, they behaved well, and a well-formed
"the network is not up" message reads as a pass in a transcript. Worse, the one capture
in the tree that shows these commands printing live interface counters —
`build/testhd-doclive/` — was produced by a purpose-built `LiveTools` binary that links
the netstack and calls each command's `main()` in-process. That binary works. The
shipped executables never could, and nothing compared the two.

### 22.2 Why linking the netstack into the tools would not have fixed it

It is tempting to read "no tool links `aminetxduo_netstack`" as a missing line in a
`CMakeLists.txt`. It is not. Adding it gives the command a *second* NetX Duo: its own
`NX_IP`, its own packet pool, its own ThreadX scheduler globals, its own SANA-II
attachments — none of them the ones the running stack is using. `netstack_ip()` would
stop returning NULL and start returning a pointer to an `NX_IP` with no interfaces in
it, and `netstat -i` would print an empty table instead of an error. That is strictly
worse: the error was at least true.

The same fact had already been met and solved twice in this tree, which is why the
answer was not in doubt:

* `src/tools/nettrace.c` reaches the capture engine through the eight published `bpf_*`
  LVOs *"because a tool that linked the archive would get its OWN copy of the channel
  table and capture nothing at all"*.
* `src/tools/sntp.c` does not use the vendored NetX Duo SNTP add-on, for the same
  reason (§19.6).

### 22.3 Two LVOs, and why a snapshot rather than a pointer

`include/aminetxduo/netstatus.h`:

```
    NetStackQuery(magic d0, what d1, buffer a0, size d2)     LVO -0x366
    NetStackControl(magic d0, op   d1, arg    a0, size d2)   LVO -0x36c
```

**Two, not six.** `Query` takes a selector — `SYSTEM`, `INTERFACES`, `STATS`, `ARP`,
`ROUTES`, `SOCKETS` — so a seventh table costs a selector rather than a vector. `Control`
is separate from `Query` on purpose: a caller that only reads cannot get a mutation by
mistyping a number.

**A snapshot, not an `NX_IP *`.** Handing out the live pointer is the smaller change and
the wrong one. AmigaOS has no memory protection, so a pointer into the stack's
structures stays dereferenceable long after the stack has gone down — and this project
has already shipped one use-after-free of exactly that shape, a teardown path that freed
a reply port and the stack a thread was still running on. Walking those tables also
requires holding the ThreadX baton, which a Shell command must not do while it prints.
So the library copies scalars into the caller's own buffer under one `bsd_nx_enter()`
bracket and hands the baton straight back. Nothing the caller holds afterwards points
into the stack.

The buffer starts with a `NetStatusHeader`. The caller writes a magic and the version it
was built against; the library writes back the entry size **it** was built with, how many
entries it wrote, and how many it had. A caller with a small buffer therefore learns how
big one it needs instead of being silently truncated, and a half-installed pair —
`bsdsocket.library` from one build, `netstat` from another — is caught at the size check
rather than printing plausible nonsense from shifted offsets.

**Where the slots are.** Both sit past every offset any published bsdsocket ABI names:
past AmiTCP V3, past V4, past Roadshow's extension set, and past the six
reserved-for-expansion slots `clib/bsdsocket_protos.h` documents after `getnameinfo()`.
§19.6 suggested `[124]`, `[125]` and `[137]`–`[142]`; those were **not** used, because
`[137]`–`[142]` are precisely Roadshow's documented expansion reservation and `[124]`/
`[125]` sit inside the published table between `gethostbyaddr_r` and `ipf_open`.
`tools/gen_vectors.py` already knew this and said so — `0x360` is described there as
"the first slot after ... every offset any published bsdsocket ABI assigns" — so the new
pair went after it, at `0x366` and `0x36c`.

**One latent bug had to be fixed to put them there.** `bsd_ObtainNetXDuoContext` at
`-0x360` was emitted inside `#ifdef AMINETXDUO_TLS_CONTEXT`, and the table is a dense
array, so in a `-DAMINETXDUO_TLS=OFF` build the slot **vanished** and everything after it
would have moved down six bytes. The slot is now unconditional and carries `bsd_enosys()`
when TLS is off. That is the rule `bpf.c` had already written down for its own eight:
*"a caller gets a documented failure instead of a jump into a slot that means something
else in the next build."*

`ObtainNetXDuoContext` was also rejected as the way in, for the reason §19.6 gives: it
exists only in a TLS build, and a network command that works only in a TLS build is not
a command.

**The caller cannot skip the identity check.** `tool_netstatus_open()` refuses a
`bsdsocket.library` that is not ours before it calls either vector. The magic argument
guards against a *future* vendor defining the same offset; only the identity check guards
against a *present* one that has not defined it, where the slot is whatever that table
happens to end with — possibly the `(APTR)-1` terminator.

### 22.4 There is no ping vector, and the measurement that decided it

The obvious third LVO is "do a ping for me", and it was designed and then thrown away.

`nx_icmp_ping()` matches an inbound echo reply on the **sequence number alone**.
`nx_icmp_interface_ping.c:222` builds the request with the identifier set to the low 16
bits of the outgoing interface's address and the sequence from a per-`NX_IP` counter, and
stores only the sequence on the suspended thread (`:308`). `nx_icmpv4_process_echo_reply.c:124`
then compares:

```c
    if ((USHORT)(thread_ptr -> tx_thread_suspend_info) == sequence_num)
```

and nothing else — not the identifier, not the source address. `nx_icmpv4.h:191` says so
outright: the identifier *"is not used as a host"*.

§20.2 measured, on the A2065 frame dump, that **FS-UAE's SLIRP zeroes the ICMP sequence
number on a proxied reply and preserves the identifier**. Those two facts multiply. A
vector wrapping `nx_icmp_ping()` would match on the one field the NAT destroys and ignore
the one it keeps, so `ping 8.8.8.8` would time out on every probe except the very first
of the `NX_IP`'s lifetime (the counter starts at 0, and `nx_ip_create.c:106` memsets it)
— while the replies arrived, were counted in `nx_ip_ping_responses_received`, and were
dropped.

So `ping` was rewritten over `SOCK_RAW` instead, the same path `traceroute` already uses.
That is not a workaround; it is better on every axis that matters here:

| | `nx_icmp_ping()` via a new LVO | `SOCK_RAW` via the published ABI |
|---|---|---|
| reply attribution | sequence only, fixed | the command's own rule |
| identifier | the interface address — same for every process | per-task, so two pings do not steal each other's replies |
| Ctrl-C during the wait | suspended in ThreadX, where an Exec signal means nothing | `WaitSelect` with a 200 ms cap |
| runs on Roadshow / AmiTCP | no | yes |
| new ABI surface | one more vector | none |

`ping` now matches on identifier **and** source address, and accepts either the expected
sequence or zero. Accepting zero is what makes it work through SLIRP, and it is safe
*here* and not in `traceroute`: this command has exactly one probe outstanding at a time,
so there is no other probe a reply could be credited to. `traceroute` sends three per hop
and is right to print stars instead.

### 22.5 Routing, which had to be decided before anything could report it

**`NX_ENABLE_IP_STATIC_ROUTING` is not defined in `port/netxduo-amiga/inc/nx_user.h`**, so
`nx_ip_static_route_add()`, `nx_ip_static_route_delete()` and the `nx_ip_routing_table[]`
they fill are not in the build at all. `NX_IP_ROUTING_TABLE_SIZE 4` **is** set there,
which reads as though they were, and is inert without the enable.

It stays off. What exists without it is exactly two kinds of route, and both are real: the
directly-attached prefix of each interface that has an address, and the default gateway,
which `nx_ip_gateway_address_set()` maintains in every build. That is what a machine on
one Ethernet actually has. So `NETSTATUS_ROUTES` reports those rather than reporting an
empty table and letting the user conclude their network is misconfigured, `NETSTATUS_SYS_ROUTING`
tells a caller which world it is in without guessing, and `NETCTRL_ROUTE_ADD`/`DELETE`
return `ENOSYS`. Turning the option on is a `port/` change with a rebuild of every
`NX_IP`-sized structure behind it, and it belongs to whoever writes `AddNetRoute` rather
than being switched on speculatively here.

`arp` is not blocked on anything: `NETSTATUS_ARP` reads the cache and `NETCTRL_ARP_ADD` /
`ARP_DELETE` / `ARP_FLUSH` are implemented over `nx_arp_static_entry_create()`,
`nx_arp_entry_delete()` and `nx_arp_dynamic_entries_invalidate()`. Only the command
itself is missing. `nx_arp_entry_delete()` rather than the static-only spelling, because
the person typing `arp -d` neither knows nor should have to know which kind of entry
answers to the address.

### 22.6 The ARP cache walk that spins instead of failing

Worth restating where the new code is, because it is the one loop here that punishes a
plausible mistake with a hang rather than an error: NetX Duo's ARP table is a hash table
of **circular** lists. `nx_arp_active_next` of the last entry in a bucket points back at
the bucket head, not at `NX_NULL`. A walk written the obvious way never terminates — and
it would not terminate *inside the ThreadX bracket*, with the baton held, which stops the
IP thread and every other socket user on the machine.

### 22.7 The regression test is the point

`tests/tools/run-livetools.sh`. It brings the stack up with `AddNetInterface eth0` and
then runs **the shipped executables**, from the staged directory a user would install,
through `ToolsSmoke` in a single emulator boot — not a purpose-built binary that links
the netstack, because that is exactly how this stayed hidden for a release.

Two halves, and both are needed:

* **Negative.** A list of the real sentences that mean *"I cannot see the stack"* —
  `the network is up, but this command cannot read it`, `the network has not been
  started`, `the stack is running but has no IP instance`, and the rest — none of which
  may appear anywhere after the interface is up. This is the assertion a human would
  make, and it is written against the *messages* rather than the exit codes, because the
  failing commands exited 5 (`RETURN_ERROR`) and a `ToolsSmoke` transcript records that
  where nobody reads it.
* **Positive.** The leased address `10.0.2.15`, the gateway `10.0.2.2`, a receive counter
  that is not zero, and at least one echo reply. Without these, a command could pass the
  negative half by printing nothing at all.

**It needs no host-side peer, and that is deliberate.** Every other command test starts a
Python server on the host with a lifetime; with several agents queued on the FS-UAE lock
that lifetime can expire before the guest has booted, and then every case fails with
"connection refused" — which looks exactly like a broken command and is not.
`tests/tools/netpeer.py` did precisely that. SLIRP answers DHCP itself, answers ICMP to
its own alias `10.0.2.2` itself, and lives exactly as long as the emulator does. There is
nothing in this test that can time out while the run waits its turn, so a failure from it
is a failure in the commands.


### 22.8 What the shipped commands print now, and the one that still does not

The commands were run from a staged directory, as a user would install them, through
`ToolsSmoke` in one boot. This is the shipped `netstat`, the shipped `ShowNetStatus`,
reading the stack that `AddNetInterface` brought up in `bsdsocket.library`:

```
    ===== SYS:ShowNetStatus =====
    Network stack:  running
    Host name:      amiga
    Default route:  10.0.2.2

    Interface eth0 (a2065.device unit 0)
      state       online          link up
      address     10.0.2.15       netmask 255.255.255.0 (/24)
      broadcast   10.0.2.255
      hardware    00:80:10:32:33:34
      mtu         1500 bytes        10000000 bits/s
      configured  DHCP

    ===== SYS:netstat -i =====
    Name    Mtu   Address          Link   Ipkts      Ierrs  Opkts      Oerrs
    eth0    1500  10.0.2.15        up     2          0      1          0

    ===== SYS:netstat -r =====
    Destination      Gateway          Netmask          Flags  Interface
    default          10.0.2.2         0.0.0.0          UG     eth0
    10.0.2.0         *                255.255.255.0    U      eth0
    127.0.0.0        *                255.0.0.0        U      lo0

    ===== SYS:netstat -s =====
    ip:
            2 packets sent (616 bytes)
            2 packets received (578 bytes)
    udp:
            2 datagrams sent (600 bytes)
            2 datagrams received (562 bytes)
    packet pool:
            256 packets of 1568 bytes, 222 free

    ===== SYS:netstat -a =====
    Proto  Local  Foreign               State
    udp    68     *                     0 queued
    udp    0      *                     0 queued
```

Every one of those numbers came out of the running `NX_IP` through `NetStackQuery()`.
Before this work each of those five commands printed *"the network is up, but this
command cannot read it"* and exited 5. **`netstat` and `ShowNetStatus` are verified.**

**`ping` is not.** It does not merely fail — **it takes the machine down, and the machine
comes back up and does it again.** That is the finding, and it took three runs and a
correction to see it.

The symptom in the transcript is a command that produces no output and a run that ends on
its timeout. That reads as a hang, and it was recorded as one twice before the boot
counter was looked at:

| run | commands in the list | `netstack: starting ThreadX` in the serial log |
|---|---|---|
| `pingtrace` | `AddNetInterface`, one `ping` | **6** |
| `livetools` | the full list | **10** |
| `livecheck` | the full list plus `host` | **14** |

One boot starts the stack once. The guest is restarting, `ToolsSmoke` opens
`DH0:tools.txt` afresh each time it starts, and the file therefore truncates and refills
to exactly the same point on every pass — which is why watching its length shows it
shrink from 184 lines to 75 and climb back, and why the instrumented `ping` printed its
trace three times for a list that contains one `ping`.

**Where it stops is measured.** `AMI_INFO` writes to the serial port unbuffered, which is
the only way to see inside a command that never exits — the Shell holds a command's stdout
until it terminates, so a command that dies prints nothing wherever it got to:

```
    pingtrace: opening the library
    pingtrace: library open, resolving
    pingtrace: resolved 0A000202, opening a raw socket
    pingtrace: raw socket 0
    pingtrace: entering the loop
    pingtrace: send 64 seq 0
    pingtrace: sendto returned 64
    pingtrace: select left=200
    pingtrace: select returned 1
    pingtrace: recvfrom
    <the machine restarts>
```

Every stage before the read is fine: the library opens, the raw socket opens, the echo
request is built and `sendto()` returns the full 64 bytes. `WaitSelect()` then reports the
descriptor readable, and **the `recvfrom()` that follows never returns**. In the blocking
build it went into `bsd_raw_receive()`'s `tx_semaphore_get(TX_WAIT_FOREVER)` with
`as_RawHead` empty — so `select()` and the queue disagreed. Making the descriptor
non-blocking (`FIONBIO`) so the read cannot suspend **did not fix it**, which rules the
blocking wait out as the whole story and puts the fault inside the raw receive path
itself.

That path is `src/bsdsocket/raw.c` and `src/bsdsocket/transfer.c`. `bsd_recv_raw()` does
check its packet for `NX_NULL` and answers `EWOULDBLOCK`, so the obvious null dereference
is not it. Finding the rest belongs with whoever owns those files, and the two facts to
hand over are: `select()` reports a raw descriptor readable when `bsd_raw_receive()` will
find nothing queued, and the read that follows takes the machine down.

`traceroute` reaches the wire through the same helpers and completes (§20.3), so this is
not "raw sockets do not work" — it is something narrower, and the difference between the
two commands is the place to start.

**Until that is fixed, `ping` must not be described as working, and should not be shipped
at all**: a command that reboots the machine is worse than the one that printed "the
network is up, but this command cannot read it".

**The `NUMERIC` change stands on its own** and is not part of this. A reverse lookup
through `gethostbyaddr()` costs `BSD_RESOLVE_TIMEOUT` — thirty seconds
(`src/bsdsocket/resolver.c:18`), retried per name server, against a server that need not
answer a PTR query, and FS-UAE's SLIRP does not. That is spent *before the first packet
leaves*, which is the one place a ping cannot be slow, for a cosmetic change to one line
of output; `ping` reads `DEVS:Internet/hosts` instead. It was the first explanation
offered for the hang and it was **wrong** — the run with the DNS query already gone
behaved identically — and that is recorded here rather than quietly dropped, because a
wrong cause in this document is worse than an open question. The thirty seconds remain a
real property of the stack that any command reverse-resolving an address will pay, and
`ShowNetStatus NAMES` and `host` both do.

### 22.9 The check that stops a new command guruing on an old library

`tool_netstatus_open()` refuses a `bsdsocket.library` whose `lib_IdString` is not ours
before it calls either vector, because on somebody else's library that offset is whatever
their table happens to end with. Writing that down made the other half obvious and it had
been missed: **it is also whatever OURS ends with.**

In the v0.2.0 library — which is published — `-0x366` is past the last vector, on the
`(APTR)-1` terminator `MakeLibrary()` puts there. A user who installed the new commands
over the old library would have got a guru, which is a worse answer than the message this
whole interface exists to stop being printed, and is precisely the class of defect §22 is
about.

`lib_Version` cannot carry this. It is 4, it is the AmiTCP V4 ABI number every caller
passes to `OpenLibrary("bsdsocket.library", 4)`, and moving it locks out every program
that asks for 4. `lib_Revision` is ours, so it is what says *which* of our libraries this
is:

| `lib_Revision` | |
|---|---|
| 0 | v0.2.0 and earlier |
| 1 | `NetStackQuery`/`NetStackControl` at `-0x366`/`-0x36c` |

`AMI_NETSTATUS_MIN_REVISION` is the caller's half, and a command that meets an older
library names the revision it found and the one it needs. Bump the two together whenever
a slot is added.

### 22.10 What `netstat -s` says about a ping, and why it looks wrong

`ping` sends through `SOCK_RAW` now, so it never calls `nx_icmp_ping()` and
`nx_ip_pings_sent` never moves. Replies are a different matter:
`src/bsdsocket/raw.c`'s filter **copies the datagram and then declines it** — it always
returns `NX_NOT_SUCCESSFUL`, so the stack goes on to process the packet normally and
`_nx_icmpv4_process_echo_reply()` increments `nx_ip_ping_responses_received` as it
always did.

So after a successful `ping`, `netstat -s` reports echo **replies received** and **zero
requests sent**. Both numbers are true, and the asymmetry is a fact about where the
requests go rather than a counter that is broken.


## 23. The command line is part of the contract (2026-07-26)

§22 is about commands that could not see the stack. This one is about what the
user types at them, which had drifted independently and for a different
reason.

Four commands had kept the names they are supposed to have and quietly grown
their own options. That is not a matter of taste. The premise of this project
is that a program, a script or a person that worked against the reference
stack works here, and the first thing any of the three touches is the argument
template. A command whose template differs is a compatibility bug in the same
sense that a missing library vector is one — it just fails at a layer that is
easier to look at and therefore easier to excuse.

The reference was the per-command documentation shipped with the Roadshow 1.15
demo, read for one thing only: names, keywords, flags and argument shapes. No
wording from it is reproduced anywhere in this tree.

### 23.1 Before and after

| command | was | is |
| --- | --- | --- |
| `ping` | `HOST/A,COUNT/N/K,SIZE/N/K,TIMEOUT/N/K,DELAY/N/K,QUIET/S` | `-c=COUNT/K/N,-i=INTERVAL/K/N,-l=LOAD/K/N,-n=NUMERICONLY=NUMERIC/S,-o=ONEREPLY/S,-q=QUIET/S,-s=SIZE/K/N,-t=TIMEOUT/K/N,BELL/S,HOST/A` |
| `AddNetInterface` | `NAME/A,QUIET/S` | `INTERFACE/M,QUIET/S,TIMEOUT/K/N` |
| `Online` / `Offline` | `INTERFACE/A,QUIET/S` | `NAME/A,UNIT/N,TIMEOUT/N` |
| `ShowNetStatus` | `INTERFACE/K,STATS/S,ALL/S` | `INTERFACE/M,INTERFACES/S,ARPCACHE=ARP/S,ROUTES/S,DNS=DOMAINNAMESERVERS/S,ICMP/S,IP/S,MB=MEMORY/S,TCP/S,UDP/S,TCPSOCKETS/S,UDPSOCKETS/S,NAMES/S,ALL/S,REPEAT/S,QUIET/S` |

`netstat` keeps its template and is discussed in 23.5.

The dual-form convention — `-c` and `COUNT` naming one option — was already in
`nc` and `netstat` (`LISTEN=-l/S`, `INTERFACES=-i/S`) and is now in `ping` as
well. ReadArgs takes more than one alias per item, so
`-n=NUMERICONLY=NUMERIC/S` is a single switch with three spellings. That was
verified under FS-UAE before anything was built on it, because a malformed
template does not fail loudly: it makes *every* invocation of the command fail
with "required argument missing", which reads like a bug in the command.

### 23.2 `ping`: implement it or leave it out

Ten of the fourteen options are implemented. Four are absent, and absent is the
point — a template that accepts `RECORDROUTE` and then does not record the
route is worse than one that rejects it, because the second tells the truth
immediately.

* `-d`/`DEBUG` sets `SO_DEBUG` on a socket.
* `-R`/`RECORDROUTE` needs the IP RECORD_ROUTE option on the outbound packet.
* `DONTROUTE` is a routing bypass.
* `-v`/`VERBOSE` lists received ICMP that is not an echo reply.

Two of the ten are worth spelling out because their *meaning*, not just their
name, had drifted:

**`TIMEOUT` is the whole run, not one reply.** Ours used to be the per-reply
wait, so `ping host TIMEOUT 5` with the default count waited up to twenty
seconds — four times what anyone typing it expects. It is now the elapsed
limit it is documented to be everywhere, `0` meaning no limit, and the
per-reply wait is fixed and not exposed. The reply wait is clamped to the time
remaining, so a short `TIMEOUT` cannot be overshot by one outstanding request.

**`INTERVAL` replaces `DELAY`, and it is seconds.** `DELAY` was a name we
invented, taking milliseconds. The default is one second either way, so
nothing that did not name the option changes.

`-l`/`LOAD` is honest but partial, and the code says so: a preload sends its
packets with no wait *between* them.

`-n`/`NUMERIC` had to be given something to suppress before it could be
accepted, so a numeric HOST is now looked up backwards for the header and
summary lines, through the local `DEVS:Internet/hosts` and then the resolver.
`-n` skips that.

### 23.3 `AddNetInterface`: `/M` and a DHCP allowance

`INTERFACE/M` accepts a list, capped at `AMI_CFG_MAX_INTERFACES` because that
is what the parsed configuration holds, sorted before use. The files are read
TWICE on purpose: the first pass only parses, and aborts before anything is
started if any file is unusable. Without that, a list of three with a typo in
the third brings two interfaces up and then fails, which is the one outcome a
boot script cannot recover from. Nothing is printed until every file has been
checked either, or the report implies that the first two were started.

`TIMEOUT` is the DHCP allowance. Ten seconds is both the default and the
floor: a shorter limit expires before the exchange can finish and reports a
failure that is not one. An interface whose address is in its file waits for
nothing.

### 23.4 `Online` / `Offline`: one argument, two kinds of thing

This one needed a decision rather than a rename. The reference commands take a
**SANA-II device driver name and a unit** — that is the level at which a
driver is switched off to run diagnostics on a card. Ours took a **configured
interface**, which is the handle every other command here uses.

Both are now accepted, resolved in this order:

1. an interface, if `DEVS:NetInterfaces/<NAME>` parses. A `UNIT` that
   contradicts the unit in that file is an error, not a silent override.
2. otherwise a driver, matched on the last path component of `DEVICE` and on
   `UNIT` against every interface file in the drawer. The command says which
   interface it picked.
3. otherwise neither, and the message lists every interface with its driver
   and unit, so both spellings are visible in the same table.

A name that is both is taken as the interface and says so, naming the other
candidate. The drawer is scanned directly rather than through
`netstack_config()`, so the resolution works with the stack down.

`QUIET` is gone: it is not in the interface these commands are supposed to
have, and this project carries no legacy burden.

`TIMEOUT` bounds the wait for the interface to reach the requested state, `0`
meaning indefinitely, as documented; Ctrl-C aborts it either way.

### 23.5 `ShowNetStatus` and `netstat`: keep both, share the data

The decision, and the reasoning:

* **`ShowNetStatus` grows the full category set.** It is the Amiga-shaped
  introspection command — named categories, a general summary when no category
  is given, and a "what to look at" diagnosis aimed at somebody who does not
  yet know what is wrong. That summary and that diagnosis are why a beginner
  types it, and they have no equivalent anywhere else.
* **`netstat` stays as it is.** It is the BSD-shaped convenience: `-i`, `-r`,
  `-a`, `-s`, columns, and nothing but data. It is ours rather than
  Roadshow's, so it owes nothing to this section, and it is what a decade of
  Amiga documentation and install scripts type.
* **Neither reads the stack for itself.** Both take the same two snapshots
  from `tool_nx.c` — `ToolSnapshot` for interfaces, routes and sockets,
  `ToolStats` for the protocol counters, the ARP cache and the packet pool —
  so they cannot report different values for one fact, and a counter added in
  one place is available to both. Only the layout differs.

Three categories are absent because there is nothing behind them:

* `IGMP` — `nx_igmp_enable()` is never called, so there is no group
  membership and the counters do not exist.
* `MR`/`MULTICASTROUTING` — there is no multicast router to have statistics
  about.
* `RT`/`ROUTING` as a *statistics* category — no routing counters are kept,
  and `NX_ENABLE_IP_STATIC_ROUTING` is off, so the routing table is the
  connected routes plus the default gateway. `ROUTES` prints exactly that.

The rest are implemented against real data. `netstat -s` used to say
per-protocol statistics were switched off in this build; that had stopped
being true — `nx_user.h` defines no `NX_DISABLE_*_INFO` — and it now prints
them.

`NAMES` resolves both halves: addresses through `ami_netdb_host_by_addr()` and
then a reverse lookup, ports through `ami_netdb_serv_by_port()`. `ALL` is a
modifier on the two socket categories rather than a category of its own, so
`ShowNetStatus ALL` is still the summary — which is what the installer's boot
check had been relying on it to be. `REPEAT` reprints every second after a
form feed until Ctrl-C.

One trap worth recording for anyone reading the ARP cache directly:
`nx_ip_arp_table[]` is a hash of **circular** lists — the last entry in a
bucket points back at the head, not at NULL — so a walk needs a head
comparison. Getting it wrong does not fail. It spins.

### 23.6 How it was exercised

One FS-UAE boot on SLIRP, 53 command lines through `ToolsSmoke`: every option
of every changed command in both spellings, the templates through ReadArgs'
own `?`, the failure cases (no argument, unknown name, unknown driver, wrong
unit, a bad name in either position of a list), and `REPEAT` started in the
background so it could be seen looping. The emulator is serialised across
workstreams by `build/.fsuae.lock`, which is the argument for one boot that
checks fifty things rather than fifty boots that check one.

## 24. The loopback window, sized from the packet pool (2026-07-26)

§16.5 found the one path in this stack that is limited by the advertised window
rather than by the machine: on loopback the sender held **100% of the window in
flight** and waited a **14.9 ms median** between segments with nothing it was
allowed to do. §16.6 measured what raising the window is worth and then did not
ship it, for a stated reason: a TCP socket's receive queue is packets off the
same `NX_PACKET` pool the SANA-II readers pin an eighth of, and 32 KB of window
times forty concurrent sockets is several times the whole pool. Exhausting it
drops frames stack-wide, which is a **functional** failure and not a slowdown —
so the trade on offer was a loopback gain against a stack that falls over at
forty sockets, and that is not a trade.

This section makes the window pool-derived, so the gain does not have to be paid
for. **Loopback goes from 230 to 283 KB/s (+23%), the wire from 152 to 159 KB/s,
and curl's own `http://` figure from 145 to 182 KB/s (+25%), with zero
retransmissions on either path and `d03_parallel_40` green.**

### 24.1 First: the case against a bigger window was measured before the fix that removes it

§16.6's warning is the reason nobody raised this window earlier, and it was
real. Before commit `78b4ed9`, RFC 1122's "acknowledge at least every second
full-sized segment" was absent, the ACK interval was therefore proportional to
the window, and 8 KB → 32 KB took the **wire** from 161 to 89 KB/s with
**zero retransmissions in both columns** — the sender waiting, nothing lost, and
no instrument in the tree able to tell those two apart.

`78b4ed9` has landed, so that measurement is stale, and everything below is
retaken with delayed ACK in. **The direction reverses completely**: with the
ACK rule present, raising the window costs the wire nothing and pays loopback
more than §16.5's headline. §16.5's "+18%" compared a 32 KB number that had the
ACK fix against a 297 KB/s baseline that did not; like for like it is +23%.

### 24.2 Why the answer cannot be a constant, and why the RX-depth pattern does not transfer unchanged

`AMI_SANA2_RX_DEPTH_IPV4` is the precedent, and it is worth being precise about
where the analogy stops. That fix could be settled from the pool alone because
the number of things drawing on the pool is **fixed and known when the stack
starts**: two reader threads, three with IPv6. One in eight of the pool, floor
4, ceiling 32, decided once, done.

Here the consumer count is neither known at start-up nor small. It is one socket
for a bulk transfer and **forty** for `tests/curl` `d03_parallel_40`, and a
single number chosen at stack start would have to assume the worst. Assuming
forty gives every socket `(256/8 × 1568) / 40 = 1254` bytes — below the floor,
so every socket back at 8192 and the entire gain thrown away.

**So the variable that matters is sockets, not interfaces, and the pool cannot
be the only input.**

### 24.3 What ships

`ami_bsd_tcp_window()` (`src/bsdsocket/socket.c`), called at all three
`nx_tcp_socket_create()` sites:

```
budget = (pool_total / 8) * pool_payload      /* 1 in 8 of the pool, in bytes */
window = clamp(budget / (live_tcp_sockets + 1), 8192, 32768)
```

**The pool sets a budget; the live socket count divides it.**

| sockets open | 1 | 2 | 3 | 4 | 5 | 6 | 7+ |
|---|---:|---:|---:|---:|---:|---:|---:|
| window, 8 MB profile | 32768 | 25088 | 16725 | 12544 | 10035 | 8362 | 8192 |

- **The floor is the status quo, deliberately.** 8192 is what every socket got
  before this function existed, and it is what §16.8 measured forty concurrent
  transfers passing on. No socket can come out of here with less than a
  configuration that is known to work, so the change has no downside case to
  argue about — only an upside case to bound.
- **The budget bounds the upside.** Only the first six sockets get more than the
  floor at all, and the excess over the floor summed across every socket comes
  to **56,370 bytes = 36 of 256 packets**, about 1.1× the budget. That is the
  same order as the eighth the SANA-II readers take, which is why the share here
  is also an eighth rather than a number picked to make loopback look good.
- **On the 4 MB / 68020 floor nothing changes at all.** The pool is
  `AMI_POOL_MIN_PACKETS` (16), an eighth of which is 3136 bytes — below the
  floor — so every socket gets 8192 exactly as before. Same outcome, and the
  same reason, as the RX depth's on that machine.

**Why at create time rather than continuously.** A window that tracked the
socket count would have to shrink an *established* socket's advertised window,
and NetX Duo has no supported way to do that: `nx_tcp_socket_rx_window_current`
is unsigned and derived from the default by subtraction, so lowering the default
under a socket with data queued underflows it, and retracting the right edge of
a window the peer is already filling is the one thing RFC 793 tells you not to
do. Create time is the only safe point, and it is sufficient, because the two
workloads that matter are distinguishable exactly there: a bulk transfer opens
one or two sockets, a concurrent client opens forty in a burst.

**The pool's free count was considered instead of the socket count and is
worse.** curl's multi interface opens all forty sockets *before* any of them
carries data, so `nx_packet_pool_available` is still at its maximum at the
moment every one of them would be sized. It would hand out forty large windows
and then discover the problem.

**The counter is NetX Duo's own.** `ip->nx_ip_tcp_created_sockets_count` is
maintained by `nx_tcp_socket_create/delete`, so there is none here to leak —
which matters, because a leaked one would pin every future socket at the floor
and look like nothing at all. It counts listeners and parked spares, neither of
which ever carries a connection; that over-counts consumers, which is the
direction to be wrong in.

`AMINETXDUO_TCP_WINDOW` now **pins** the window, floor and ceiling together,
rather than setting a floor, so a fixed window is still one `-D` out of one
tree — which is how every A/B below was taken. `getsockopt(SO_RCVBUF)` reports
what the socket actually got rather than the compile-time floor, because the
floor is now a number no particular socket necessarily has.

### 24.4 The traces

Two libraries out of one tree, differing only in `AMINETXDUO_TCP_WINDOW`; same
`NetTrace`, same toolchain, same 524,288-byte workload, `tests/trace/run-trace.sh`.
**Each arm was run twice and the two passes agree to the millisecond** — FS-UAE
measures the guest's own `GetSysTime()`, so these are emulated-time figures and
host contention does not enter them.

| A1200, 14 MHz, 524288 B | pinned 8192 | pool-derived | |
|---|---:|---:|---:|
| **loopback**, no capture | 230 KB/s | **283 KB/s** | **+23.1%** |
| loopback, capturing | 178 KB/s | 229 KB/s | +28.6% |
| **wire**, no capture | 152 KB/s | **159 KB/s** | +4.5% |
| wire, capturing | 119 KB/s | 127 KB/s | +6.5% |

And the mechanism, which is the point — a throughput number alone could not
distinguish any of this:

| loopback | pinned 8192 | pool-derived |
|---|---|---|
| advertised window | 8192 | **16725** |
| max bytes in flight | 4096 of 4096 — **100%** | 8192 of 8533 |
| **zero-window advertisements** | **64** in 128 segments | **0** |
| gap before a data segment, p50 | 22.4 ms | **17.8 ms** |
| ACK delay, p50 | 8.0 ms | 5.5 ms |
| **retransmitted** | **0** | **0** |

| wire | pinned 8192 | pool-derived |
|---|---|---|
| advertised window | 8192 | **32768** |
| peer bytes in flight | 2880 of 8192 (35%) | 2880 of 32768 (9%) |
| gap before a data segment, p90 | 23.7 ms | **6.6 ms** |
| **retransmitted** | **0** | **0** |

Three things are worth reading off these rather than only the throughput:

- **The zero windows §16.6 introduced are gone.** That section reported one
  thing getting worse when the ACK rule landed: two 4096-byte writes fill an
  8192-byte window exactly, so acknowledging on the second advertises zero, 64
  times in a 128-segment transfer. A 16,725-byte window is not an exact multiple
  of the application's write, and the count is zero.
- **The window did not have to reach 32 KB to collect the gain.** Loopback's
  receiver is the third socket created (listener, client, the socket `listen()`
  parks) so it gets 16,725 — half the ceiling — and lands on the same throughput
  a pinned 32 KB reached in §16.6 relative to its own control. What the path
  needed was a window larger than the delayed-ACK quantum of two application
  writes, not a large window.
- **The wire moved a little, and the trace says why it is not the window.** The
  peer still holds only 2880 bytes outstanding, 9% of what it is now offered, so
  the wire is not window-limited before or after — exactly as §16.5 said. What
  changed is the p90 gap between data segments, 23.7 → 6.6 ms, i.e. our own
  acknowledgements are no longer occasionally waiting on a window edge. A 4.5%
  wire gain from a change aimed at loopback is a small side effect and is
  reported as one rather than claimed.

**No retransmissions anywhere.** Four arms across two passes, both capture
points, `bs_drop` 0 on every channel. This was checked deliberately rather than
assumed: there is no SACK in the vendored tree, so a bigger window means more in
flight to lose, and the trace is the only thing that can tell a stall from a
loss. One earlier, uncontrolled run — taken before the arms were rebuilt against
one another — did show a single 1440-byte retransmission and a ten-deep
duplicate-ACK run on the wire; it did not reproduce in either controlled pass
and is recorded here rather than swept up.

### 24.5 The concurrency case, which is what this was guarded against

`tests/curl/run-curlverify.sh -p`, both builds, back to back on the same host,
every body hashed against the server's copy:

| `--parallel-max` | pinned 8192 | pool-derived |
|---:|---:|---:|
| 8 | 2.66 s | 3.38 s |
| 16 | 4.88 s | **3.42 s** |
| 24 | 5.58 s | 5.48 s |
| 32 | 6.94 s | 6.34 s |
| 40 | 7.96 s | **7.34 s** |
| 48 | 9.12 s | **7.30 s** |
| mean | 6.19 s | **5.54 s** |

**Nothing is lost at any point on either build, every body is byte-identical,
and `AvailMem` delta is +0 on both.** Five of six points are faster and one (the
eight-way) is slower; the mean is 10% better. The direction is what the
mechanism predicts — the sockets that get a window above the floor are the ones
opened while few others are — but the honest reading of a single sweep with this
spread is **no regression**, not a 10% win.

The named acceptance test and curl's own throughput, groups A–D on both builds:

| | pinned 8192 | pool-derived |
|---|---:|---:|
| `d03_parallel_40` | 7.02 s | **6.90 s** |
| `a04_get_1m2` — 1,200,000 B over `http://` | 8.06 s = **145 KB/s** | 6.44 s = **182 KB/s** |
| groups A–D | 112 passed, 1 failed | 112 passed, 1 failed |

The single failure is `a44_cookies_send` on both arms — the pre-existing one
§14.7 settled against a third-party binary, not ours. Note that curl's ~117 KB/s
in §11 is a **public-internet** fetch and is not this number; against the
hermetic peer on the same wire `NetTrace` measures 159 KB/s, curl 182 KB/s of
payload, and the two are not in conflict because curl's figure excludes the
request round trip that `NetTrace`'s includes.

### 24.6 The ceiling is 32768 and not 65535, and the reasons are structural

Both bounds §16.7 named still bind where this can go, and neither has moved:

- **No window scaling.** `NX_ENABLE_TCP_WINDOW_SCALING` is not defined, so 65535
  is a hard architectural cap; and because the option is bilateral, not offering
  it also stops the *peer* scaling.
- **No SACK.** Not implemented in the vendored tree at all, so a burst loss
  inside a larger window costs a full go-back-N. That is why the ceiling is the
  largest window that has been **measured** rather than the largest that is
  representable — and, per §24.4, loopback collects its whole gain at 16,725
  bytes, so the ceiling is not where the value is anyway.

### 24.7 Two guards NetX Duo ships for exactly this, and both are compiled out

This is the most useful thing found while sizing the window, and it is reported
rather than switched on.

`NX_ENABLE_LOW_WATERMARK` is not defined anywhere outside `third_party/`. With
it, two mechanisms that are currently dead code become live:

1. **`nx_packet_pool_low_watermark`.** When the pool falls below it, a TCP
   socket sets `nx_tcp_socket_rx_window_current = 0` and drops the **tail of its
   own receive queue** rather than the stack dropping frames, and
   `nx_tcp_socket_packet_process.c:137` reopens the window when the pool
   recovers. That converts pool exhaustion from a stack-wide functional failure
   into a per-socket stall — precisely the failure this section guards against
   by arithmetic instead.
2. **`nx_tcp_socket_receive_queue_maximum`.** `NX_TCP_MAXIMUM_RX_QUEUE` defaults
   to 20 and `nx_tcp_socket_create.c:136` only assigns it under the same
   `#ifdef`, so today a socket's receive queue has **no packet-count cap at
   all** — the advertised window is its only bound.

It is not switched on here because it is a second behaviour change, touching
IPv4 fragment reassembly and UDP receive as well as TCP, and because the
measurements above say the arithmetic guard holds. Three things would have to
land together rather than one, which is why it is separate work:

- `NX_ENABLE_LOW_WATERMARK` in `nx_user.h`;
- a call to `nx_packet_pool_low_watermark_set()` from `src/netstack/`, because
  `nx_packet_pool_create()` never touches the field and a zeroed watermark means
  the guard is compiled in but can never fire;
- `NX_TCP_MAXIMUM_RX_QUEUE` raised, because at 20 packets and 1440-byte wire
  segments it binds at about 28 KB — **before** a 32 KB window does — and the
  tail-drop it would then perform costs a retransmission this stack has no SACK
  to recover cheaply.

### 24.8 What was considered and rejected: giving loopback its own pool

The alternative the framing invites is that loopback should not draw on the same
pool at all. It is the wrong fix, for a reason that has nothing to do with how
hard it would be:

**the forty sockets that could exhaust the pool are wire sockets.**
`d03_parallel_40` is forty HTTP transfers over `eth0`. A separate loopback pool
would take memory permanently away from the path that is at risk in order to
protect the path that is not, and would leave the concurrency case exactly where
it started.

Two supporting facts, checked rather than assumed. There is one pool in the
whole stack — `netstack_pool()`, which is also the IP instance's default — and
`_nx_tcp_socket_send_internal()` allocates each segment from
`packet_ptr -> nx_packet_pool_owner` *before* anything knows the destination is
loopback; `_nx_ip_driver_packet_send()` only shortcuts into
`_nx_ip_packet_deferred_receive()` afterwards. So there is no point at which a
per-destination pool could be chosen without patching the vendored tree, and
nothing in `third_party/` is patched (§17.3).

### 24.9 Regression cover

| | |
|---|---|
| conformance, loopback tier | **130 passed, 0 failed, 12 skipped** |
| conformance, network tier | **141 passed, 1 failed, 0 skipped** |
| `tests/clients` | **94 checks, 0 failures** |
| `tests/curl` groups A–F | **147 passed, 2 failed, 149 cases** |
| `tests/curl` groups A–D, both builds | **112 passed, 1 failed, 113 cases** |
| `tests/curl` concurrency sweep, both builds | **9 passed, 0 failed**, `AvailMem` delta +0 |
| host `ctest` | **6/6** |
| `tools/ci.sh` | all green on macOS and on the Linux host with the pinned toolchain |

Both conformance tiers are unchanged from §17.4's numbers and the full curl
suite is unchanged from §16.8's, which is the result that matters: this touches
the size of a window and nothing about what the socket API does. The two curl
failures are the two §14 already names and neither is ours — `a44_cookies_send`
(curl does not write its cookie jar on AmigaOS, §14.7) and `f07_ftp_active`
(FS-UAE 3.2.35's SLIRP opens no inbound path, §12). `AvailMem` drops once, by
291 KB, when `tls.library` loads and is flat either side of it.

One measurement hazard is worth recording because it cost two runs. The absolute
throughputs here are **lower than §16's** — 230 KB/s on loopback at a pinned
8192 where §16.6 reports 287 — and the reason is not this change. `NetTrace` is
a command in `src/tools/`, that directory has been under active change, and the
instrument moved under the measurement. Both arms were therefore rebuilt from
one tree into private build directories and taken back to back; a comparison
against a number archived from an earlier tree would have been measuring the
tool. `build/` directory names are shared, so a measurement arm that points at
one another piece of work also builds is not a controlled arm.

## 25. `ping` rebooted the machine, and none of it was the network (2026-07-26)

`ping` was pulled from the distribution archive because it took the Amiga down.
§22.8 recorded where it stopped — the library opened, the raw socket opened,
`sendto()` returned all 64 bytes, `WaitSelect()` called the descriptor readable,
and the `recvfrom()` that followed never returned — and handed over two facts:
that `select()` and `bsd_raw_receive()` disagreed about the queue, and that the
read that followed killed the machine.

**Both facts were true observations and the conclusion drawn from them was
wrong.** The raw receive path was never involved. Neither was `select()`,
`recvfrom()`, `sendto()`, `bsdsocket.library`, or the network stack. The bug is
in the linked image, it is put there by the toolchain, and it is not confined to
`ping`.

### 25.1 What the bisection actually showed

The repro is `AddNetInterface eth0` followed by `ping 10.0.2.2 -c 3 -t 20`: six
boots for two commands. `-c 1` passes. `-c 3 -i 0` — three probes, three replies,
no interval — passes. So the fault is not in a probe, a reply or a read; it needs
the machine to still be running about a second after `ping` starts.

Instrumenting the receive path with `ami_log()` showed it working perfectly:

```
    rawtrace: queue copy 002935C4 len 84 sock 002FC334 count 0
    rawtrace: deq 002935C4 next 00000000 len 84 count 0
    rawtrace: recv_raw packet 002935C4
    rawtrace: source done
    rawtrace: length 84
    rawtrace: extracted 84
    rawtrace: release 002935C4
    rawtrace: released
    pingtrace: recvfrom returned 84
    pingtrace: matched, rtt
```

The reply arrives, is copied, dequeued, extracted, released, and matched. Then
`ping` waits out its one-second interval and the machine restarts.

From there each step removed something and the fault stayed:

| what was taken away | still reboots? |
|---|---|
| `Delay()`, replaced by a busy-wait of the same length | yes |
| the `select()`/`recvfrom()` loop, never entered | yes |
| `sendto()` | yes |
| the raw socket — `SOCK_DGRAM` instead | yes |
| the socket entirely | yes |
| `OpenLibrary("bsdsocket.library")` entirely | yes |
| `ami_millis()` / `timer.device` | yes |
| `AddNetInterface`, i.e. any network stack at all | yes |
| `ToolsSmoke`, run straight from the Startup-Sequence | yes (hang, not reset) |

What was left was a Shell command that parses arguments, prints a line, and
sleeps for a second. That still took the machine down. Enforcer reported **zero**
illegal accesses and MungWall **zero** wall hits, which rules out a wild pointer
and a heap overrun; the `struct Task` was intact, `tc_ExceptCode` and
`tc_TrapCode` were the ROM defaults, and the stack low-water mark never came
within 2,700 bytes of `tc_SPLower`.

It was also a Heisenbug: adding two `AMI_INFO()` calls in the right place made it
disappear. That is a code-layout dependency, and code layout is a property of the
build, not of the program.

### 25.2 The actual defect

`-O0` passes. `-O1` passes. `-Os` reboots. Narrowing by file, with everything
else at `-O1`, put it in `src/tools/tool_util.c` — and specifically in the one
thing `-Os` does there that `-O1` does not:

```c
BOOL tool_delay_ticks(ULONG ticks)
{
    while (ticks > 0) { ... }
    return tool_break();        /* -Os makes this a TAIL CALL */
}
```

`-ffunction-sections` puts `tool_break` in its own section, so the tail call is a
branch to another section of the same object, and the assembler emits a 32-bit
PC-relative relocation (`RELRELOC32`) against a **local section symbol**. In the
linked binary:

```
    5abc <_tool_delay_ticks>:
    5ac8:   4cdf 440c       moveml sp@+,d2-d3/a2/a6
    5acc:   60ff ffff ffae  bra.l   <-82>        -> 0x5a7c
```

`_tool_break` is at **0x5a88**. The branch lands on **0x5a7c**, twelve bytes
short — in the middle of `_tool_break_arm`, on the second word of

```
    5a7a:   223c 0000 1000  movel #4096,d1
```

so the machine resumes executing at an instruction boundary that does not exist,
with `a6` holding whatever the caller left there and `d1` about to be loaded with
`0x00001000`. **`d1=00001000` appears in every register dump the crash guard
took**, and so does a PC inside `_tool_break_arm` — a function that is called
exactly once, at the top of `main()`, and could not possibly have been running.
That contradiction was the tell, and it was on the screen for a long time before
it was read correctly.

The assembler leaves a non-zero addend in the displacement field and the linker
adds it a second time. Cross-**object** tail calls relocate against a global
symbol with a zero addend and come out right, which is why the 219 other 32-bit
PC-relative branches in `bsdsocket.library` — the `_nxe_*` error-checking
wrappers, which all end `return _nx_...()` — are correct and always have been.

Auditing every linked image found the same defect in four more places:

| image | branches | mis-resolved |
|---|---|---|
| `ping`, `AddNetInterface`, `Online`, `Offline`, `ShowNetStatus`, `nc`, `telnet` | 1 each | `tool_delay_ticks` → `tool_break` |
| `bsdsocket.library` | 223 | 4, all in `src/common/ami_random.c` |

`ping` was simply the command that reached the broken branch first: everything
else calls `tool_delay_ticks()` only on a path that a healthy machine skips.
**This was never a `ping` bug and `ping` was never the only thing exposed to it.**

### 25.3 The fix

`-fno-optimize-sibling-calls` wherever `-ffunction-sections` is used —
`src/tools`, `src/config`, `src/common`. The tail call is the only construct that
produces the broken relocation here: ordinary calls are absolute `jsr`, and
intra-section branches need no relocation at all. The two flags are now one
decision and the comment at each site says so; `-ffunction-sections` stays,
because the 27–38% it takes off a command (§21) is real and the sibling-call
optimisation is worth nothing next to it.

That is a workaround for a toolchain defect, so it is backed by a check rather
than by trust. `cmake/check-pcrel-branches.cmake` runs `POST_BUILD` on every
linked AmigaOS image, decodes each `bra.l`/`bsr.l`, and fails the build if the
target is not a function entry point. It has to run on the image: the object
file's relocation is well formed and the wrong address exists only after the
link, which is exactly why nothing caught this earlier. Run against the old
binary it says

```
  0x5acc: branch by -82 bytes lands inside a function, not on one
```

### 25.4 What the test now asserts

`tests/tools/run-livetools.sh` counted no boots, and that is why this was written
up as a hang twice. One boot starts the netstack once, so `netstack: starting
ThreadX` in the serial log is a boot counter and the only thing in the run that
can tell a reset from a block. The test now fails with **THE MACHINE REBOOTED**
if it sees more than one, and says in as many words that a transcript which looks
like a hang is not one. Checked both ways against real logs: the pre-fix run
(6 boots) fails, the post-fix run (1 boot) passes.

`ping` now answers, including through SLIRP's proxy, in one boot:

```
    ===== SYS:ping 10.0.2.2 -c 3 -t 20 =====
    PING 10.0.2.2 (10.0.2.2): 56 data bytes
    56 bytes from 10.0.2.2: icmp_seq=0 time=16 ms
    56 bytes from 10.0.2.2: icmp_seq=1 time=9 ms
    56 bytes from 10.0.2.2: icmp_seq=2 time=8 ms

    --- 10.0.2.2 ping statistics ---
    3 packets transmitted, 3 received, 0% packet loss
    round-trip min/avg/max = 8/11/16 ms

    ===== SYS:ping 8.8.8.8 -c 2 -t 20 =====
    PING 8.8.8.8 (8.8.8.8): 56 data bytes
    56 bytes from 8.8.8.8: icmp_seq=0 time=689 ms
    56 bytes from 8.8.8.8: icmp_seq=1 time=920 ms

    --- 8.8.8.8 ping statistics ---
    2 packets transmitted, 2 received, 0% packet loss
```

### 25.5 It is worse on the toolchain that builds the releases

macOS cannot execute the pinned `m68k-amigaos-gcc`, so everything above was
found with the local NDK and had to be confirmed against the toolchain CI
actually ships from. It was, on `turo@playhouse2`, and the pinned toolchain is
**not better — it is worse**:

| toolchain | mis-resolved branches in `ping` |
|---|---|
| local NDK (`~/amigaos/tools`, 15.2.0) | 1 — `tool_delay_ticks` → `tool_break` |
| pinned (`~/.cache/aminetxduo/toolchain/current`, 15.2.0) | **3** — the same one, plus two more |

So this was never a property of one developer's machine. The `ping` in the
v0.2.0 archive carried three of these, and the same audit found four in
`bsdsocket.library` from `ami_random.c` under both toolchains. After the fix,
all four cross configurations build clean on the pinned toolchain and every
linked image passes the check.

One thing about the check itself is worth recording, because it failed in the
way checks characteristically do. `cmake -P` starts a script with **no policies
set**, so `IN_LIST` is an `if` operator only under CMP0057 — which CMake 4.x
turns on by default and 3.31 does not. The check therefore passed on the machine
it was written on and failed every cross build on the Linux host with "Unknown
arguments specified": a script error wearing a build failure's clothes, which is
exactly the shape that gets a check disabled rather than fixed.
`cmake_minimum_required()` in the script settles it.

### 25.6 The lesson, which is not about ping

§22.8 handed over a measured trace and a diagnosis, and the diagnosis was an
inference: the trace stopped at `recvfrom`, therefore `recvfrom` blocked. The
trace stopped at `recvfrom` because the machine stopped. **`FIONBIO` was added
specifically to rule the blocking wait out, it changed nothing, and that result
was recorded and then not believed** — the search stayed inside the receive path
for another whole investigation on the strength of a story that its own control
experiment had already falsified.

The correction that mattered was cheap and was available from the beginning:
take away one thing at a time until the fault goes away, and be willing for the
answer to be that none of the suspects were involved. The stale account in
`src/tools/ping.c` has been rewritten in place rather than deleted, because the
next person to read a serial trace that stops mid-command needs to know that a
trace stopping is not evidence about the last line in it.


## 26. Six Roadshow commands, five of them written (2026-07-26)

§22 gave a Shell command a way to reach the running stack. This is what that made
writable: the part of Roadshow's command set we had never shipped. Roadshow 1.15 and
AmiTCP_NG both have all six; we had none of them, and the reason was never taste — a
command that cannot reach the stack cannot start one, stop one, or route through one.

**The provenance rule, restated because it is the whole basis for doing this at all.**
Roadshow's documentation was read for one thing: the argument templates. Those are
interface facts — a script written against `AddNetRoute VIA=…` has to keep working — and
matching them is the point. Nothing else was taken. Every description, every diagnostic
and every comment in `src/tools/` here is written from scratch, and AmiTCP_NG was not
opened at all: it is GPL, this project is MIT throughout, and that is one of its few
genuine advantages over both of them.

One name was deliberately **not** matched. Roadshow's checker is `CheckRoadshowConfig`;
ours is `CheckNetConfig`, because we are not Roadshow and a command name should not claim
to be.

### 26.1 What shipped, and what did not

| | template | state |
|---|---|---|
| `CheckNetConfig` | `QUIET/S,VERBOSE/S` | ships |
| `GetNetStatus` | `CHECK/K,QUIET/S` | ships |
| `NetShutdown` | `TIMEOUT/N,QUIET/S` | ships |
| `AddNetRoute` | `QUIET/S,DST=DESTINATION/K,HOSTDST=HOSTDESTINATION/K,NETDST=NETDESTINATION/K,VIA=GATEWAY/K,DEFAULT=DEFAULTGATEWAY/K` | ships |
| `DeleteNetRoute` | `QUIET/S,DST=DESTINATION/K,DEFAULT=DEFAULTGATEWAY/K` | ships |
| `RemoveNetInterface` | `INTERFACE/K,QUIET/S,FORCE/S` | **not written** |
| `ConfigureNetInterface` | `INTERFACE/A,…` | **not written** |

The last two are not deferred for time. They are blocked on capabilities the stack does
not have, and §26.5 says which — a command that took the arguments and could not act on
them would be exactly the class of defect §22 exists to stop.

### 26.2 `CheckNetConfig`, and the checks a parser cannot make

This is the one worth having. A wrong configuration does not announce itself: the stack
comes up, every field it prints is individually correct, and nothing works. The installer
only helps at install time, and after that nothing reads the files and says *line 3 names
a driver you do not have*.

`src/config` already reports bad syntax, unknown keywords, a missing `DEVICE` line and a
static interface with no `ADDRESS`, each with a file and a line number, through the
`AmiCfgReporter` hook that `tool_config_watch()` installs. All of that is **forwarded**
rather than reimplemented — `cnc_report()` is four lines and hands the parser's own
`AmiCfgProblem` straight to the same formatter everything else here uses. What the command
adds is the set of checks that need more than the line in front of them:

* the driver named is on this machine, and the **unit** named opens — asked of the
  hardware through `tool_device_probe()`, not guessed at;
* the netmask is a mask at all (a contiguous run of ones), and the address is a host on
  it rather than the network address or the broadcast address;
* the router is on a network one of the interfaces is on;
* no two interface files claim the same card, or the same address;
* a name server is reachable, either directly or through a default route that exists;
* the netdb files parse as the columns they are meant to be.

**It works with the network down**, which is the whole point: the machine that needs
checking is the one where the stack did not come up. It never opens `bsdsocket.library`.

Three decisions in it are worth recording because each was a bug first.

**The probe is skipped while the network is running.** The stack holds the card's driver
open, and a second `OpenDevice()` of a unit already in use fails — so probing would report
a working interface as broken on precisely the machine where it is demonstrably working.
Whether the card opens is answered by the network being up.

**The duplicate-card finding names the drawer, not a file.** The first version attributed
it to whichever of the two interfaces sorted later, which on the test configuration was
`eth0` — the *correct* one. Nothing here can tell which of two files is the mistake, so
both are named and neither is accused.

**One assertion in the test is the reverse of all the others.** `DEVS:Internet/name_resolution`
in the broken fixture names `8.8.8.8`, which is not on this machine's network — and is
reachable, because a default route exists. It must produce no finding. A checker that
fires on correct configuration gets ignored, and then it protects nothing; the same
reasoning `tests/tools/run-livetools.sh` already records for its forbidden-phrase list.

`DEVS:Internet/networks` is deliberately **not** checked for the same reason: its second
column may be written short (`10`, `192.168.1`), and a checker that flagged those would
fire on correct files.

### 26.3 `GetNetStatus`: the return code is the interface

`ShowNetStatus` prints a table for a person. This returns a number for a script, and that
is the entire difference. A startup script that has to wait for the network cannot parse
a table.

```
    0   every condition asked about is satisfied
    5   at least one is not          <- what IF WARN tests
   10   the command could not find out
```

The third is not decoration. A `bsdsocket.library` that is somebody else's cannot be
asked, and answering "not ready" would send the reader to fix a network that is fine.
That is a failure to find out, not a verdict.

Two of Roadshow's six conditions mean something specific here and are documented in the
command rather than left to be inferred. `PTPINTERFACES` is **never** satisfied: a
point-to-point interface is SLIP or PPP, and every interface this stack attaches is a
SANA-II Ethernet device with a hardware address, so the honest answer is "none" rather
than "none found". `ROUTES` is satisfied by the routes that exist rather than by a routing
table, because the directly-attached prefix of an interface is a real route.

It answers from the **same** `ToolSnapshot` that `ShowNetStatus` and `netstat` print from.
Three commands that disagreed about whether the network was up would be worse than having
only two.

### 26.4 `AddNetRoute` / `DeleteNetRoute`, and the netmask that is not in the template

Roadshow's templates have nowhere to write a netmask, so one has to be inferred, and the
rule is stated in the source rather than left to be discovered: `HOSTDESTINATION` is /32,
`NETDESTINATION` takes the mask covering the octets that are not zero, and `DESTINATION`
is whichever of the two the address looks like. A prefix length written into the address
(`10.1.2.0/24`) overrides all of it — that is a superset of `<IP>`, not a new keyword, so
the template is still Roadshow's.

`DeleteNetRoute` infers nothing. It reads the live table and deletes the entry the
destination falls in, with the netmask **that entry really has** — which is the only way
to implement a template with no netmask in it, and means a route added with any idea of
the mask can be removed by naming where it goes. It matches **static entries only**: a
directly-attached prefix is a real route that `netstat -r` prints, but it belongs to the
interface's address and `nx_ip_static_route_delete()` cannot remove it, so matching one
would turn *that is not yours to delete* into an unexplained failure from the stack.

**The bug the emulator caught, and no unit test would have.** `dest &= mask` sat at the
end of the shared parse block. On the add path `mask` is set by then; on the delete path
it is still zero, because the mask has not been read out of the table yet. So every
`DeleteNetRoute DESTINATION=192.168.77.0` asked for `0.0.0.0` and was told, correctly and
uselessly, that there was no route to it. The command compiled, linked, printed a
well-formed diagnostic and exited with a defensible code. Only running it against a route
that was demonstrably there found it.

**And the harness caught a second thing that was not in the commands at all.** The first
live run refused both route commands with *this stack has no routing table* — correctly,
because `NETSTATUS_SYS_ROUTING` was clear in the `bsdsocket.library` that had been staged.
`NX_ENABLE_IP_STATIC_ROUTING` had just been turned on in `port/netxduo-amiga/inc/nx_user.h`
and only `--target tools` had been rebuilt. The refusal is the design working: the flag is
read **before** anything is attempted, so a stale library produces a command that says why
rather than one that silently does nothing.

### 26.5 `NetShutdown` stops the traffic, and says what it cannot stop

Roadshow's own documentation for this command says, in its FUNCTION section, that it stops
all running interfaces. That is exactly what ours does, and the honest part is what it says
afterwards.

The stack is a singleton inside `bsdsocket.library`; it comes up on that library's first
`OpenLibrary()` and goes down when the last opener closes (`src/bsdsocket/library.c`).
`AddNetInterface` starts the network *by* opening the library and never closing it, and
that deliberately leaked reference is what keeps the interface up after the command exits.
No other command holds that reference, so no other command can drop it. The library stays
in memory with its ThreadX kernel running until a reboot.

What is stoppable is the traffic. Every interface goes down through
`NETCTRL_INTERFACE_DOWN` — the same call `Offline` makes — and afterwards nothing is sent
and nothing is received. The command says that in those words. A command that claimed to
have shut the stack down and left it running would be worse than one that explains the
distinction.

**`RemoveNetInterface` is blocked on this same seam and was not written.** Its documented
purpose is to make the stack forget an interface *so that it may be added again with
different parameters*. There is no detach: `include/aminetxduo/netstack.h` has
`netstack_interface_up/down` and nothing else, `NetStatusControl` has no operation for it,
and interfaces are read once at startup — `AddNetInterface` already tells a user who adds
a file to a running stack to reboot. The command would take its arguments and be unable to
do the one thing its name promises. It needs a `netstack_interface_detach()` and a
`NETCTRL_` selector to reach it, both outside `src/tools/`.

**`ConfigureNetInterface` was not written for a different reason.** The part of its
template this stack can act on — `ONLINE`, `OFFLINE`, `UP`, `DOWN` — is exactly what
`Online` and `Offline` already do, and the part that would justify a separate command
(`ADDRESS`, `NETMASK`, `CONFIGURE=DHCP`, `LEASE`, `ALIASADDR`) needs a control operation
that can change a live interface's addressing, which does not exist. What is left is a
command that mostly refuses.

### 26.6 What the tests assert, and why there are two of them

`tests/tools/run-livetools.sh` was extended rather than duplicated: it already boots once,
brings the stack up with `AddNetInterface eth0` and runs the shipped executables from a
staged directory. The new half checks each command on **what it printed and on what it
returned**, and requires the two to agree — a route command that printed a success line
while adding nothing, or a `GetNetStatus` that answered "not ready" because it could not
see in, would both pass a test written against exit status alone. So `AddNetRoute` is
followed by `netstat -r`, which reads the same table through a different command, and
`NetShutdown` is followed by the same `GetNetStatus CHECK=INTERFACES` that answered 0
before it and must answer 5 after.

`tests/tools/run-checkconfig.sh` is a second boot and needs to be, because its whole
subject is a **broken** `DEVS:`, which the live test cannot have and still be live. The
fixture in `tests/tools/checkconfig/devs` is wrong in seven ways a parser cannot see, and
right in one way that must not be complained about. The same boot runs the other four
commands with no stack at all, which is the state their argument handling has to survive.

Two boots, and the second earns its place: between them they cover the working machine and
the broken one, which are the two a user is ever on.

## 27. The rest of the DHCP lease, and the first time RFC 3927 ever ran (2026-07-26)

Every emulator run in this tree takes a DHCP lease. `DISCOVER`, `OFFER`, `REQUEST`, `ACK`
is therefore the best-tested path in the stack, and **nothing else in the lease's life had
ever executed once**: not renewal at T1, not rebinding at T2, not a lease running out, not
a NAK. FS-UAE's SLIRP grants a lease and never takes it away, so there was no way for any
of it to happen by accident.

Behind that, `nx_auto_ip_create/start/stop/delete` had been wired into `netstack.c` since
the interface work — two triggers, an explicit `CONFIGURE=AUTO` and a DHCP-timeout
fallback commented *"RFC 3927: fall back to a link-local address"* — with no test anywhere
and no `169.254` in the tree outside a config fixture. This project has a recent record of
wired-and-broken (§16's capture subsystem with 201 green checks and no caller; §22's three
commands that could never read the running stack), so "it is wired" was treated as no
evidence at all.

`tests/netstack/dhcp3927_test.c` and `tests/netstack/run-dhcp3927.sh` are what came of it.
Nine phases, and every claim below is from a run, not from reading the source.

### 27.1 A DHCP server on the host is not reachable, and here is the proof

The obvious rig is a Python DHCP server in the style of `tests/tools/netpeer.py`. It cannot
work, and the reason is not "SLIRP intercepts broadcasts" — that would leave unicast
renewals as a way in. **FS-UAE 3.2.35 embeds libslirp** (`slirp/src/bootp.c`,
`slirp/src/udp.c` and thirteen more are in the binary's string table) and it answers UDP
port 67 at *both* the addresses a DHCP client can ever send to.

From the A2065 frame dump of a real run — `tests/trace/a2065pcap.py` over the emulator's
own log, which is produced inside the emulated hardware and below every line of this stack:

```
  1 DHCP DISCOVER  0.0.0.0   -> 255.255.255.255  udp 68->67
  2 DHCP OFFER     10.0.2.2  -> 255.255.255.255  udp 67->68
  ...
  8 ARP  req sender=10.0.2.15 target=10.0.2.2
  9 ARP  rep sender=10.0.2.2  target=10.0.2.15
 10 DHCP REQUEST   10.0.2.15 -> 10.0.2.2         udp 68->67     <- the T1 renewal
 11 DHCP ACK       10.0.2.2  -> 255.255.255.255  udp 67->68
```

Frame 10 is the renewal RFC 2131 4.4.5 requires to be **unicast to the server**
(`nxd_dhcp_client.c:6377` picks `nx_dhcp_server_ip` for exactly this case), and frame 11 is
an answer to it. `lsof -nP -iUDP:67` on the host at the time: nothing. No process on the
host was listening on port 67, and the datagram was answered anyway — it never left the
emulator. Broadcast is eaten the same way, so both halves are closed.

There is no second way round it either. `otool -L` on the fs-uae binary shows no libpcap;
the only ethernet backends its string table offers are `slirp` and `SLIRP + Open ports
(21-23,80)`, and the one winpcap path in there is a Windows-only failure message
(`A2065: failed to initialize winpcap driver`).

**So the rig is three other things**, and each is chosen so the code under test never
knows the difference:

* **A clock that moves.** SLIRP's lease is 24 hours (`lease 4320000 s, T1 2160000 s`,
  measured off the live record — the fields are in ticks, `_nx_dhcp_extract_information()`
  multiplies the server's seconds by `NX_IP_PERIODIC_RATE` on the way in). The test writes
  `nx_dhcp_renewal_time`, `nx_dhcp_rebind_time` and `nx_dhcp_lease_time` in the live
  `NX_DHCP_INTERFACE_RECORD` and leaves everything else alone: the state machine, the
  packets and the server are real, only the countdown is not.
* **A wire that goes away.** `netstack_interface_down(0)` is what an unplugged cable looks
  like to the client, and it is the only way to stop SLIRP answering.
* **A peer that is not there.** SLIRP answers ARP inside 10.0.2.0/24 and nowhere else, so
  it can produce exactly ONE probe conflict before AutoIP redraws inside 169.254/16.
  Rate limiting needs ten in a row and defence needs a host that stays quiet through the
  probes and then claims the address anyway. Those two are synthesised: a correctly formed
  ARP frame handed to `_nx_arp_packet_deferred_receive()`, which is the same function the
  SANA-II reader calls for every ARP frame off the card (`sana2_rx.c:95`). Everything
  downstream of that — `nx_arp_packet_receive.c`'s conflict detection, the defence it
  sends, the AutoIP thread's response — is the real thing.

One new knob makes the fourth case possible. `AMINETXDUO_FSUAE_A2065` in
`tools/fsuae-run.sh` picks the card's backend, and `none` fits the A2065 and wires it to
nothing: the card opens, the driver links up, and no answer ever comes. That is the only
way to make a DHCP timeout happen with the link genuinely up, which is what the RFC 3927
fallback needs.

### 27.2 The lease lifecycle: it works, and it always did

| | verdict | evidence |
|---|---|---|
| renewal at T1 | **works** | `BOUND -> RENEWING -> BOUND`, acks 1 -> 2, address unchanged; frames 10/11 above |
| rebinding at T2 | **works** | `RENEWING -> REBINDING` with the wire down |
| the lease running out | **works** | `REBINDING -> INIT`, and `_nx_dhcp_interface_reinitialize()` takes the address AND the gateway off the interface |
| recovery afterwards | **works** | wire back, `SELECTING -> REQUESTING -> BOUND`, 10.0.2.15 again |
| a NAK | **works** | asked for 10.0.2.231, `DHCP NAK` from 10.0.2.2 on the wire, naks 0 -> 1, `REQUESTING -> INIT`, re-DISCOVER, lease recovered |
| **DECLINE** | **cannot happen** | see 27.5 |

**What did NOT work was telling anybody.** The DHCP client changes the interface address
from its own thread and announces nothing unless somebody registers for it, and nothing
did. A machine could go `BOUND -> RENEWING -> REBINDING -> INIT` at three in the morning,
lose its address and its gateway, and the only trace anywhere was that `netstat` started
answering differently. That is fixed in 27.4.

### 27.3 RFC 3927 had never run. It does now, and it is right

First execution ever, `CONFIGURE=DHCP` with `AMINETXDUO_FSUAE_A2065=none`:

```
[WARN] netstack: no DHCP server answered in 30 seconds
[INFO] netstack: RFC 3927 link-local configuration started
[INFO] netstack: address 169.254.44.97 mask 255.255.0.0
```

**The timing, sampled off the guest's own 50 Hz clock** on a clean cycle (`CONFIGURE=AUTO`,
nothing on the wire). This is the part that cannot be checked from counters:

| event | at | RFC 3927 |
|---|---|---|
| probe 1 | 360 ms | 2.2.1: random 0..PROBE_WAIT (1 s) |
| probe 2 | 1360 ms | +1.00 s, inside PROBE_MIN..PROBE_MAX (1..2 s) |
| probe 3 | 2360 ms | +1.00 s |
| address claimed | 4000 ms | after PROBE_NUM = 3 probes with no answer |
| announce 1 | 6000 ms | 2.4: +2.00 s = ANNOUNCE_WAIT |
| announce 2 | 8000 ms | +2.00 s = ANNOUNCE_INTERVAL, ANNOUNCE_NUM = 2 |

and the same sequence read off the wire rather than off the counters — three probes with a
zero sender, then two announcements with the sender filled in, which is precisely what
RFC 3927 2.2.1 and 2.4 ask for:

```
  1 ARP req sender=0.0.0.0        target=169.254.67.40
  2 ARP req sender=0.0.0.0        target=169.254.67.40
  3 ARP req sender=0.0.0.0        target=169.254.67.40
  4 ARP req sender=169.254.67.40  target=169.254.67.40
  5 ARP req sender=169.254.67.40  target=169.254.67.40
```

**Conflict at probe time** (SLIRP as the other host: probe 10.0.2.2, which it answers).
Frames 20/21 of the same capture are the probe and SLIRP's reply; the conflict is counted,
the candidate is thrown away and a fresh one is drawn from 169.254.1.0-169.254.254.255.

**Rate limiting.** Eleven conflicts in a row, one per candidate, using the synthetic peer.
Past `NX_AUTO_IP_MAX_CONFLICTS` (10) the next probe came **61 seconds** later against a
`NX_AUTO_IP_RATE_LIMIT_INTERVAL` of 60 — timed, not assumed, and reproduced at 60 s and
61 s on separate runs.

**Conflict after the address is in use.** Another host announces the address we are using:
`nx_ip_arp_requests_sent` goes up by one — that is the defence leaving on the wire, from
`nx_arp_packet_receive.c`'s `nx_interface_arp_defend_timeout` path — and
`nx_auto_ip_defend_count` goes up by one. See 27.5 for what happens next, which is where
this implementation stops being conformant.

**DHCP arriving later.** With the wire back, the still-running DHCP client takes a lease
and writes over the link-local address, which is what RFC 3927 1.9 wants. Nothing in the
stack arbitrated that before — it happened only because the DHCP client wrote last — and
nothing stopped the AutoIP module, which then sat holding an address the interface no
longer had. Fixed in 27.4.

### 27.4 Six defects in `src/netstack/`, and what each one cost

1. **Nothing was ever reported.** `nx_dhcp_interface_state_change_notify()` and
   `nx_ip_address_change_notify()` were both unregistered. `ami_ns_dhcp_state_changed()`
   and `ami_ns_address_changed()` now log every transition, and the one that matters says
   so plainly:

   ```
   [WARN] netstack: interface 0 has LOST its DHCP lease -- the address and the gateway
          have been taken off it, and every open connection through it is dead
   ```

   Both callbacks run on a NetX Duo thread. `AMI_INFO`/`AMI_WARN` are `RawPutChar()` and
   `RawDoFmt()`, which is Exec-only and legal from any Task, and neither blocks.

2. **Losing the lease did not start the RFC 3927 fallback.** `netstack.c` fell back to
   link-local when DHCP never answered at startup and did nothing at all when a lease was
   lost afterwards — the same machine, the same absence of a server, two different
   answers. The state-change callback now calls `ami_ns_start_autoip()` on
   `BOUND|RENEWING|REBINDING -> INIT`. Deliberately **not** on `REQUESTING -> INIT`: a NAK
   there is an ordinary part of acquiring a lease, and a first boot reporting a lost lease
   would be a false alarm on the one message that has to be believed.

3. **`nx_ip_status_check()` is interface 0 only.** It is one line at the bottom of
   `nx_ip_status_check.c` — `return(_nx_ip_interface_status_check(ip_ptr, 0, ...))` — and
   the startup wait was built on it. A machine whose Ethernet is static and whose second
   interface is the DHCP one waited out the full thirty seconds and then reported that
   nothing had an address. `ami_ns_wait_for_address()` polls every configured interface.

4. **The host name every AmiNetXDuo machine announced was `"amiga"`.** DHCP option 12 is
   what a router's client list shows and what many of them put in local DNS, and
   `nx_dhcp_create()` was passed a string literal, silently discarding the `HOSTNAME` the
   user configured. Two of these machines on one network were indistinguishable. It now
   passes `ns_Config.hostname` — NetX Duo keeps the pointer rather than a copy, so it has
   to be storage with the lifetime of the `NX_DHCP`, and `ns_Config` is inside the same
   `AmiNetStack`.

5. **Thirty seconds of nothing after thirty seconds of nothing.** The fallback path waited
   another `AMI_DHCP_TIMEOUT_TICKS` for AutoIP after the DHCP wait had already failed. The
   whole probe/announce sequence is eight seconds; `AMI_AUTOIP_TIMEOUT_TICKS` is fifteen,
   and failing it now says why:
   `link-local configuration did not settle either -- is the cable in?`

6. **AutoIP was left running once DHCP won.** Its thread waits indefinitely for a conflict
   and never re-reads the address, so it sat defending one the interface no longer had.
   `ami_ns_address_changed()` stops it when a routable address appears — never from the
   AutoIP thread itself, because `nx_auto_ip_stop()` is `tx_thread_suspend()` and calling
   it on the running thread would suspend it in the middle of its own announcement. Stop
   is a suspend, so 2 above can start it again:

   ```
   [INFO] netstack: link-local configuration stopped -- interface 0 has a routable address now
   ...
   [WARN] netstack: interface 0 has LOST its DHCP lease -- ...
   [INFO] netstack: RFC 3927 link-local configuration restarted
   ```

   The `ns_AutoIpRunning` guard is not decoration. `nx_auto_ip_start()` writes
   `nx_auto_ip_current_local_address` from the caller's thread with no synchronisation
   against the AutoIP thread reading it, and starting an instance that is already running
   loses a race: frame 25 of one capture is
   `ARP req sender=0.0.0.0 target=0.0.0.0` — a probe for the null address, sent because
   `start` cleared the candidate between the module deriving one and probing for it. The
   test provokes that on purpose; the stack now cannot.

### 27.5 Two things that are still wrong, and both are vendored

`third_party/netxduo` is consumed unmodified, so these are stated rather than patched.

**AutoIP gives the address up on the first late conflict.** RFC 3927 2.5 describes a host
that has already announced an address defending it once — a single ARP announcement, and
only if a *second* conflict arrives inside DEFEND_INTERVAL does it give the address up.
`nx_auto_ip.c:1083` says what it does instead, in its own words: *"No defense currently,
just clear the IP address once a late collision is detected and start over."* Measured:
the ARP layer underneath it does send the defensive announcement (`nx_ip_arp_requests_sent`
+1, from `nx_arp_packet_receive.c`'s `NX_ARP_DEFEND_INTERVAL` path), and then AutoIP zeroes
the interface and re-probes anyway. So the wire behaviour is half right and the outcome is
wrong: a single stray ARP from a misconfigured host costs this machine its address.

**`DHCPDECLINE` cannot be sent at all.** RFC 2131 4.4.1 says a client SHOULD ARP-probe the
address the server offered and DECLINE if something answers. NetX Duo compiles the whole of
that — the `ADDRESS_PROBING` state, `_nx_dhcp_ip_conflict()` and
`_nx_dhcp_interface_decline()` — behind `NX_DHCP_CLIENT_SEND_ARP_PROBE`, which this port
does not define, so **no DHCP probe and no DECLINE has ever been possible**.

It works when it is switched on. A measurement build with the define, answering the
client's own probe with the synthetic peer:

```
[dhcp] -> REQUESTING
[dhcp] -> ADDRESS_PROBING
  client is probing 10.0.2.15
[dhcp] -> INIT
  ok   the conflict sent the client back to INIT (a DECLINE)
  ok   the disputed address was not put on the interface
```

and frame 67 of that run's capture is `DHCP DECLINE 0.0.0.0 -> 255.255.255.255 udp 68->67`.

**It is left off, and the number is why.** Measured on the A1200 profile, ThreadX ticks
from `tx_amiga_kernel_start()` to `netstack_startup()` returning, same SLIRP, same lease:

| | ticks | seconds |
|---|---|---|
| as shipped | 56 | 1.12 |
| with `NX_DHCP_CLIENT_SEND_ARP_PROBE` | 250 | 5.00 |

Three ARP probes at `NX_DHCP_ARP_PROBE_MIN`..`MAX` apiece, on every bring-up, for a
conflict that a home network with one DHCP server cannot produce — **3.9 seconds added to
every boot**, and this stack is brought up and torn down by `bsdsocket.library` on the
first `OpenLibrary()`, so it is 3.9 seconds added to the first network command in a Shell
session as well. The line to change is one `#define` in
`port/netxduo-amiga/inc/nx_user.h`; `tests/netstack/dhcp3927_test.c` grows phase I as soon
as it is there.

### 27.6 What could not be tested here at all

* **A DHCP server that behaves differently from SLIRP** — a different lease length, options
  SLIRP does not send, a server that offers an address already in use. 27.1 is the
  evidence; it needs an emulator with a bridged or tap backend, or real hardware.
* **Two DHCP servers on one wire**, which is the case `SELECTING` exists to arbitrate. Same
  reason.
* **The AutoIP defend counter reaching a second conflict inside DEFEND_INTERVAL**, which is
  the branch RFC 3927 2.5 turns on — moot while the module abandons the address on the
  first one.

### 27.7 Running it

```
tests/netstack/run-dhcp3927.sh                       # lease lifecycle, B-H
tests/netstack/run-dhcp3927.sh -M killlink           # the fallback, then DHCP taking over
AMINETXDUO_FSUAE_A2065=none \
    tests/netstack/run-dhcp3927.sh -C AUTO           # RFC 3927 on its own, no server
```

The first two are the ones to keep an eye on -- **35 checks, 0 failures** and **40 checks,
0 failures** on the runs this section is written from. The third is situational: with no
server on the wire its DHCP phases have nothing to work with and say so rather than
failing.

## 27. tcpdrill: a packet-level conformance harness, and the three defects it found (2026-07-26)

Every time anyone in this project has looked at TCP packet by packet, they have
found a defect, and all four of them were found by accident: the delayed ACK
that was never implemented (§16.6), the four-frame SANA-II receive window that
TCP hid in retransmissions, `shutdown(SHUT_WR)` sending a RESET instead of a
FIN, and `bsd_readable()` calling a half-closed socket readable with nothing to
read. §16 built the instrument that shows what TCP is doing. This section
builds the one that says what it *should* be doing, and then disagrees with it.

**Result, on `b3b4b49`: 21 cases, 152 checks passed, 4 failed, and three
defects — one of which means that unacknowledged data is never retransmitted at
all.**

### 27.1 packetdrill cannot run here, and neither can any host-side injector

The technique is Google's packetdrill's: a script states both the socket calls
the application makes and the exact packets that must appear on the wire, in
order, with timing, and a failing case therefore reads as a specification of
what should have happened. Its published scripts were read as a description of
correct behaviour. None of its code, and none of its syntax, is used here.

**The tool itself is unusable and so is its architecture.** packetdrill is a
POSIX program that opens a TUN device and makes the system calls locally;
neither exists on AmigaOS, and porting it would be a larger job than the stack
under test. The obvious substitute — a host-side peer that injects raw frames
into the emulated wire — is not available either, and this is worth stating
with evidence because it is the first thing anyone will try:

- **FS-UAE's A2065 has three backends and no more.** `slirp`, `slirp_inbound`
  and `none` are the only values `uae_a2065` accepts on this build; there is no
  tap, no bridge, and libpcap is not linked (the only `pcap` strings in the
  3.2.35 binary are three Windows-only winpcap failure messages, §16.3).
- **SLIRP is a user-mode NAT that terminates TCP and re-originates it.** The
  guest's peer is SLIRP's own TCP stack, not anything on the host. A host peer
  never sees the guest's sequence numbers, never sees its flags, and cannot
  place a byte of its own choosing in the guest's receive path. That is why
  `tests/tools/netpeer.py` and `tests/curl/curlpeer.py` are stream peers, and
  no amount of work on them changes it.
- **A guest-side raw socket is not a way round it either.** `src/bsdsocket/
  raw.c` says so in its own header: NetX Duo's core has no `IP_HDRINCL`, so the
  source address of a raw send is always the outgoing interface's. An injected
  segment would therefore come *from* the machine it is aimed at, and the
  stack would answer the reply with a RESET to itself.

**So the peer goes below the stack instead of beside it.** `tests/tcpdrill/
tapdev.c` is an AmigaOS Exec device — `MakeLibrary()` plus `AddDevice()`, six
vectors, no segment list — that implements enough of SANA-II for `src/sana2/`
to open it, query it, configure it, take it online and run its reader threads
against it. `DEVS:NetInterfaces/tap0` names it, so the stack brings it up
through exactly the code that brings up `a2065.device`, with no build switch
and nothing in `src/` aware that it is being tested.

The ordering is the whole trick and it is the only fragile thing in the design:
`TcpDrill` installs the device **before** it opens `bsdsocket.library`, because
`OpenDevice()` searches ExecBase's device list before it searches `DEVS:`, and
the stack opens its interfaces when it starts.

What that buys:

| | |
|---|---|
| every transmitted frame | arrives complete, timestamped and in order, and is never lost — `CMD_WRITE` is a function call, not a wire |
| every received frame | is one the harness composed byte for byte, including sequence numbers it has no business knowing |
| the peer | is not a TCP implementation, so **nothing answers by accident** — which is what makes an RTO measurable |
| the emulator | is not involved: no `-n`, no `a2065.device`, no SLIRP, no host networking |

Two things are deliberately *not* bypassed. The buffer-management hooks are
real: `S2_CopyToBuff` and `S2_CopyFromBuff` are called for every byte in both
directions, so `src/sana2/sana2_copy.c` and the packet positioning in
`sana2_rx.c` are under test rather than around it. And raw framing is refused
with `S2ERR_NOT_SUPPORTED`, so `ami_sana2_probe_raw()` decides against it and
the stack runs the **cooked** path — the one `a2065.device` drives and the one
every measurement in this document was taken through.

One implementation note, because it compiles cleanly and corrupts every frame.
The two hooks are m68k register-convention functions (`a0` = to, `a1` = from,
`d0` = length), and calling them through a `register ... __asm()` typedef
**miscompiles** on this toolchain: GCC 15 loads the function pointer into `a0`
and then jumps through it, destroying the first argument. Found by
disassembling the call, not by running it. The call is written out as inline
asm.

### 27.2 The script language

A failing case has to read as the specification it violates, so the format is
one directive per line, verb first, and every sequence number in it is an
offset:

```
case c04_shutdown_wr_sends_fin
socket
connect
tx S seq=0
rx SA seq=0 ack=1 win=8192 mss=1460
tx A seq=1 ack=1
shutdown wr
tx FA seq=1 ack=1
rx A seq=1 ack=2
```

`tx` asserts on the next frame the stack sends; `rx` injects one; `notx MS`
asserts silence. **Our** sequence numbers count from our own ISN, which the
harness learns from the first SYN it sees; the peer's count from the ISN the
harness chose. So no script contains a number the stack picked, and nothing in
`tests/tcpdrill/scripts/` is a snapshot of current behaviour.

Two decisions are worth defending:

- **The socket under test is non-blocking, always.** packetdrill runs the
  application call on another thread so a blocking `connect()` can be
  interleaved with the packets that complete it. `bsdsocket.library` is
  per-task, so that would mean a second Process with its own library instance
  in every case; making the socket non-blocking and driving the packet engine
  from the one Process costs the blocking-call semantics and buys a harness
  with no concurrency of its own. The blocking paths are already covered by
  `tests/conformance` and `tests/clients`.
- **Timing is taken from the frame, not from the harness.** Every transmitted
  frame is stamped with `ReadEClock()` **inside the device's `BeginIO`** — the
  instant the stack handed it over — so `after=` and `within=` do not measure
  the harness's 20 ms poll interval. That mattered: `a02` asserts a 200 ms
  delayed-ACK timer and would be untestable through a 20 ms poll otherwise.

The window is asserted with `winmin`/`winmax` and never exactly. §24 makes it
pool-derived and divided by the live socket count, so an exact number would be
a test of how many sockets the previous case left behind.

### 27.3 What passes, and it is most of it

18 of 21 cases, 152 of 156 checks, on `b3b4b49` with the pinned configuration.
Recorded as a result rather than as an absence of news, because several of
these are the successors of defects this project has already paid for:

| | |
|---|---|
| three-way handshake, active and passive | ISN, MSS 1460 offered and echoed, ACK carries the peer's ISN + 1 |
| `shutdown(SHUT_WR)` | **FIN, not RESET** — the third of the four historical defects, asserted rather than assumed |
| peer half-close | our ACK of the FIN, `recv()` = 0, socket still writable, and a 50-byte segment sent afterwards |
| an idle established socket | **not readable**, and `recv()` = `EWOULDBLOCK` — the fourth historical defect, likewise |
| SYN to a closed port | `RST|ACK`, seq 0, ack 1 |
| data to a closed port | bare `RST` whose sequence number is the segment's ACK number |
| delayed ACK | 153–213 ms after a lone segment: the 200 ms timer, on the 50 Hz tick, exactly as §16.6 predicts |
| out-of-order | the hole holds the ACK at its left edge (dup ACK at 10 ms), the fill produces a cumulative ACK, and `recv()` returns both segments in order |
| duplicate segment | acknowledged twice, delivered once |
| urgent data | ACK covers the urgent byte, and `recv()` returns 4 bytes — the deliberate inline divergence §17 records, now asserted |
| RESET on an established connection | tears it down, **answers nothing**, `recv()` = 0 |
| zero window | `send()` correctly refused with `EWOULDBLOCK`, and a **one-byte probe follows 919 ms later** (RFC 1122 4.2.2.17) |
| advertised window | inside §24.3's floor and ceiling, 8192–32768 |
| TCP and IP checksums | verified on **every** frame the stack sent — 71 frames, no failures |

`tap: tx 71  rx delivered 33  rx no-reader 0  copy-failed 0  tx-overrun 0`.
Nothing was dropped in either direction, so no assertion in the run is standing
on a frame the harness lost.

### 27.4 Defect 1: unacknowledged data is never retransmitted

**This is the one that matters, and it is not in TCP.**

```
case x02_data_retransmission_backs_off
  ok   tx PA seq=1 ack=1 len=100 within=200   [+1ms]
  FAIL tx PA seq=1 ack=1 len=100 after=700 within=1500
       nothing was sent at all
  FAIL tx PA seq=1 ack=1 len=100 after=1500 within=3000
       nothing was sent at all
```

A 100-byte segment goes out on an established connection. The peer never
acknowledges it. **Nothing is ever sent again.** `x04` extends the observation:

```
case x04_eleven_seconds_of_silence
  ok   tx PA seq=1 ack=1 len=100 within=200   [+1ms]
  ok   notx 11000
```

Eleven seconds of total silence — no retransmission, and not even the RESET
that ten expired retries should produce. A green line, and it is the worst
result in the section.

**The mechanism, isolated in `x03` and confirmed on the wire:**

```
case x03_retransmission_waits_for_a_later_send
  ok   tx PA seq=1 ack=1 len=100 within=200   [+1ms]
  ok   notx 1500                                          <- still nothing
  ok   send 100
  ok   tx PA seq=101 ack=1 len=100 within=400   [+1ms]    <- the new data
  ok   tx PA seq=1 ack=1 len=100 within=1500   [+917ms]   <- and NOW the first
```

The retransmission timer was firing the whole time. The packet was not
eligible. `nx_tcp_socket_retransmit.c:200` walks the transmit queue with

```c
while (packet_ptr && (packet_ptr -> nx_packet_queue_next == (NX_PACKET *)NX_DRIVER_TX_DONE))
```

and `NX_DRIVER_TX_DONE` is written by `_nx_packet_transmit_release()`
(`nx_packet_transmit_release.c:98`) — i.e. **only a packet the driver has given
back can be retransmitted.** Every NetX Duo reference driver releases inside
the `NX_LINK_PACKET_SEND` handler. Ours cannot: a SANA-II `CMD_WRITE` is
asynchronous, so `src/sana2/sana2_tx.c` releases in `ami_sana2_tx_reap()`
instead — and reap has exactly three callers, all of them reactive:

| `sana2_tx.c:215, 228` | the **start of the next transmit**, and the spin when the ring is full |
| `sana2_driver.c:316` | `NX_LINK_GET_TX_COUNT` |
| `sana2_driver.c:362` | `NX_LINK_DEFERRED_PROCESSING`, which this shim never asks for — it calls `_nx_ip_packet_deferred_receive()` directly and never sets `NX_IP_DRIVER_DEFERRED_PROCESSING` |

So on a link that goes quiet, the last packets sent are never released and
never become retransmittable. The TX reply port is `PA_IGNORE` (`sana2_tx.c:41`
— deliberately, so any thread may post to it), which means the completion
signals nobody and there is no context in which reaping could happen on its
own.

**Why no instrument in this tree could have seen it.** §16.4 and §24.4 report
**zero retransmissions** across every trace ever taken here, in both
directions, on both paths, in both views — because nothing was ever lost on an
emulated wire or on loopback. A bulk transfer never triggers it either: the
next segment's `tx_send()` reaps the previous one, so under load the queue
drains and retransmission works. The failure is precisely the case that has no
"next segment": a request/response protocol whose single request segment is
lost. An HTTP GET, a DNS query over TCP, a TLS ClientHello. It hangs until the
application's own timeout, having put nothing back on the wire.

**Where the fix goes: `src/sana2/`, not `third_party/`.** Reaping needs a
context that runs when nothing is being sent. Not implemented here — this is
`src/`, and it is a lifecycle change to the transmit ring rather than a
one-liner.

### 27.5 Defect 2: the retransmission timer does not back off, and is not derived from anything

```
case x01_syn_retransmission_backs_off
  ok   tx S seq=0 within=200                   [+2ms]
  ok   tx S seq=0 after=700 within=1500        [+890ms]
  FAIL tx S seq=0 after=1500 within=3000
       gap too short, ms: wanted 1500, got 1002
```

SYN retransmissions arrive at a flat ~1 s. RFC 6298 §5.5 requires the RTO to be
doubled on every retransmission; §2 requires it to be computed from measured
round-trip times in the first place. Neither happens, and the source says why
in two lines:

- `nx_tcp_socket_retransmit.c:188` computes the next timeout as
  `timeout_rate << (timeout_retries * timeout_shift)`, and `timeout_shift` is
  `NX_TCP_RETRY_SHIFT`, which `nx_tcp.h:114` defaults to **0** and
  `port/netxduo-amiga/inc/nx_user.h` does not override. The shift is a no-op.
- `timeout_rate` is `_nx_tcp_transmit_timer_rate`, which `nx_tcp_enable.c:116`
  fixes at `NX_IP_PERIODIC_RATE / NX_TCP_TRANSMIT_TIMER_RATE` = 50 ticks =
  **exactly one second**, for every socket, on every path. There is no RTT
  estimator in the vendored tree at all.

So a lossy long-fat path retransmits ten times in ten seconds and then gives
up, and a lossy LAN waits a full second for a loss it could have recovered in
twenty milliseconds. Both directions of wrong from one constant.

`#define NX_TCP_RETRY_SHIFT 1` is the one-line half of this and it is **not**
obviously safe: with `NX_TCP_MAXIMUM_RETRIES` at 10 the last interval becomes
`50 << 10` ticks = 1024 s, where RFC 6298 §5.7 caps the RTO at 60 s. Raising
the shift therefore needs the retry count lowered with it. That file is another
workstream's, so this is reported rather than changed.

The same flat 1 s governs the zero-window probe, which `z01` measures at
919 ms. RFC 1122 4.2.2.17 asks for exponentially increasing probe intervals and
`nx_tcp_socket_retransmit.c:133` writes the code for it — through the same
`timeout_shift` of 0.

### 27.6 Defect 3: `CloseSocket()` sends a RESET, now with the packet

```
case c03_close_sends_fin
  ok   close
  FAIL tx FA seq=1 ack=1
       flags: wanted FA, got R
       observed  R seq=1 ack=0 win=32768 len=0
```

§12.3 listed this as "a risk that has not been reproduced"; §16.9 promoted it
to an observation, having seen `RST 1` at the end of every flow in every
capture. It is now an assertion with the packet next to it: a close on a
connection where everything is acknowledged and nothing is queued emits a bare
RESET, seq 1, no ACK flag, where RFC 793 §3.5 requires a FIN.

Two things make this worse than a style point. `shutdown(SHUT_WR)` on the same
connection, in `c04`, sends a correct `FIN|ACK` and takes the correct ACK back
— so **the stack contains a working orderly-close path and `close()` does not
use it**. And a RESET tells the peer to discard anything it has not yet handed
to its application, which is the same class of data loss as the
`shutdown(SHUT_WR)` defect that has already been fixed once here.

`src/bsdsocket/` is where that lives, and it is a semantic change (linger,
TIME_WAIT, how long `CloseSocket()` is allowed to take) rather than a swap of
one call for another, so it is reported here and not made.

### 27.7 Two behaviours that are not defects, recorded so they are not rediscovered

- **Every full-sized segment is acknowledged, not every second one.** `a01`
  originally asserted that the first of two 1460-byte segments went
  unacknowledged, and it fails: the first is acknowledged after 213 ms (the
  delayed-ACK timer) and the second after 30 ms. RFC 1122 4.2.3.2 is a floor
  — "at least every second" — so acknowledging both is conformant, and the
  script now says so. Worth writing down because §16.6's headline was that
  this rule was *missing*, and the natural next mistake is to assert the
  stricter reading of it.
- **The zero-window probe is `ACK`, not `PSH|ACK`.** One byte, sequence number
  at the right edge, no PSH. Correct, and `z01` originally expected PSH.

### 27.8 What it costs to run, and what it does not cover

One emulator boot for the whole script file, 21 cases, about 40 seconds of
which 11 are `x04` deliberately watching nothing happen. `tests/tcpdrill/
run-tcpdrill.sh` stages `bsdsocket.library`, the device drawer and the script,
runs `tools/fsuae-run.sh` **without** `-n`, and reads `DH0:tcpdrill.txt` back
off the host directory the guest wrote it to. Output is flushed line by line,
for the reason §16.9 gives: a diagnostic tool that loses its last twenty lines
when the machine has to be killed is not a diagnostic tool.

Not covered, and named rather than left to be discovered:

- **Simultaneous open and simultaneous close.** Both are scriptable in this
  format and neither is written.
- **TIME_WAIT.** Its duration and the handling of a new SYN arriving during it.
- **Anything that needs two sockets at once**, because the engine drives one
  socket under test.
- **Blocking-call semantics**, by the choice in §27.2.
- **Loopback.** The device is an interface; `lo0` does not go through it.
- **Congestion control.** Slow start and the cwnd after a loss are visible in
  this format — segment counts per RTT — and nothing here asserts on them.


## 28. Three macros AmiTCP_NG has and we did not, and what each was actually worth (2026-07-26)

Three capabilities this stack lacked were, in every case, code NetX Duo already
ships behind a `#define` nobody here had written. That is a pleasant kind of gap
and a dangerous one: the work looks like typing, so the temptation is to type it
and move on. **A `#define` that changes no packet is not a feature**, so each one
below was taken to the wire — two of them are now proven there, and the third is
switched off with the measurement that says why.

A fourth item, randomised initial sequence numbers, turned out to be a claim
about NetX Duo that is wrong in the direction nobody checks: it *does*
randomise. What it does badly is combine two draws with `|` instead of `+`, and
that is worth nine bits.

| | | |
|---|---|---|
| `NX_ENABLE_IP_STATIC_ROUTING` | **on** | the table is real, and a routed packet takes a next hop nothing else in the run ever names |
| `NX_DNS_CACHE_ENABLE` | **on** | six lookups, two queries on the wire |
| `NX_ENABLE_TCP_WINDOW_SCALING` | **off** | measured: the negotiated scale is structurally always zero, and turning it on removes NetX Duo's guard against a window this machine cannot carry |
| RFC 6528-style ISN | n/a | NetX Duo randomises; the bias in *how* is fixed without touching `third_party/` |

### 28.1 Static routing: the enable that `NX_IP_ROUTING_TABLE_SIZE` looked like

`port/netxduo-amiga/inc/nx_user.h` set `NX_IP_ROUTING_TABLE_SIZE 4`, with a
comment about Roadshow-era configurations, and did not set
`NX_ENABLE_IP_STATIC_ROUTING`. The first is inert without the second, and the
pair reads exactly as though routing were compiled in. It was not:

* `NX_IP` carries no `nx_ip_routing_table[]` and no
  `nx_ip_routing_table_entry_count` (`nx_api.h:2972`);
* `nx_ip_static_route_add()` and `..._delete()` are stubs returning
  `NX_NOT_SUPPORTED`;
* `_nx_ip_route_find()` skips the table lookup entirely
  (`nx_ip_route_find.c:150`), so the only next hops that exist are each
  interface's own prefix and the single default gateway.

One gateway is enough for a machine on one Ethernet, which is why nobody
noticed. It is not enough for a second interface reachable only through its own
next hop — the configuration `NX_MAX_PHYSICAL_INTERFACES 2` exists for — or for
a subnet behind a router that is not the default one. §22.5 and §26 both record
this as the thing blocking `AddNetRoute`; it is now on, and §26 covers the two
commands that became writable because of it.

**What the vendored implementation actually does, checked rather than assumed,
because three of these are surprising:**

* **The next hop must be on one of this machine's own subnets.** NetX Duo
  derives the outgoing interface from it and refuses the entry with
  `NX_IP_ADDRESS_ERROR` when nothing matches (`nx_ip_static_route_add.c:88`).
* **First match is longest prefix**, but only because `add` keeps the table
  sorted by netmask descending on insertion. `_nx_ip_route_find()` itself walks
  it in order and takes the first hit.
* **The table is consulted after the on-link check and before the gateway**, so
  a route can override the default for part of the address space — which is the
  whole reason to have one rather than a second name for the gateway.
* **Deleting from an EMPTY table returns `NX_SUCCESS`** (`nx_ip_static_route_delete.c:97`).
  Deleting an absent entry from a non-empty one returns failure. A test that
  checks "deleting a route that is not there fails" has to make sure the table
  is not empty, or it is testing nothing.

`NX_OVERFLOW` — the four-entry table full — now maps to `ENOBUFS` rather than
falling through to `EINVAL`, because with the table compiled in that outcome is
reachable by a user rather than only by a bug.

#### The reports had to change too, and they had drifted apart already

`netstat -r` and `ShowNetStatus ROUTES` did not read `NETSTATUS_ROUTES`. Each
**synthesised** a routing table from the interface list and the default gateway,
in its own copy of the loop. That was correct exactly while there was no routing
table, and stopped being correct the moment there was one: a route added by hand
would have been in the stack and in neither report.

Both now render the live table, through one function (`tool_print_routes()` in
`src/tools/tool_nx.c`) so they cannot disagree again, in the order
`_nx_ip_route_find()` matches: connected prefixes, then the static table longest
prefix first, then the gateway. `ShowNetStatus` keeps its `NAMES` behaviour by
passing its own address formatter in, rather than by keeping its own copy of the
renderer. Loopback is printed as a row and is not in the table, deliberately:
NetX Duo's loopback interface is not one of the `nx_ip_interface[]` slots and
`_nx_ip_driver_packet_send()` shortcuts 127/8 without consulting a route at all,
so the row is a true description of where the packet goes and its absence would
read as "there is no loopback".

#### On the wire

`tests/tools/routeprobe.c` adds a route through `NETCTRL_ROUTE_ADD` and sends
one datagram to a destination only that route can reach.
`tests/tools/run-routes.sh` runs it and reads the answer out of the emulated
A2065's own frame log, which is written inside the emulated hardware below every
line of this stack.

The experiment is built so that the answer cannot come from anywhere else:

| | |
|---|---|
| destination | `192.168.77.5` — on none of the guest's subnets |
| next hop | `10.0.2.99` — on the guest's subnet, so NetX Duo will accept it, and answered by nothing, because SLIRP is `10.0.2.2` and `10.0.2.3` |

*With* the route, `_nx_ip_route_find()` matches, the next hop becomes
`10.0.2.99`, and the stack has to resolve an address it has never seen.
*Without* it, the default gateway `10.0.2.2` is used, whose ARP entry the DHCP
exchange already resolved, so the frame goes straight out and there is **no ARP
at all**. So `ARP who-has 10.0.2.99` on the wire happens if and only if the
routing table was consulted.

```
  ok: NX_ENABLE_IP_STATIC_ROUTING is in the running stack
  ok: NETCTRL_ROUTE_ADD accepted 192.168.77.0/24 via 10.0.2.99
  ok: the route is in the 'with' listing and in neither of the other two
  ok: it is flagged S (added by hand) with 10.0.2.99 as its next hop
  ok: a next hop on no local subnet was refused
  ok: deleting a route that is not in a non-empty table failed
  ok: NETCTRL_ROUTE_DELETE removed the route
  ok: netstat -r printed the table from NETSTATUS_ROUTES
  ok: the wire shows 1 ARP request(s) for 10.0.2.99 -- the route was used
  ok: nothing for 192.168.77.5 went out via the default gateway
```

**And the negative control ran by accident, which is the best kind.** An earlier
pass of this test had a wrong constant in the probe — `0x0A020263` is 10.2.2.99,
not 10.0.2.99 — so every `ROUTE_ADD` was refused, correctly, for a next hop on
no local subnet. That run shows exactly what the paragraph above predicts for a
stack with no route: no ARP for anything, and **one packet for 192.168.77.5 out
through the default gateway**. The two runs differ in one 32-bit constant and
produce the two opposite wire behaviours.

The probe drives `NetStackControl()` rather than `AddNetRoute`, on purpose: what
is under test is the stack, and a test written against a command's ReadArgs
template fails whenever the template changes, which is the wrong thing to be
sensitive to.

### 28.2 Window scaling: measured, and left off

`NX_ENABLE_TCP_WINDOW_SCALING` exists in the vendored tree, §16.7 recorded its
absence as a documented limitation — "a hard 64 KB ceiling, and it is bilateral,
so it also caps the peer" — and §24.6 named it as one of the two things bounding
where the pool-derived window can go. It is a one-line change. **It is still off,
and this is the measurement that says why.**

Three arms, built back to back out of one tree, differing only in the flags
named; `tests/trace/run-trace.sh`, A1200 profile, 524,288 bytes per workload,
each workload run twice (once captured, once not) as always.

| | loopback, no capture | loopback, capturing | wire, no capture | wire, capturing |
|---|---:|---:|---:|---:|
| **A** no scaling (ships) | 351 KB/s | 309 KB/s | 164 KB/s* | **172 KB/s** |
| **B** scaling on | 351 KB/s | 309 KB/s | 164 KB/s | **172 KB/s** |
| **C** scaling on, window pinned 65536 | 352 KB/s | 309 KB/s | 31 KB/s | **31 KB/s** |

\* Arm A's uncaptured wire pass is quoted from arm B's re-run: arm A's own
uncaptured pass hit a host peer that had expired while the run queued for the
emulator lock, and 115 KB/s from a connection that had to be re-established is
not a measurement. The captured passes — the ones the traces come from — were
taken cleanly in both arms and are **identical to the millisecond**, 2963 ms
each.

**A and B agree to within run-to-run noise everywhere, and on loopback they do
not merely agree, they match segment for segment:**

| loopback | A, no scaling | B, scaling on |
|---|---|---|
| segments / bytes | 128 / 524288 | 128 / 524288 |
| advertised window, sender / receiver | 25088 / 16725 | 25088 / 16725 |
| max bytes in flight | 8192 of 8533 (96%) | 8192 of 8533 (96%) |
| gap before a data segment, p50 / p90 | 13.0 / 14.4 ms | 13.0 / 14.5 ms |
| ACK delay, p50 / max | 4.0 / 5.6 ms | 4.0 / 5.6 ms |
| retransmitted | 0 | 0 |
| bytes/s | 315,838 | 315,873 |

#### The option is on the wire, and the scale factor is zero

`tcpdump -r` on the guest's own capture, the outbound SYN in each arm:

```
A   10.0.2.15.62412 > 10.0.2.2.7440: Flags [S], win 32768,
        options [mss 1460,nop,nop,nop,eol]
B   10.0.2.15.58950 > 10.0.2.2.7440: Flags [S], win 32768,
        options [mss 1460,wscale 0,eol]
C   10.0.2.15.54758 > 10.0.2.2.7440: Flags [S], win 65535,
        options [mss 1460,wscale 1,eol]
```

Two things to read off that. First, **the option costs nothing**: NetX Duo
builds a fixed eight-byte option area, and the window scale replaces the
`nop,nop,nop,eol` padding that is otherwise there, so the SYN is 24 bytes in
both A and B and no other segment carries options at all. Second, **arm B's
scale factor is 0** — and that is not a property of this workload.

#### Why the scale is *structurally* always zero here

NetX Duo picks the smallest shift that brings the receive window under 65536
(`nx_tcp_packet_send_syn.c:292`). §24.3's `ami_bsd_tcp_window()` draws every
window from one eighth of the packet pool, and the pool is bounded by
`AMI_POOL_MAX_PACKETS` (256), so the **largest window any socket can ever be
offered is 256/8 × 1568 = 50,176 bytes** — and that is with the 32768 ceiling
removed entirely. 50,176 < 65,536, so the shift is zero on every socket this
stack can create.

The trace confirms the budget rather than taking it from the source: loopback's
two sockets advertise 25,088 and 16,725, which are 50,176/2 and 50,176/3, and
the wire's single socket advertises 32,768, which is the ceiling clamping
50,176/1.

The other half of §16.7's argument — that not offering the option also stops the
*peer* scaling — is true and does not help either. The peer's scale governs
**our send window**, and §16.5 measured the peer holding at most 2,880 bytes
outstanding against the window it is already offered. There is nothing there for
a larger send window to collect.

#### And turning it on removes a guard that is doing real work

`nxe_tcp_socket_create.c:170` rejects a window above 65535 with
`NX_OPTION_ERROR` while scaling is off, and accepts anything below 2^30 while it
is on. Arm C is what that permits: `AMINETXDUO_TCP_WINDOW=65536`, the smallest
value that makes the scale non-zero, and the arm that proves the mechanism works
end to end.

| wire, 524288 B | A/B (32768 window) | C (65536 window) |
|---|---:|---:|
| throughput | 172 KB/s | **31 KB/s** |
| **retransmitted** | **0 segments** | **15 segments, 21,600 bytes** |
| longest duplicate-ACK run | 0 | **9** |
| our ACK delay p90 / max | 2.1 / 2.1 ms | 68.8 / **266.2 ms** |
| longest gap before a data segment | 48.5 ms | **1343.9 ms** |
| our advertised window, min | 32768 | **13695** |
| peer bytes in flight | 2880 of 32768 (9%) | 27360 of 49695 (55%) |

A 5.5× regression, reproduced across two passes (15,856 / 15,767 ms, then
16,228 / 16,085 ms), with real loss where the shipped configuration has none.
There is no SACK in the vendored tree (§16.7), so a burst loss inside a larger
window costs a full go-back-N — §24.6 said that was why the ceiling is the
largest window that has been *measured*, and this is the measurement that would
have been taken if anyone had tried.

**So the trade is: 12 bytes per `NX_TCP_SOCKET` and a shift on every segment
sent, retransmitted and acknowledged, for a factor that is always zero, in
exchange for losing the compile-time check that stops a window this machine
cannot carry.** With scaling off, `AMINETXDUO_TCP_WINDOW=65536` fails at
`socket()`. With it on, it runs, and runs five times slower.

`AMINETXDUO_TCP_WINDOW_SCALING` is a CMake option, default OFF, for the same
reason `AMINETXDUO_TCP_WINDOW` and `AMINETXDUO_NET68K_CHECKSUM` are: the arms
above are two libraries out of one tree differing in one flag, and that is the
only way this question can be answered again when something changes. Two things
would change it: SACK, or a pool budget that can offer one socket more than
64 KB. Neither is close.

### 28.3 DNS caching: six lookups, two queries

`addons/dns` has had a cache all along. `nxd_dns.h` ships the define commented
out, nothing here uncommented it, and so **every lookup went to the wire** —
including the second lookup of a name resolved a moment earlier, which is what a
shell session, an FTP transfer and a redirect-following `fetch` all do.

Two changes, and both are needed: `NX_DNS_CACHE_ENABLE` in `nx_user.h` compiles
the code in, and a call to `nx_dns_cache_initialize()` from
`src/netstack/netstack_dns.c` gives it a buffer. Without the second the feature
is present and inert — `nx_dns_create()` leaves `nx_dns_cache` NULL and every
path checks for it — which is the same shape of trap as
`NX_IP_ROUTING_TABLE_SIZE` above.

**Sizing, and how the number was chosen.** The cache is one buffer with resource
records growing up from the bottom and the strings they name growing down from
the top. Measured on this toolchain rather than estimated: `sizeof(NX_DNS_RR)`
is **20 bytes**, and a string costs `((len & ~3) + 8)`, so a cached A record for
`www.example.com` is 20 + 20 = 40 bytes and two end pointers cost 8.

**2048 bytes ≈ fifty cached names.** Fifty against what? A shell session
resolves one host and then talks to it; `fetch` following redirects resolves two
or three; the whole `tests/curl` suite names one peer. The largest real consumer
is not forward lookups at all but the reverse ones `ShowNetStatus NAMES` and
`netstat` perform, one per peer address on screen, bounded by `TOOL_MAX_SOCK`
(32). Fifty covers both at once.

Being wrong is cheap in one direction only, which is why the number leans small:
too small costs a DNS query — exactly the behaviour being replaced, since NetX
Duo evicts the least recently used record rather than failing — while too large
costs resident memory on a 4 MB machine forever. For scale, 2048 bytes is 0.05%
of the floor target's RAM and a fifth of the **9,792-byte packet pool the DNS
client already carries inside the same `NX_DNS`** (10,112 bytes) for its own
queries. The buffer is inline in `AmiNetStack` rather than separately allocated:
same lifetime, and an allocation that can fail would need a "no cache" path for
no benefit.

Forward (A, AAAA) and reverse (PTR) lookups share it — everything funnels
through `_nx_dns_host_resource_data_by_name_get()` and
`_nx_dns_host_by_address_get_internal()`, and both consult the cache before
binding a socket. TTLs are the server's own, aged from `tx_time_get()` against
`NX_IP_PERIODIC_RATE`. `DEVS:Internet/hosts` still wins over all of it, because
`netstack_resolve()` consults the file first and never reaches NetX Duo for a
name that is in it — so a hosts entry cannot be shadowed by a cached answer.

#### On the wire

`tests/tools/run-dnscache.sh`: two names, three lookups each, **alternating**,
from six separate invocations of `host`. Six processes, one `NX_DNS`, because
`AddNetInterface`'s deliberately-leaked library reference keeps the stack up
between commands. Two names rather than one so that the count is the assertion
by itself — a stack that never queried and one that queried every time both
fail, and no separately built control arm is needed to tell them apart.

```
  DNS queries seen on the wire: 2
       IP 10.0.2.15.65013 > 10.0.2.3.53: 7600+ A? example.com. (29)
       IP 10.0.2.15.61697 > 10.0.2.3.53: 6669+ A? example.org. (29)
  ok: example.com was asked for exactly ONCE in three lookups
  ok: example.org was asked for exactly ONCE in three lookups
  ok: example.com/.org resolved on all three lookups, same answer every time
```

Six lookups, two queries. The "same answer every time" half is not decoration:
a cache that returned nothing, or something else, on the hit would show up as a
pass on the query count alone, because there would be no query either way.

The one external dependency is stated rather than hidden: SLIRP's server at
`10.0.2.3` forwards to the host's resolver, so this test needs the host to be
able to resolve two public names, and it fails rather than passing on an empty
wire if it cannot.

### 28.4 Initial sequence numbers: NetX Duo does randomise, and the bias is worth nine bits

The starting claim was that `nx_tcp_socket_connect.c` contains no randomisation.
It does — in `nxd_tcp_client_socket_connect.c:411` and
`nx_tcp_server_socket_accept.c:106`, both of which read:

```c
if (socket -> nx_tcp_socket_tx_sequence == 0)
{
    socket -> nx_tcp_socket_tx_sequence  = ((ULONG)NX_RAND()) << 16;
    socket -> nx_tcp_socket_tx_sequence |=  (ULONG)NX_RAND();
}
else
    socket -> nx_tcp_socket_tx_sequence += 0x10000 + (ULONG)NX_RAND();
```

`NX_RAND()` is `ami_random_rand()` on this port — a SHA-256 hash DRBG over an
entropy pool (`src/common/ami_random.c`, and `nx_port.h` says why) — so the
generator is not the problem.

**A first reading of this file missed the second line and concluded the ISN had
16 bits of entropy and always ended in four zero nibbles. That was wrong, and
the trace said so before anything was written**: the ISNs in the captures had
non-zero low halves. Recording it because the failure mode is the one this
document keeps finding — a confident claim about code from a grep rather than a
read, when an instrument that could settle it was already running.

**What is actually wrong is `|`.** It is a bitwise OR of two independent draws,
and `rand()` returns `0..0x7FFFFFFF`, so:

| bits | source | P(1) |
|---|---|---|
| 0–15 | second draw alone | 1/2 |
| 16–30 | first draw **OR** second draw | **3/4** |
| 31 | first draw's bit 15 alone (the second draw's bit 31 is always 0) | 1/2 |

Fifteen of thirty-two bits are three-quarters ones. That is **29.2 bits of
Shannon entropy**, and — the number that matters for guessing — a **min-entropy
of 23.2 bits**: the single likeliest ISN comes up **438 times** more often than
it would under a uniform distribution, and an attacker who tries the dense
upper-half values first faces 2^23 rather than 2^32.

**Confirmed in the captures rather than argued from the expression.** Counting
how often bits 16–30 are set across the SYNs in the traces above:

| | bits 16–30 set | predicted |
|---|---|---|
| before | 35 of 45 = **0.78** | 0.75 |
| after | 69 of 135 = **0.51** | 0.50 |

#### The fix, without patching `third_party/`

The `else` branch is a supported path rather than a fallback: it is what every
**reused** socket takes, and it **adds** where the other **ors**. Seeding
`nx_tcp_socket_tx_sequence` from `ami_random_ulong()` at create time makes a
fresh socket take the branch a reused one takes, and a full 32-bit seed plus
`0x10000 + NX_RAND()` is uniform over the whole space. Three call sites in
`src/bsdsocket/socket.c`, one DRBG draw per TCP socket at create time, no
vendored file changed, no symbol override and no `--wrap`. Zero is special-cased
back to 1 because it is the value that means "not seeded" to the code above.

#### What this is not

**It is not RFC 6528**, and the difference is not cosmetic. 6528 computes
`M + F(local addr, local port, remote addr, remote port, secret)`; the
four-tuple hash exists so that a new connection on a *recently used* four-tuple
gets an ISN above the old one, which is what makes TIME-WAIT recycling safe.
What ships here is a purely random ISN — RFC 793 / RFC 1948 against prediction,
and silent about recycling, which the 2 MSL timer already answers. 6528 proper
is not reachable from this seam in any case: NetX Duo picks `tx_sequence` inside
`connect()`, and at socket-create time there is no peer to hash. Doing it
properly would mean patching the vendored connect path, and prediction — which
is the attack — is fixed without that.

### 28.5 Regression cover

Everything §24.9 measured, re-measured on the shipping configuration.

| | §24.9 | here |
|---|---|---|
| conformance, loopback tier | 130 passed, 0 failed, 12 skipped | **130 passed, 0 failed, 12 skipped** |
| conformance, network tier | 141 passed, 1 failed, 0 skipped | **141 passed, 1 failed, 0 skipped** |
| `tests/clients` | 94 checks, 0 failures | **94 checks, 0 failures** |
| `tests/curl` groups A–F | 147 passed, 2 failed, 149 cases | **147 passed, 2 failed, 149 cases** |
| `tests/curl` concurrency sweep | 9 passed, 0 failed | **9 passed, 0 failed**, `AvailMem` delta +0 |
| `tests/tools/run-livetools.sh` | — | every functional check passes; see below |
| `tools/ci.sh` | all green | **all green** on macOS (host, four cross configs, conformance build) |

The two curl failures are the two §14 already names and neither is ours:
`a44_cookies_send` (curl does not write its cookie jar on AmigaOS, §14.7) and
`f07_ftp_active` (FS-UAE 3.2.35's SLIRP opens no inbound path, §12).

**`run-livetools.sh` reports FAILED on this host for a reason that is not the
stack and is worth recording so the next person does not chase it.** Its first
assertion counts `netstack: starting ThreadX` in the serial log to tell a reboot
from a hang (§25), and FS-UAE writes **nothing at all** to the serial port on
this machine — `build/serial-*.log` is zero bytes for every run in this section,
including runs that demonstrably worked. Every functional assertion in that
script passes. `tests/tools/run-routes.sh` counts the first command's banner in
the transcript instead, which works here because `ToolsSmoke` reopens
`DH0:tools.txt` from the top after a reset; the same fallback would make
`run-livetools.sh` green again on this host.

### 28.6 What is still not there

* **SACK.** Not implemented in the vendored tree at all, and it is now the thing
  standing between §24's pool-derived window and anything larger — arm C above
  is what a bigger window costs without it.
* **RFC 6528 proper**, for the TIME-WAIT recycling property rather than for
  prediction. It needs the four-tuple, which does not exist where the ISN is
  chosen, so it means patching `third_party/`.
* **`NX_ENABLE_LOW_WATERMARK`**, still. §24.7 found it and reported it rather
  than switching it on; nothing here changed that argument.

## 30. mDNS: the machine gets a name, and SLIRP turns out to pass multicast (2026-07-26)

§27 gave a machine with no DHCP server a working address. It did not give it a *name*:
169.254.x.y is drawn at random, changes on the next boot, and there is by definition no
DHCP server whose client list or local zone could hold it. RFC 6762 is the piece that
closes that, and `third_party/netxduo/addons/mdns` had been vendored and unused since the
submodule went in.

It is now built, wired into `netstack.c` alongside DHCP, DNS and AutoIP, and behind
`AMINETXDUO_MDNS` (ON, like `AMINETXDUO_BPF`). Everything below is from a run.

### 30.1 The decisions, and the reasoning for each

**What it announces: `<HOSTNAME>.local`, and nothing else.** §27.4 fixed
`nx_dhcp_create()` being passed the string literal `"amiga"` instead of the configured
`HOSTNAME`; inventing a second name source here would have re-created that bug in a
different place. `src/config/config_file.c` already resolves one name through four
fallbacks (`hostname` in `DEVS:Internet/name_resolution`, then `ENV:HOSTNAME`, then the
first non-loopback entry in `DEVS:Internet/hosts`, then `"amiga"`), and this uses whatever
that produced.

With one transformation, which the run exercises deliberately: **mDNS wants one DNS label
and `HOSTNAME` may not be one.** The hosts-file fallback in particular finds fully
qualified names, because that is what a hosts file conventionally holds. Handing
`amigatest.home.lan` to `nx_mdns_create()` would claim `amigatest.home.lan.local`, which
nothing will ever ask for. `ami_ns_mdns_label()` takes everything up to the first dot and
changes nothing else — in particular it does **not** lowercase, because mDNS comparison is
case-insensitive (RFC 6762 §16) and a log that disagreed with the configuration file would
be worse than a mixed-case label.

**No services are advertised, and that is a decision rather than an omission.**
AmiNetXDuo ships `fetch`, `ftp`, `telnet`, `tftp`, `nc`, `sntp` and `whois`, and every one
of them is a *client*. There is no FTP server and no telnet server on this machine, so a
`_ftp._tcp` or `_telnet._tcp` record would advertise something that is not there, and a
browser that believed it would hang on a connection nothing will accept. `src/tools/tftp.c`
already settles the general question for this tree — *"a mode that is announced and not
honoured is worse than one that is absent"* — and the same applies to a service record.
`nx_mdns_service_add()` is one call, and `netstack_mdns.c` says where it goes on the day a
server exists. (§30.7 is about that day.)

**Name collisions: the module renames, we report.** RFC 6762 §9 says probe three times and
pick another name on a conflict, and the vendored module does exactly that —
`NX_MDNS_CONFLICT_COUNT` is set to 4 here rather than the default 8, because past
`amiga (4)` the answer is to set `HOSTNAME`, not to keep counting.

**One wart, recorded rather than patched.** The vendored renamer appends the *RFC 6763
service-instance* suffix: `amiga` becomes `amiga (2)`. For a service instance that is
correct and is what Bonjour shows in a browser. For a **host** name it is not — RFC 6762
§9's own example is `PrinterOne-2.local.`, and a host label containing a space and
parentheses is one nobody will successfully type at a shell. `_nx_mdns_conflict_process()`
is `static` in `nxd_mdns.c`, so neither a symbol override nor `-Wl,--wrap` can reach it
(§13.2), and this project does not patch vendored source. What is done instead is the
useful half: `ami_ns_mdns_probing()` says loudly which name was actually claimed and what
to do about it, and `netstack_mdns_hostname()` returns the **claimed** name rather than the
configured one so that anything displaying it shows what the network will answer to.

**IPv6 is deliberately not enabled in the module** even in the `AMINETXDUO_IPV6` build.
`NX_MDNS_ENABLE_IPV6` wants MLD group membership this stack does not run, and the point of
the module here is that a machine is reachable by name over the network it actually has.

**`NX_MDNS_ENABLE_ADDRESS_CHECK` is left off**, which is the vendored default. It is RFC
6762 §11's source-address check, and it compares an incoming packet's source against the
receiving interface's subnet. On a link-local machine that subnet is a /16 drawn at
random, and the check would reject exactly the case §27 exists to serve.

### 30.2 `.local` is resolved in the resolver, not in a new command

The obvious shape for "let a user see it work" is a new command. It would have been the
wrong one. Every name any AmigaOS program looks up arrives at `netstack_resolve()` —
`gethostbyname()` and `getaddrinfo()` in `src/bsdsocket/` both route through it — so a
seven-line branch there gives `.local` to the whole command set at once, and to somebody
else's program written for Roadshow. No command was modified.

The branch is **exclusive**, and RFC 6762 §6.7 is explicit about why: a name ending in
`.local` goes to 224.0.0.251 and never to a unicast server. It is not a matter of taste —
a great many home routers answer any name at all with their own NXDOMAIN-substitute page,
and a few forward `.local` upstream where somebody else's server answers. So no mDNS answer
means the name does not exist, which is the truth, and asking the unicast servers
afterwards could only produce a wrong one.

`DEVS:Internet/hosts` still wins over both, deliberately: a name pinned there is an
instruction from the machine's owner and outranks anything the network claims.

### 30.3 The vendored add-on does not compile on a big-endian port

`third_party/netxduo/addons/mdns/nxd_mdns.c:8489`:

```c
    *(USHORT *)(packet_ptr -> nx_packet_prepend_ptr + NX_MDNS_FLAGS_OFFSET)
        |= NX_CHANGE_USHORT_ENDIAN(tc_bit);
```

Every little-endian port defines that macro as `a = ((a >> 8) | (a << 8))` — an assignment
*expression*, legal in statement position and in the middle of a larger one. Every
big-endian port defines it as nothing at all, which is legal only in the first. So the line
expands to `x |= ;` and the file will not compile. It is the **only** expression use in the
entire vendored tree: six others exist, all in `nx_icmpv6_*`, all in statement position. As
far as this project can tell, nobody has ever built the mDNS add-on on a big-endian
machine.

Fixed in `port/netxduo-amiga/inc/nx_port.h` rather than in `third_party/`, and it is a
correction rather than a workaround — `(a)` is the exact big-endian analogue of what the
little-endian definition evaluates to:

```c
    #define NX_CHANGE_ULONG_ENDIAN(a)   (a)
    #define NX_CHANGE_USHORT_ENDIAN(a)  (a)
```

The cost is a `-Wunused-value` on the six statement uses, in files `cmake/ci-warnings.cmake`
exempts from `-Wall` anyway.

### 30.4 One lookup, not two: `ipv6_address` is a second serial query

`nx_mdns_host_address_get()` takes an `ipv4_address` and an `ipv6_address`, and a non-NULL
second pointer means "also ask for AAAA" — **serially**, with its own full timeout after
the A query's. The first version of `netstack_mdns.c` passed a buffer it then ignored. On
the wire that doubled the traffic of every successful lookup, and `host nosuchbox.local
TIMEOUT 5` spent fifteen seconds failing rather than five. `NX_NULL` there is the whole fix
and the run asserts it, by counting `AAAA (QM)?` frames and requiring zero.

### 30.5 What the run proves, and the SLIRP finding

`tests/tools/run-mdns.sh`, A1200 + A2065 on SLIRP, one boot. The guest's transcript:

```
    ===== SYS:host amigatest.local =====
    amigatest.local has address 10.0.2.15
    ===== SYS:host AMIGATEST.LOCAL =====
    AMIGATEST.LOCAL has address 10.0.2.15
    ===== SYS:host amigatest.local. =====
    amigatest.local. has address 10.0.2.15
    ===== SYS:ping -c 2 -t 5 amigatest.local =====
    PING amigatest.local (10.0.2.15): 56 data bytes
    56 bytes from 10.0.2.15: icmp_seq=0 time=10 ms
    ===== SYS:host nosuchbox.local TIMEOUT 5 =====
    host: cannot resolve "nosuchbox.local"
    ===== SYS:host example.com =====
    example.com has address 104.20.23.154
```

`hostname amigatest.home.lan` was in `DEVS:Internet/name_resolution`, so `amigatest` is the
derived label and not a copy of anything. `ping` was not modified and resolves through the
same path. `example.com` is the control: the `.local` branch sits in front of the unicast
resolver and does not break it.

**A query for the machine's own name is answered from the module's local cache without a
packet** (`_nx_mdns_query_check()` scans the local cache before the peer one), which is
what makes this half testable inside an emulator whose network is a NAT.

And what actually left the machine, from the emulated A2065's own frame log — below every
line of our code (§16.3) — read with `tcpdump`:

```
  00:80:10:32:33:34 > 01:00:5e:00:00:fb, IPv4, length 106:
      10.0.2.15.5353 > 224.0.0.251.5353: 0 [1n] ANY (QM)? amigatest.local. (64)
  00:80:10:32:33:34 > 01:00:5e:00:00:fb, IPv4, length 106:
      10.0.2.15.5353 > 224.0.0.251.5353: 0 [1n] ANY (QM)? amigatest.local. (64)
  00:80:10:32:33:34 > 01:00:5e:00:00:fb, IPv4, length 106:
      10.0.2.15.5353 > 224.0.0.251.5353: 0 [1n] ANY (QM)? amigatest.local. (64)
  00:80:10:32:33:34 > 01:00:5e:00:00:fb, IPv4, length 132:
      10.0.2.15.5353 > 224.0.0.251.5353: 0*- [0q] 2/0/0
          (Cache flush) A 10.0.2.15, (Cache flush) NSEC (90)
```

Three probes — an ANY question with the proposed record in the authority section, RFC 6762
§8.1 — then the announcements, unsolicited responses with the cache-flush bit set, §10.2.
`tcpdump` decodes them as mDNS without being told to. Wireshark opens the same file.

#### FS-UAE's SLIRP **does** relay outbound multicast, and that was not expected

This is the finding, and it contradicts the prior this section started with. §27.1
established that SLIRP eats DHCP broadcast entirely, and §20.2 that it is a NAT rather than
a router; the reasonable expectation was that 224.0.0.251 would go nowhere.

`tests/tools/mdnswatch.py` sat on the **host's real LAN** for the whole run, joined to
224.0.0.251:5353, calibrated by sending one query of its own (id `0x4d44`) and requiring to
see it come back. What it recorded:

```
    [96210.47] 192.168.1.193:5353   query  ... mdnswatch.local(type 1)   <- calibration
    [96342.36] 192.168.1.193:58517  query  ... amigatest.local(type 28)  <- THE GUEST
    [96372.09] 192.168.1.191:5353   query  ... amigatest.local(type 1)   <- a real machine
    [96396.91] 192.168.1.193:58517  query  ... nosuchbox.local(type 1)
    [96427.19] 192.168.1.193:58517  query  ... mdnspeer.local(type 1)
```

`192.168.1.193` is the *host's* address. The guest's mDNS crossed SLIRP, was NAT'd, and
arrived on a real home network — where `192.168.1.191`, a Windows machine that had never
heard of any of this, queried for `amigatest.local` in response to hearing it announced.

**Outbound works, with the source port rewritten.** The guest sends from
`10.0.2.15:5353`; it arrives from `192.168.1.193:58517`. The NAT allocated an ephemeral
port, as a NAT does for any UDP flow.

#### The inbound direction fails, and the reason is conformance working correctly

This took a second run to establish and the first answer was wrong. `mdnswatch.py` was made
to answer `mdnspeer.local` by **both** multicast **and** unicast straight back to
`192.168.1.193:58517` — the latter being what RFC 6762 §6.7 requires of any responder facing
a query whose source port is not 5353, which every query crossing this NAT looks like. The
guest did not resolve the name. The obvious reading is "the NAT has no return path".

It is not that. The A2065's own frame log shows the reply **arriving**:

```
  52:55:0a:00:02:02 > 00:80:10:32:33:34, IPv4, length 84:
      192.168.1.193.5353 > 10.0.2.15.5353: 0*- [0q] 1/0/0
          (Cache flush) A 10.0.2.2 (42)                        x5
```

Five of them, decoded, well formed, at the card. **The module rejected them, and it was
right to.** `_nx_mdns_packet_address_check()` implements RFC 6762 §11: a *unicast* mDNS
response (destination not 224.0.0.251) is accepted only if the source address is on the
receiving interface's subnet.

```c
    if ((des_address.nxd_ip_address.v4 != NX_MDNS_IPV4_MULTICAST_ADDRESS) &&
        (iface -> nx_interface_ip_address & iface -> nx_interface_ip_network_mask) !=
        (src_address.nxd_ip_address.v4   & iface -> nx_interface_ip_network_mask))
        return (NX_MDNS_NOT_LOCAL_LINK);
```

The guest is `10.0.2.15/24`. **SLIRP passes the host's real LAN address through unchanged**
as the source — `192.168.1.193`, which is off-link by any reading — while rewriting only
the destination. So the packet genuinely arrives from off-link as far as the guest can
tell, and §11 says drop it. That check is unconditional in the vendored source; it is not
behind `NX_MDNS_ENABLE_ADDRESS_CHECK`, which guards something else.

So the precise statement is:

* **The responder half is provable end to end under this emulator, and was proved.**
* **The querier half against a real peer is not, and the obstruction is the emulator
  forging an off-link source address** — not the NAT's return path, which works, and not
  the stack, which is doing exactly what the RFC requires. On a bridged backend or on real
  hardware the peer is on link and the check passes.

Everything the querier does that needs no peer is exercised: the `.local` branch, the
local-cache answer, one query rather than two, and a clean bounded failure on a name nobody
owns. §27.6 reached the same conclusion about a second DHCP server, for a related reason.

One consequence worth stating plainly, because it surprises: **a guest running this stack
under FS-UAE announces itself onto the developer's real home network.** It did here.

### 30.6 The vendored `ftp`, `telnet` and `tftp` add-ons: no, on the SNTP ground

Asked whether the vendored client add-ons should replace the hand-written commands in
`src/tools/`, on the two conditions that they be substantially more complete *and* reduce
maintenance burden. The answer is no on all three, and the comparison never gets as far as
completeness because §19.6's wall is in the way.

**A Shell command links its own copy of ThreadX and NetX Duo whose kernel never runs.**
Every one of these add-ons is created against an `NX_IP`:

```
    nx_ftp_client_create   (client, name, NX_IP *ip_ptr, window, NX_PACKET_POOL *pool)
    nx_telnet_client_create(client, name, NX_IP *ip_ptr, window)
    nx_tftp_client_create  (client, name, NX_IP *ip_ptr, NX_PACKET_POOL *pool)
```

In a shipped tool `netstack_ip()` and `netstack_pool()` are the weak stubs in
`src/tools/netstack_weak.c` and return NULL — check any tool's `link.txt`. And even given
a valid pointer, `nx_tcp_socket_create()`, `nx_tcp_socket_receive()`,
`nx_udp_socket_bind()` and `nx_tcp_client_socket_connect()` all suspend the calling thread
through scheduler state belonging to a kernel that was never entered. That is precisely why
`sntp` was written against `bsdsocket.library` instead. Nothing about FTP, TELNET or TFTP
changes it.

Two further facts settle it even if that wall were removed:

* **The FTP and TFTP *servers* need FileX** — `nxd_ftp_server.h` and `nxd_tftp_server.h`
  both `#include "fx_api.h"`, and the sources carry 116 and 47 `fx_` references. This
  machine has AmigaDOS. Already recorded in §5.4 and in `src/tools/tftp.c`'s own header.
* **A server is not a substitute for a client** in any case, and the client halves are
  thinner than what is already shipped. `src/tools/ftp.c` has twenty-odd subcommands,
  active *and* passive data connections, a genuine ASCII/binary distinction that
  translates line endings in both directions, and a `PASV` parser that tolerates
  malformed 227 replies. `src/tools/telnet.c` has a full IAC state machine that survives
  `recv()` boundaries and a documented reason for refusing every option whose
  subnegotiation it could not complete. `src/tools/tftp.c` enforces the TID, avoids the
  sorcerer's-apprentice duplicate-ACK bug, and handles the empty terminating block.

And on maintenance, the direction is the opposite of the premise. Ours are AmigaDOS
commands with `ReadArgs` templates and prose diagnostics, they run on Roadshow and AmiTCP
as well as on this stack because they use nothing but the published socket vectors, and
they are code we own. Theirs is vendored code consumed unmodified — the category this
project has repeatedly had to work around with symbol overrides, `-Wl,--wrap` (§13.2) and,
as §30.3 above shows, port-header corrections for defects nobody upstream has hit.

**Nothing was ported.**

### 30.7 A telnet *server*, scoped but not built

Raised separately and it is a different question, because a server is new capability rather
than a duplicate: everything this project ships is a client, and `listen()`/`accept()` is
exercised only by `nc -l` and active-mode FTP, guest-to-guest, because SLIRP forwards
nothing inward (§10).

**Can the vendored telnet server be used?** Not from a Shell command — same wall, it takes
an `NX_IP` and calls `tx_thread_create()`, `tx_timer_create()` and
`tx_event_flags_create()`. But unlike the FTP and TFTP servers it needs **no FileX**, and
unlike a command it *could* live inside `bsdsocket.library`, which is where the kernel
actually runs. What it would give: a listener on port 23, up to `NX_TELNET_MAX_CLIENTS`
(4) sessions, `WILL ECHO` / `DONT ECHO` / `WILL SGA` on connect, an activity timeout, and
three callbacks — `new_connection`, `receive_data`, `connection_end`. What it would not
give is the entire hard part: it has no notion of authentication and no notion of a shell.

**The hard part, and what it actually costs.** AmigaOS has no socket-as-file-handle, so a
descriptor cannot be handed to `SystemTagList()`. Roadshow's answer is a DOS handler, and
its `tcp-handler.doc` documents the mechanism exactly:

```
    Open("TCP:[HOST=<name or address>]/[PORT=<port number>]", ...)
    Open("TCP:OBTAIN=<number>", ...)
      ...
    PACKETS: ACTION_FINDINPUT  ACTION_FINDOUTPUT  ACTION_FINDUPDATE  ACTION_END
             ACTION_READ  ACTION_WRITE  ACTION_WAIT_CHAR
             ACTION_IS_FILESYSTEM  ACTION_STACK
      ...
    SEE ALSO: bsdsocket.library/ObtainSocket()
```

That is the right mechanism here and the missing half of it already exists: `ObtainSocket`,
`ReleaseSocket` and `ReleaseCopyOfSocket` are implemented in `src/bsdsocket/handoff.c` and
published at LVOs -0x090, -0x096 and -0x09c. So a telnet server becomes

```
    accept()  ->  ReleaseCopyOfSocket(fd, id)
              ->  Open("TCP:OBTAIN=<id>")            a DOS filehandle
              ->  SystemTagList("", SYS_Input, fh, SYS_Output, fh2,
                                    SYS_Asynch, TRUE)
```

and the cost is one handler process answering the nine packet types above — a few hundred
lines, in `src/` and not in `third_party/`. `ACTION_WAIT_CHAR` is the one that matters for
a Shell: it is what lets the console layer poll rather than block.

The alternative — pumping bytes between the socket and a Shell's input and output handles —
is worse than it looks. It needs `PIPE:` (`L:Queue-Handler`, present on a normal 3.x
install but not something to depend on), and it needs to wait simultaneously on a socket,
which is `WaitSelect()`, and on a DOS `Read()`, which is not selectable. That is two
threads per session and a shutdown race, to end up with something less useful than the
handler.

**The handler is worth building on its own account**, independently of telnet: it makes
`Type TCP:host/daytime` and `Copy TCP:…` work for every AmigaDOS program, and it is part of
the documented Roadshow surface this project is tracking.

**And it generalises to Dropbear**, which is the stated next destination. An SSH server has
the identical problem — attach a shell to a socket, on a system with no pty — with
authentication and crypto on top, and `src/crypto68k/` already accelerates the primitives it
would want. The socket-to-filehandle bridge should be built **once**, as a handler, and used
by both.

**Security, stated rather than omitted.** A telnet server with no authentication is a
remote shell for anyone on the LAN, in clear text, with no audit trail — and unlike a
client, it is reachable by people who did not choose to run it. The user's position is that
this is an obsolete machine protecting nothing valuable, which is a fair assessment of the
*data*; it is not an assessment of the machine as a foothold on the network it is plugged
into. So if it is built, the recommendation is: **off unless configured on**, bound to a
configurable interface rather than every interface, a password required by default with the
no-password case something the user has to write down explicitly, and the fact that it is
listening reported by `ShowNetStatus` so a machine cannot be left open by accident. None of
that is expensive, and all of it is much cheaper before the handler exists than after.


## 31. `ssh` from an Amiga (2026-07-26)

Upstream Dropbear, unpatched, cross-built for m68k AmigaOS 3.x, opening an
outbound SSH connection through our `bsdsocket.library` and running a command on
a real server. Read from the emulated 14 MHz A1200, `clients/dropbear/
run-fsuae.sh`:

```
--- SYS:dbclient -V
Dropbear v2026.94
--- rc 0, 0.22 s

--- SYS:dbclient -T -y -y -i DH0:id_amiga -p 2222 turo@10.0.2.2 "echo AMIGA-SSH-OK; uname -a; date"
SYS:dbclient: Caution, skipping hostkey check for 10.0.2.2
AMIGA-SSH-OK
Darwin Mac.local 25.5.0 Darwin Kernel Version 25.5.0: … arm64
Sun Jul 26 13:30:35 PDT 2026
--- rc 0, 96.06 s
```

`curve25519-sha256`, host key `ssh-ed25519`, `chacha20-poly1305@openssh.com`
both directions, public-key authentication with an ed25519 key — the modern
default suite, negotiated with a **stock OpenSSH 10.2** that knows nothing about
this client and was given no compatibility settings. The binary is 331,220 bytes
(278 KB text), against curl's 899,048.

**Nothing in `third_party/dropbear` is patched.** There is no counterpart to
`clients/curl/curl-amitls.patch`; the whole port is one C file, two shim
headers, a `localoptions.h` and the flags in `clients/dropbear/build.sh`.
Dropbear is MIT (with public-domain libtomcrypt/libtommath and two 2-clause BSD
files from OpenSSH), which suits this tree's licensing throughout.

### 31.1 What Dropbear needed that curl did not

curl was easy for a reason that is easy to miss: **curl already knows this
platform.** `lib/curl_setup.h` knows a socket is not a file descriptor here,
that `close()` is `CloseSocket()`, that `fcntl()` must not touch a socket and
that `select()` is `WaitSelect()`. §11.7 predicted wget would be harder because
it knows none of that. Dropbear is in wget's position — `grep -ril amiga src/`
finds nothing — and this is what that costs.

Everything **compiled**. The whole gap arrived as one link error list, 36
symbols:

```
bind chdir connect dup dup2 execv fcntl fork fsync getgroups gethostbyaddr
gethostbyname getpass getpeername getpid getpwnam getpwuid getrlimit
getservbyname getsockname getsockopt getuid inet_aton inet_ntoa kill listen
pipe select setrlimit setsid setsockopt shutdown signal socket tcgetattr
tcsetattr vfork
```

`socket` and `connect` being in that list is the finding. The Roadshow NDK's
`<proto/bsdsocket.h>` defines them as inline macros with the plain BSD names, so
a client that includes it gets them free — and Dropbear includes
`<sys/socket.h>`, which **declares** them and links against nothing.

### 31.2 The one real problem: two descriptor spaces that both start at zero

`bsdsocket.library` allocates socket descriptors from its own table starting at
0 (`src/bsdsocket/socket.c`, `bsd_fd_alloc()`). newlib allocates file
descriptors starting at 0 as well, and 0/1/2 are the Shell's standard streams.
So `socket()` returns 0 while `stdin` is also 0 — and an SSH client is precisely
a program that holds both at once and copies between them.

`clients/dropbear/amiga_dropbear.c` merges the two spaces by **offset**:

| range | what it is |
|---|---|
| 0 – 63 | newlib: `stdin`, `stdout`, `stderr`, files |
| 64 – 191 | `bsdsocket`, biased by `DB_SOCK_BASE` |
| 192 – 193 | the wakeup "pipe" |
| 194 | the entropy device |

Every call that takes a descriptor dispatches on the range. Three details were
decisions rather than details:

- **`read`, `write`, `close` and `open` are taken with `-Wl,--wrap`.** All four
  live in ONE object in newlib's `libc.a` (`lib_a-open.o`), so referencing any
  of them drags in all four and none can be redefined. `--wrap` is also the only
  route that survives `atomicio(read, …)`, which passes `read` as a **function
  pointer** — a macro would not have.
- **`FD_SETSIZE` is 256.** newlib's default is 64 and 192 has to fit.
  `dbutil.c`'s `dropbear_fd_set()` checks the bound itself, so an overflow is a
  legible error rather than a smashed stack.
- **`select()` is ours, not `WaitSelect()`.** `WaitSelect()` understands sockets
  and nothing else, and Dropbear's `session_loop()` selects over the socket AND
  the channel's `stdin`/`stdout`/`stderr` in one call. The shim splits the set,
  answers the DOS handles without blocking (`IsInteractive()` then
  `WaitForChar(h, 0)`; a non-interactive handle is always readable, because a
  read returns data or end-of-file and either is progress), and calls
  `WaitSelect()` with a zero timeout when anything off-socket is already ready.

### 31.3 Two AmigaOS bugs that both looked like something else

Neither is a Dropbear bug, and neither would have been found by reading.

**A requester nobody could click.** `dbrandom.c`'s `write_urandom()` feeds the
pool back with `fopen(DROPBEAR_URANDOM_DEV, "w")` and calls the result
opportunistic — *"don't worry about failure"*. On Unix it is. Our device is
named `RANDOM:`, which to dos.library is a **volume name**, and its answer to an
unmounted volume is not an error code:

```
Please insert volume RANDOM: in any drive
```

On a headless run there is nobody to press Cancel, so the process waits forever.
The symptom was `dbclient -V` — *print the version and exit* — hanging until the
run timed out, because `seedrandom()` happens before option parsing.
`pr_WindowPtr = -1` is dos.library's documented "fail the call instead of
asking"; it is set in a constructor so it is in place before `main()`, and it is
right for **any** ported client, all of which assume a failed `open()` returns.
`dbclient -V` went from a 420-second timeout to 0.22 s. It would have bitten a
real user at a real Shell too, not only the emulator.

**Closing `stdout` rebooted the machine.** With the requester gone, the run did
this, over and over:

```
--- SYS:dbclient -T -y -y -i DH0:id_amiga -p 2222 turo@10.0.2.2 "echo AMIGA-SSH-OK; uname -a; date"
AMIGA-SSH-OK
Darwin Mac.local 25.5.0 … arm64
Sun Jul 26 13:25:07 PDT 2026
     <machine reboots; ClientRun restarts and the command list begins again>
```

A complete, correct transcript, and then nothing — no return code, because the
machine did not survive to write one.

On Unix a process owns its descriptor table and `close(1)` at exit is free. On
AmigaOS a Shell command does **not** own `Input()` and `Output()`: they are the
parent's DOS `FileHandle`s, lent for the duration. newlib's `close()` `Close()`s
the BPTR, and then `SystemTagList()` in the parent closes the same freed handle
again. On a machine with no memory protection a double `Close()` is not an error
return.

Dropbear does exactly this and is right to: `common-channel.c`'s
`close_chan_fd()` calls `m_close()` on the session channel's
`readfd`/`writefd`/`errfd`, and for `dbclient -T host command` those three are
0, 1 and 2. The shim's `close()` refuses them. **This is the most transferable
line in the port**: every future client will do it, and it fails as a reboot
several seconds after a run that looked successful.

### 31.4 The three questions that decided whether this was possible

**1. `fork()` — the client needs none, and that is a property of the client
rather than a workaround.** Dropbear calls `fork()` in eight places. Six are
server-only or `scp`. The two a client can reach are `-J proxycmd` / `-B netcat`
(`spawn_command()`) and the `SSH_ASKPASS` helper, and all three are switched off
in `clients/dropbear/localoptions.h`. A linked `dbclient` contains the `ENOSYS`
stubs and calls none of them. Nothing was worked around, and `getpass()` over
`dos.library` is a better answer than the forked helper anyway.

**2. The pty — sidestepped, and it is the exact boundary of what works.**
`tcgetattr()`/`tcsetattr()` fail with `ENOTTY`, deliberately: AmigaOS has no
termios, a console is a DOS handle and raw mode is `SetMode()`. Dropbear calls
them only on the pty path, so `dbclient -T user@host command` never does. The
consequence is precise: **`dbclient host` without `-T` fails at Dropbear's own
"Failed to set raw TTY mode"**, and an interactive session needs the same
mechanism a telnet server needs — a DOS handler that makes a Shell look like a
socket. Roadshow solves it with `tcp-handler`. That is being scoped separately
and there should not be two.

A second, quieter limit sits in the same place: the shim's `select()` can only
notice an interactive `stdin` when `WaitSelect()` next returns, so a keystroke
can wait as long as `select_timeout()`. Non-interactive input (a file, `NIL:`)
is unaffected, which is why the supported shape is the one that works.

**3. Entropy — survivable for a client, disqualifying for a server.**
`dbrandom.c`'s `seedrandom()` opens `DROPBEAR_URANDOM_DEV`, reads 32 bytes and
`dropbear_exit()`s if it cannot. There is no build without one. So the device is
renamed `RANDOM:` — shaped like an AmigaOS device, because nothing here should
pretend to be Unix — and the shim answers `open()` for that one string from
**`src/common/ami_random.c`**, the same generator `bsdsocket.library` uses for
TCP initial sequence numbers and `nx_secure` uses for TLS key agreement. One
entropy story per machine, and one place to fix it.

**What that is worth, plainly.** The conditioning is textbook. The collection is
guesswork: it credits itself about **21 bits** against its own 64-bit bar, so
`ami_random_is_seeded()` returns FALSE by construction and
`include/aminetxduo/random.h` says so in its opening paragraph. Several of its
sources were measured byte-identical across three cold boots and are credited
nothing.

For the client, what comes out of that pool is the ephemeral curve25519 private
key and the session cookie — per-connection, forward-secret, never written down.
An attacker who can predict them reads *that* session.

For a server it is disqualifying, and this is the finding to carry forward: a
host key generated from a 21-bit pool is a key an attacker can enumerate, it
persists on disk, and clients pin it. That does not weaken the protocol, it
defeats it. `clients/dropbear/sshd-testserver.sh` therefore generates even the
*test client* key on the host, with a natively built `dropbearkey` from the same
pinned source, and says why in its header.

### 31.5 What a handshake costs

Timed by the guest's own clock, through `ClientRun`. Every row is a real
connection to a real OpenSSH, authenticated with a real key, running a real
command:

| connection | negotiated | wall clock |
|---|---|---|
| 1st, `echo; uname -a; date` | curve25519 / ed25519 / chacha20-poly1305 | **96.06 s** |
| 2nd, same command line | same | **95.88 s** |
| 3rd, `-c aes128-ctr` | curve25519 / ed25519 / **aes128-ctr + hmac-sha2-256** | **96.06 s** |
| 4th, `-c chacha20-poly1305@openssh.com` | as 1st, named explicitly | **95.92 s** |

Two things fall out of that table before any analysis.

**The cipher does not matter and the key exchange is everything.** Rows 3 and 4
differ only in the record path, and they differ by 0.14 s out of 96. The
payload is about 3 KB; at this size the symmetric cost is invisible and the
entire wall clock is public-key arithmetic. §18's per-byte work would show up on
a transfer, not on a login.

**SSH has no session resumption.** Row 2 costs what row 1 costs. There is no
counterpart to §13's 5.10 s → 0.62 s, and there cannot be: the protocol has no
such mechanism. Every connection pays the full key exchange, forever, which
makes the per-handshake number matter more here than it does for HTTPS.

#### The optimistic guess a modern OpenSSH always rejects

Dropbear sets `first_kex_follows` and sends a `KEXDH_INIT` for its own
first-preference key exchange before it has seen the server's `KEXINIT`,
betting the server prefers the same one. Against OpenSSH 10.2 that bet is not a
gamble, it is a guaranteed loss — OpenSSH prefers an ML-KEM hybrid we do not
offer:

```
debug2: proposal mismatch: my mlkem768x25519-sha256 peer curve25519-sha256
debug2: skipped packet (type 30)
```

`cli-kex.c`'s `send_msg_kexdh_init()` then runs again, beginning with
`cli_kex_free_param()` and a **fresh** `gen_kexcurve25519_param()`. Measured,
same binary otherwise, same server:

| | wall clock |
|---|---|
| `DROPBEAR_KEX_FIRST_FOLLOWS 1` (upstream default) | 96.06 95.88 96.06 95.92 s — mean **95.98** |
| `DROPBEAR_KEX_FIRST_FOLLOWS 0` | 84.18 83.96 s — mean **84.07** |

**About twelve seconds, which is one curve25519 scalar multiplication on this
part.** What the guess buys in exchange is one round trip. It is off in
`clients/dropbear/localoptions.h`.

#### `LoginGraceTime`, and why the test server is ours

**OpenSSH's default `LoginGraceTime` is 120 seconds** from TCP connect to
completed authentication, and 96 seconds of arithmetic does not leave much of
it — a slower machine, a deeper `-v`, or one retry and the server hangs up
first. This is the wall §11.8 hit with Cloudflare, arriving from a different
direction. `clients/dropbear/sshd-testserver.sh` sets `LoginGraceTime 600` so
that a run measures the Amiga rather than the server's patience, and that
setting is a statement about the client, not a convenience.

**One methodological note, because it cost time.** The server's log was
timestamped host-side to try to split the handshake into keygen / shared secret
/ verify. Under the load this build host actually carries the host-side
timestamps drift by tens of seconds and the split came out contradicting itself.
The guest's own `DateStamp()` clock, which is what `ClientRun` reports, stayed
consistent to 0.2 s across four connections. Trust the in-guest timer; a
host-side log timestamp is not a measurement of the guest.

### 31.6 None of `src/crypto68k/` applies to this, and that is fixable

`src/crypto68k/` accelerates RSA-2048, P-256, SHA-256 and AES-128-CBC — 1.25×
AmiSSL on AES, 1.62× on HMAC-SHA256, 10.8× on a P-256 scalar multiply (§15,
§18). The suite this handshake negotiated is **curve25519, ed25519 and
chacha20-poly1305**, and `crypto68k` accelerates none of them. Wiring it in
would buy exactly nothing against a modern OpenSSH.

That is not the end of it, because Dropbear offers the other half as well, and
this binary already contains it:

```
kex     curve25519-sha256  ecdh-sha2-nistp256/384/521  diffie-hellman-group14-sha256
hostkey ssh-ed25519  ecdsa-sha2-nistp256  rsa-sha2-256
cipher  chacha20-poly1305@openssh.com  aes128-ctr  aes256-ctr
mac     hmac-sha2-256
```

The second column of each row is `crypto68k`'s territory. `ecdh-sha2-nistp256`
is the P-256 scalar multiply that is 10.8× faster in our code than in AmiSSL's;
`rsa-sha2-256` is RSA-2048; `aes128-ctr` + `hmac-sha2-256` is the record path
§18 rewrote — and row 3 of the table above proves the client negotiates it on
request. So the question is not "does `crypto68k` apply to SSH" but **"is P-256
with our assembly faster on this machine than curve25519 in portable C"**, and
nobody has measured it. `-c` and `-m` select the cipher and MAC at runtime; the
key exchange list is compile-time, so the experiment is one more build with
`DROPBEAR_CURVE25519 0` and the harness that is now in the tree.

Two things bias the answer in opposite directions and neither is obvious. Our
P-256 is hand-written 68020 assembly with a limb-domain Solinas reduction;
Dropbear's curve25519 is portable C over 64-bit arithmetic, which on m68k means
software 64-bit multiplies — that favours us. Against it, **chacha20 is
add-rotate-xor with nothing to look up**, and §18 established that this part has
no data cache and charges 159.8 ns for a table read regardless of table size, so
AES pays full memory latency on all sixteen lookups per round and chacha20 pays
none. It is entirely possible we win the key exchange with P-256 and lose the
record path with AES, and that the right answer is a mixture.

### 31.7 The harness, and what a second tenant actually cost

| | |
|---|---|
| `third_party/dropbear` | submodule pinned to `DROPBEAR_2026.94`, **unpatched** |
| `clients/dropbear/build.sh` | cross-configure and build |
| `clients/dropbear/localoptions.h` | what is compiled in, and why each thing is off |
| `clients/dropbear/amiga_dropbear.c` | the port: the descriptor map, `select()`, the stubs |
| `clients/dropbear/include/` | `dirent.h` and `termios.h` |
| `clients/dropbear/run-fsuae.sh` | the run |
| `clients/dropbear/sshd-testserver.sh` | a rootless OpenSSH on port 2222 for it to talk to |

`clients/compat/` and `clients/amiga-client.sh` were reused **unchanged** — the
`stat`/`fstat`/`mkdir`/`unlink`/`isatty`/`gettimeofday` shims, the libgcc
helpers, the `crt0.o` `argv` repair and the three NDK flags all carried over with
no edit at all. `clients/curl/clientrun.c` was reused as the driver as well; it
is a general "run these command lines with a real stack" program, and `dbclient`
needs the 512 KB for the same reason curl does. **The harness took a second
tenant without being touched**, which is what §11.5 claimed it would do.

**`./configure` rather than a hand-written `config.h`.** §11.7 recommended the
hand-written route for wget because wget needs `./bootstrap` and therefore
autoconf, automake, libtool, gettext and a gnulib checkout on the build host.
Dropbear ships a **generated** `configure`, so a cross-configure is one command
with no build-host autotools — and it is worth more than a hand-written header,
because it finds the gaps by **linking** rather than by somebody remembering to
list them.

One thing it must not see: `amiga_dropbear.o`. That object defines `fork()`,
`getpass()`, `select()` and `getpwnam()`, so a `configure` run with it in `LIBS`
would answer `HAVE_FORK=1` and Dropbear would compile the `fork()` paths for a
`fork()` that always fails. It is added at **make** time, so `config.h`
describes the toolchain honestly.

`--disable-harden` is not cosmetic. Dropbear's configure probes for `-fPIE`,
`-fstack-protector-strong` and `-D_FORTIFY_SOURCE=2` and keeps whatever
compiles. All three "compile", and the third is fatal several tests later:
`_FORTIFY_SOURCE` pulls in newlib's `<ssp/*.h>`, whose `__ssp_redirect0` macros
do not expand under this GCC, so **every subsequent configure test fails for a
reason unrelated to what it was testing** — which is how `netinet/in.h` and
`netdb.h` get reported missing when both are present. None of the three means
anything on a machine with no MMU, no ASLR and no guard page. `build.sh` fails
loudly if `HAVE_NETINET_IN_H` is not set, because that is the symptom rather
than the cause.

### 31.8 The server: Dropbear or TinySSH

One premise needs correcting first. **Dropbear's server does not have to fork
per connection.** `INETD_MODE` is a supported build (`svr-main.c`,
`main_inetd()`): it takes the connection on descriptor 0, runs one session and
exits, and `NON_INETD_MODE` — the accept loop with the `fork()` at
`svr-main.c:313` — can be compiled out entirely. On the *connection* axis
Dropbear and TinySSH are therefore the same shape: both need an external
supervisor to accept and spawn.

**The fork neither of them avoids is the other one.** Dropbear forks at
`svr-chansession.c:835` to run the user's shell or command; TinySSH forks at
`channel_fork.c:34` and `channel_forkpty.c` for the same reason. That is not a
process-model quirk, it is what an SSH server *is* — and on AmigaOS it is the
pty question in disguise, because the child has to be a Shell attached to
something the parent can read and write. **Blocker 1 and blocker 2 are the same
blocker**, and it is the one already being scoped for telnet.

With that established:

| | Dropbear server | TinySSH |
|---|---|---|
| licence | MIT (+ PD libtom, + 2-clause BSD in `loginrec.c`/`sshpty.c`) | CC0-1.0 OR 0BSD OR MIT-0 OR MIT |
| connection model | `-i` inetd mode, no fork | inetd only, no fork |
| session model | `fork()` + pty | `fork()` + `forkpty()` |
| auth | pubkey only here — password auth is `#error … requires crypt()` and this toolchain has none | pubkey only by design |
| crypto | curve25519 / P-256 / RSA / ed25519 / AES-CTR / chacha20 — includes everything `crypto68k` accelerates | curve25519 / sntrup761 / ed25519 / chacha20-poly1305 — includes **none** of it |
| entropy | `open()` on a device, `dropbear_exit()` on failure | `open("/dev/urandom")` in a **constructor**, inside `for(;;) { …; sleep(1); }` — a port that does not intercept it hangs forever with no message |
| the AmigaOS port | **already written**, and shared with `dbclient` | a second port of the same shape: descriptor map, `poll()` rather than `select()`, entropy, `openpty` |

**Recommendation: Dropbear's server, and the deciding argument is reuse rather
than merit.** `dropbear -i` is `clients/dropbear/build.sh -P dropbear` against
the shim that already exists — the same descriptor map, the same `select()`, the
same entropy device, the same requester fix, the same `close(1)` fix. TinySSH is
a second port of every one of those, and its `randombytes()` constructor fails
by *hanging* rather than by exiting, which is the failure mode that costs the
most to diagnose; §31.3 has a worked example of exactly that costing a run.

TinySSH's genuine advantages are real and should be recorded rather than
dismissed. The licence is cleaner, the code is far smaller, there is no password
path to disable, and its algorithm set is precisely the one that needs no tables
on a machine with no data cache — if the chacha20-versus-AES question in §31.6
resolves the way §18's instruction timings hint, its crypto choice is the right
one for this hardware. What it does **not** do is avoid the blocker it was
proposed for, and it would cost a second port to establish that.

Against either candidate the same three things bind:

1. **A supervisor.** A small AmigaOS program: `socket`, `bind`, `listen`,
   `accept`, then `CreateNewProc()` with the accepted socket handed across —
   `ObtainSocket()`/`ReleaseSocket()` are published vectors and exist for
   precisely this. Shared between the two candidates, and reusable for telnet.
2. **A Shell on a socket.** The same DOS handler telnet needs. Not to be built
   twice.
3. **A host key.** Blocking, and not an engineering problem anywhere else in the
   stack can solve. Twenty-one bits of credited entropy is not a host key. The
   options are to generate it off-machine and install it — which is what
   `sshd-testserver.sh` does for the client key today, and what a shipping
   server could reasonably require — or to feed `ami_random_add_entropy()` a
   real seed, for which operator keystroke timing at first boot is the classic
   answer and this machine does have a keyboard. Either is fine. Neither is not.

## 29. Measured against Roadshow 1.15 and AmiTCP_NG 4.1.1 (2026-07-26)

Everything above compares this stack against itself. This section compares it
against the two other SANA-II stacks that can be dropped into the same FS-UAE
profile: **Roadshow 1.15** — the commercial stack whose ABI this project
implements — and **AmiTCP_NG 4.1.1**, a GPL fork of AmiTCP 3.0b2 with a
clean-room Roadshow ABI.

**Nothing of either was copied into this tree.** `tests/compare/run-compare.sh`
takes a path to an unpacked installation and stages it at run time; Roadshow is
a commercial demo and AmiTCP_NG is GPL, and this tree is MIT.

**We win conformance and loopback outright, we win the wire on our own
instrument, and we lose the wire on somebody else's client.** That last one is
the most useful result in the section and it is stated first for that reason.

### 29.1 The rig, stated so it can be disputed

| | |
|---|---|
| emulator | FS-UAE 3.2.35, `-m A1200`: 68EC020 at 14 MHz, cycle-exact, 8 MB Zorro II Fast |
| ROM | Kickstart 3.1 40.68 (A1200) |
| NIC | one `a2065.device` on SLIRP, 10.0.2.0/24 |
| host | macOS 26.5, Apple M3; `build/.fsuae.lock` held for every run, so no two emulators ran together |
| UAE's own emulation | `bsdsocket_library = 0` — otherwise there is a fourth stack in the room |

Four things are the **same binary in every column**, which is the point:

* **the driver** — `tests/curl/curlcheck.c`, which runs each command with a
  512 KB stack and records its exit code, elapsed ticks and `AvailMem`;
* **`NetTrace`**, built once from this tree and staged unchanged against all
  three. It links nothing of `src/`: every call into the library is a published
  LVO through `toolsock.c`'s inline `jsr a6@(-n:W)`, so it is exactly as foreign
  to Roadshow as to us;
* **`bsdsocktest`**, the upstream suite, which knows about none of the three;
* **the Aminet `curl.020` 8.22.0-DEV** of §14.7 — clib2, AmiSSL, built by
  somebody with no stake in the result.

Each stack supplies only its **own** `bsdsocket.library`, `AddNetInterface` and
`ping`. The `DEVS:` tree, the interface file (`DEVICE=a2065.device`,
`CONFIGURE=DHCP`) and the driver binary are identical, and the a2065 driver is
staged in both `DEVS:` and `DEVS:Networks/` because the three stacks look in
different places for it.

**The builds are not comparable and that is stated rather than hidden.**

| | compiler | flags |
|---|---|---|
| AmiNetXDuo, commit **`b3b4b49`** | `m68k-amigaos-gcc` 15.2.0 | libraries `-O3`, commands `-Os`, `-m68020` |
| Roadshow 1.15 demo | shipped binary | not disclosed |
| AmiTCP_NG 4.1.1 release `.lha` | bebbo GCC 6.5.0b | `-O1 -fomit-frame-pointer -noixemul -std=gnu89`, 68000 baseline |

Our arm is `git archive b3b4b49` built into a private directory, because
`src/netstack/`, `src/bsdsocket/socket.c` and `nx_user.h` were all under active
change while this was measured and §24.9 records two measurements already lost
to exactly that. **`b3b4b49` contains §24's pool-derived receive window**, so
these are post-window figures, not stale ones. Neither foreign stack was tuned;
both ran as shipped.

### 29.2 Conformance — we are four ahead of Roadshow on both tiers

The suite identifies the library it ran against in its own TAP output, so there
is no question which one answered: `# bsdsocket.library: AmiNetXDuo` and
`# bsdsocket.library: Roadshow 4.364 (1.9.2023) DEMO`.

| | AmiNetXDuo | Roadshow 1.15 | AmiTCP_NG 4.1.1 |
|---|---|---|---|
| **network tier** (`HOST 10.0.2.2`) | **141 passed, 1 failed, 0 skipped** | **137 passed, 5 failed, 0 skipped** | could not run |
| **loopback tier** (`LOOPBACK`) | **130 passed, 0 failed, 12 skipped** | **126 passed, 4 failed, 12 skipped** | could not run |
| suite wall time, network tier | 35.6 s | 49.6 s | — |
| suite wall time, loopback tier | 15.0 s | 28.9 s | — |

Roadshow's five network-tier failures:

| # | | |
|---:|---|---|
| 27 | `recv(MSG_OOB)` returns `EINVAL` | the suite's own `known_failures.c` |
| 35 | loopback generates no RST for a closed peer | same |
| 76 | `SBTC_ERRNOLONGPTR` GET unsupported | same |
| 77 | `SBTC_HERRNOLONGPTR` GET unsupported | same |
| 41 | `accept()` an incoming connection from the helper | **ours fails this too** |

**Test 41 cancels out.** It is the failure §12 and §17.4 already name: FS-UAE
3.2.35's SLIRP opens no inbound TCP socket, so the helper cannot connect back
to the guest. It fails on both stacks for the same environmental reason. The
honest scoreline is **141–137 with one shared environmental loss**, and the four
we win are exactly the four the suite itself documents as Roadshow deviations —
i.e. we are ahead precisely where §17 predicted and nowhere else.

**A correction to §17.4, which quoted Roadshow at 138 passed, 4 known,
0 skipped.** That figure came from the suite's `known_failures.c`, not from a
run. Measured here Roadshow scores **137**, and the missing one is test 41:
`138 + 4 = 142` was a run with a working inbound path, and this rig has none.
Our own 141/1/0 and 130/0/12 reproduce §24.9 exactly, from a different harness,
which is what makes the Roadshow column trustworthy.

### 29.3 Throughput, and the two instruments that disagree

`NetTrace`, one binary, `NOCAPTURE`, 524,288 bytes, against
`tests/curl/curlpeer.py`. Every boot runs each workload twice.

| | AmiNetXDuo | Roadshow 1.15 | |
|---|---:|---:|---|
| **loopback**, 6 vs 4 samples | **352 KB/s** (351–353) | **251 KB/s** (234–265) | **+40%** |
| **wire**, steady-state samples | **180 KB/s** (176–186) | **116 KB/s** (115–117) | **+55%** |
| wire, first fetch of a boot | 115–180 KB/s | 115–190 KB/s | both noisy |

**Loopback is the solid one.** Six of our samples span 2 KB/s; four Roadshow
samples span 31. The *first* wire fetch after bring-up is noisy on both stacks
and both directions — ours has been as low as 115 and Roadshow as high as 190 —
so only the second fetch of each boot is quoted, where ours is 176/177/179/186
and Roadshow's is 115/117.

**And then the same wire, measured with somebody else's client, reverses.**
Aminet `curl.020`, identical binary, 1,200,000 bytes over `http://`, five
fetches each:

| | AmiNetXDuo | Roadshow 1.15 | |
|---|---:|---:|---|
| total, mean of 5 | 10.72 s = **112 kB/s** | 9.39 s = **128 kB/s** | **Roadshow +14%** |
| `time_connect` | 0.37–0.51 s | 0.67–0.76 s | ours |
| `time_starttransfer` | 0.92–1.08 s | 1.17–1.27 s | ours |
| body only, 1,200,000 B | 9.76 s = 123 kB/s | 8.50 s = 141 kB/s | Roadshow |
| body only, 300,000 B | 2.38 s = 126 kB/s | 2.17 s = 138 kB/s | Roadshow |

**This is a loss and it is reported as one.** With a client that has no stake in
either stack, Roadshow moves the same 1.2 MB about 1.3 seconds faster, every
time, five times out of five, and the gap scales with the body rather than
sitting in setup — 123 against 141 kB/s at 1.2 MB and 126 against 138 at 300 KB.

Three things are worth reading off it:

* **We win the connect and the first byte and lose the bulk.** `time_connect`
  and `time_starttransfer` are ours by 0.2–0.3 s on every fetch; everything we
  lose, we lose after the first byte arrives.
* **The disagreement between the two instruments is the finding, not a
  contradiction.** Same wire, same peer, same payload size, same boot order:
  `NetTrace` says we are 55% faster and curl says we are 12% slower. What
  differs is the *receive call pattern* — read size, and how many `WaitSelect()`
  round trips a megabyte costs — so the gap lives in our recv/select path and
  not on the wire. `NetTrace` reads 4,096 bytes at a time through its own
  single-`WaitSelect()` loop, and curl does not.
* **It is not a regression against §24.** §24's 182 KB/s for `a04_get_1m2` is
  *our* curl (newlib, `tls.library`); this is the clib2/AmiSSL Aminet binary,
  which §14.7 ran for pass/fail and never timed. The two numbers are different
  clients, not different stacks.

**The next step is named rather than guessed at**: instrument the guest's
`recv()` sizes and `WaitSelect()` count for one curl fetch and one `NetTrace`
fetch of the same size, with the bpf capture of §16 running on both, and compare
segment counts and inter-segment gaps. If curl is making several times as many
short reads, the cost is per-call and measurable directly.

### 29.4 Time to a DHCP lease — ours, by a factor of about three

Each stack's own `AddNetInterface`, timed by the driver at 50 Hz. All three
commands block until the interface has an address or the attempt has failed, so
the command's elapsed time *is* the figure.

| | AmiNetXDuo | Roadshow 1.15 |
|---|---:|---:|
| `AddNetInterface DEVS:NetInterfaces/eth0` | **1.72–1.90 s** (7 boots; one outlier at 2.40) | **4.74–5.28 s** (7 boots) |

Both end with `10.0.2.15`, a default route to 10.0.2.2 and a nameserver from
the lease. **This is the figure most likely to be stale first**: the DHCP
lifecycle is under active change in `src/netstack/` and the number above is
`b3b4b49`'s.

### 29.5 ICMP round trip — Roadshow, by about 2 ms

Each stack's own `ping`, five probes to 10.0.2.2, no loss on either.

| | min / avg / max |
|---|---|
| AmiNetXDuo | 7 / 8 / 10–11 ms |
| Roadshow 1.15 | **4.32 / 5.53–6.06 / 6.69–9.73 ms** |

**Another loss.** Our `ping` prints whole milliseconds so 7 could be anything
from 6.5 to 7.5, but Roadshow's minimum is 4.32 ms and ours never goes below 7,
and its *maximum* on one run (6.69) is below our minimum. That is a real
difference in ICMP turnaround, not a rounding artefact, and it is the same
direction as the curl result: our per-packet path costs more than Roadshow's.

### 29.6 AmiTCP_NG 4.1.1 would not start on this machine

**Every socket call fails before any of the above can be measured.**

```
NetTrace: socket() failed: the stack reported error 43     (EPROTONOSUPPORT)
eth0: AddInterface failed, errno 43
# bsdsocket.library: not available                          (the suite's own line)
```

`OpenLibrary("bsdsocket.library", 4)` **succeeds** — `NetTrace` gets past it and
prints its banner — and then `socket(AF_INET, SOCK_STREAM, 0)` returns
`EPROTONOSUPPORT`, which is what BSD returns when the INET domain was never
attached. The stack's self-start does not complete here. The suite runs to test
49, fails 42 of the first 49, and the machine then stops responding; the run
ends on its own timeout.

Five things were eliminated rather than assumed:

| tried | result |
|---|---|
| their own `Storage/NetInterfaces/A2065` instead of ours | identical, errno 43 |
| the device named by full path, `DEVS:Networks/a2065.device`, as their own troubleshooting note recommends | identical |
| their `db/` staged at `SYS:AmiTCP` **and** an `AmiTCP:` assign made before the first library call | identical |
| an empty `AmiTCP:db/interfaces`, in case a missing file aborted init | identical |
| a **different driver** — `ToolsSmoke` instead of `CurlCheck`, no `NP_WindowPtr`, no per-command redirection | identical, so the driver is not the variable |

The same `a2065.device`, `DEVS:` tree and boot volume bring up the other two
stacks in the same harness, so this is not a broken rig. **Their project
documents validation on AmigaOS 3.2 under Amiberry**, and this harness boots
Kickstart 3.1 with a bare directory hard drive and no Workbench install; that is
the most likely gap and no Kickstart 3.2 image was available to test it. It is
reported as "did not run here", not as a defect in their stack.

Two smaller observations from the attempt, recorded because they cost time:
their commands open `CON://///AUTO/CLOSE/WAIT`, so `ShowNetStatus` driven with
its output redirected to a file never returns; and their `ping`, run with the
stack dead, took the machine into a repeating address-error loop.

Their release ships no `usergroup.library`, which a clib2-built client opens
before `main()`, so the curl workload would have needed one borrowed from
elsewhere. That never became relevant.

### 29.7 lwip-amiga cannot be compared — confirmed, and stopped there

Its README settles it in its own words: it is **not a SANA-II stack**, it is
built on a purpose-built `netdev` driver ABI, and the only driver that
implements it is `genet.device` 4.x for the onboard Ethernet of a Raspberry Pi
4/CM4 under PiStorm or Emu68. There is no SANA-II shim and the repository
publishes no releases. FS-UAE's emulated A2065 cannot present a `netdev` device,
so nothing in this section can be run against it — including the loopback tier,
which would still need an AmigaOS 3.2 machine and a build. Confirmed in a few
minutes and not pursued further.

### 29.8 The harness

`tests/compare/run-compare.sh -s ours|roadshow|amitcpng -w bench|conf|curl|diag`.
The stack is a parameter; everything else is held fixed. Foreign stacks are
located at run time (`-R`, `-G`, `AMINETXDUO_CMP_*`) and never enter the tree.
Two implementation notes worth keeping:

* **TAP counts a skip as `ok N ... # SKIP`.** Reporting the raw `ok` count would
  have called our loopback tier 142/142 when the suite's own summary says 130
  passed, 12 skipped. The script subtracts.
* **`build/` directory names are shared between workstreams.** One arm here was
  taken with `-b` pointing at a private build directory for the reason §24.9
  gives, and one run was lost outright when `tools/fsuae-run.sh` was edited by
  another workstream while bash was reading it.

## 32. Unacknowledged data was never retransmitted, and `close()` was a RESET (2026-07-26)

§27 built tcpdrill and, on its first run, found three defects. Two of them are
`src/`'s and this section is those two.

**The first is the most serious thing this project has found.** One 100-byte
segment left unacknowledged produced **eleven seconds of total silence** — no
retransmission, and not even the reset that ten expired retries should produce.
The failure is not in TCP and it is not in the timer: it is a lifecycle bug in
the SANA-II transmit ring, and it means that until now this stack could not
recover from losing a single packet.

**Result: tcpdrill goes from 200 checks passed and 10 failed to 209 and 1, with
the one remaining failure belonging to the retransmission timer's workstream.
Loopback and wire throughput are unchanged inside the noise, and the concurrency
sweep, both conformance tiers, the client patterns and curl A–F are unchanged
against a clean-`HEAD` control run on the same machine.**

### 32.1 Defect 1: a packet the driver never gave back is a packet TCP will not resend

`_nx_tcp_socket_retransmit()` walks the transmit queue with

```c
while (packet_ptr && (packet_ptr -> nx_packet_queue_next == (NX_PACKET *)NX_DRIVER_TX_DONE))
```

and `NX_DRIVER_TX_DONE` is written by `_nx_packet_transmit_release()`. So **only
a packet the driver has handed back can be retransmitted**, and that is the
whole mechanism. Every NetX Duo reference driver hands it back inside
`NX_LINK_PACKET_SEND`, because their sends are synchronous — `nx_ram_network_
driver.c` copies the frame and releases before it returns.

Ours cannot. A SANA-II `CMD_WRITE` is an exec `IORequest` that completes long
after `BeginIO()` returns, and the packet must stay intact until the device's
`S2_CopyFromBuff` has read every byte of it. So `src/sana2/sana2_tx.c` releases
in `ami_sana2_tx_reap()` instead — and reap had three callers, every one of them
reactive:

| `sana2_tx.c` | the start of the **next** transmit, and the spin when the ring is full |
| `sana2_driver.c` | `NX_LINK_GET_TX_COUNT` |
| `sana2_driver.c` | `NX_LINK_DEFERRED_PROCESSING`, which nothing ever asked for |

**A packet was therefore released by the next packet.** On a link that goes
quiet there is no next packet, the last segment sent stays un-reaped for ever,
TCP believes the driver still has it, and it is never resent. tcpdrill pinned it
exactly: a *later* `send()` on the same socket released the stranded segment,
which then went out 917 ms afterwards.

The failure is precisely the shape of a request/response protocol whose single
request segment is lost — an HTTP GET, a DNS query over TCP, a TLS ClientHello.
A bulk transfer self-heals, because the next segment reaps the previous one.

**Nothing in this tree could have found it by accident.** SLIRP does not drop,
loopback does not drop, and §16.4 and §24.4 both report zero retransmissions
across every trace ever taken here, in both directions, on both paths. The
retransmission path had never once executed.

### 32.2 The fix is two hops, and the second one is the point

Completions have to be noticed when they complete. There is exactly one place in
this shim that can notice: the SANA-II reader threads are the only threads that
block in exec `Wait()` rather than on a ThreadX object, so they are the only
ones a device's `ReplyMsg()` can wake. The `NX_IP` thread waits on ThreadX event
flags, which `Signal()` cannot break.

So the TX reply port stops being `PA_IGNORE` and raises a signal on one reader
(the IPv4 one, because it is the reader that always exists). **That thread does
not touch the packet.** It calls `_nx_ip_driver_deferred_processing()`, the IP
thread comes back into `ami_sana2_driver_entry()` with
`NX_LINK_DEFERRED_PROCESSING`, and the reap happens there.

The second hop is not ceremony. Releasing a packet mutates NetX Duo's transmit
queue *and* the packet's own prepend pointer — `_nx_packet_transmit_release()`
strips the IP header back off, which is what makes a retransmission's
`_nx_ip_packet_send()` balance. Doing that from a reader thread would interleave
it with whatever the IP thread was in the middle of. On the IP thread it runs
where every other send runs, under `nx_ip_protection`. §27.4 noted that this
shim never asked for `NX_LINK_DEFERRED_PROCESSING`; this is what the command is
for.

**The transmit path pays almost nothing, and that is by construction rather than
by hope.** The reader asks for deferred processing only when the reply port is
**not already empty** — one pointer compare — and during a bulk transfer the
next `ami_sana2_tx_send()` has already drained it. So the IP thread is never
disturbed while data is flowing, and the extra hop happens only when the link
goes quiet, which is the case that was broken. The emptiness test cannot be
wrong in the dangerous direction: exec's `PutMsg()` links the message and raises
the signal inside one `Disable()`d region, so a reader that has been woken
always sees a non-empty list.

Two smaller things fell out of it.

- **`NX_LINK_DEFERRED_PROCESSING` no longer refreshes the SANA-II statistics.**
  That is a synchronous `DoIO()` to the device. It was free while nothing ever
  invoked the command; it would have been one blocking device round trip per
  transmitted frame now that something does.
- **The reap is still called at the top of `ami_sana2_tx_send()`**, and that is
  not redundancy for its own sake: it is the only reaping there is when no
  reader is bound, which is the state an interface is in during open-time
  probing, before it is enabled, and if a reader could not get a signal bit.

### 32.3 Defect 2: `CloseSocket()` sent a RESET where RFC 793 §3.5 wants a FIN

Observed as `R seq=1 ack=0 win=32768`. §12.3 listed it as a risk that had not
been reproduced, §16.9 promoted it to an observation having seen `RST 1` at the
end of every flow in every capture, and §27.6 put the packet next to it.

What makes it a defect rather than a preference is that **the orderly-close path
was already there and already asserted**: tcpdrill `c04` has always shown
`shutdown(SHUT_WR)` on the same connection sending a correct `FIN|ACK` and
taking the ACK back. `close()` simply did not use it.

It did not use it because `nx_tcp_socket_disconnect()` offers two behaviours and
neither of them is `close()`:

| `NX_NO_WAIT` | sends a RESET and returns |
| any wait | sends a FIN and then **suspends the caller** until the peer answers or the wait expires, then tears the connection down anyway |

A blocking `CloseSocket()` is not acceptable — the descriptor is gone the
instant the call is made, and a program that closes and exits must not be made
to wait on a host that has gone away. The RESET was what was left.

**The connection now outlives the descriptor.** The FIN goes out through the
same open-coded path `shutdown(SHUT_WR)` uses, `CloseSocket()` returns, and the
`AmiSocket` is parked on a list until TCP has finished. NetX Duo's fast periodic
does the rest by itself: FIN_WAIT_1 → FIN_WAIT_2 → TIMED_WAIT and LAST_ACK →
CLOSED, retransmitting the FIN and giving up after `NX_TCP_MAXIMUM_RETRIES`. The
state machine needs nothing from us except that the control block stay alive —
`nx_tcp_socket_delete()` refuses anything that is not CLOSED, and deleting
nothing is the `AmiSocket`-per-connection leak §12.5 predicted.

The list is **global rather than per base**, because the next thing a program
does after `close()` is very often exit; a per-base list would be freed with the
base while NetX Duo still pointed into it. It is swept from `socket()`,
`CloseSocket()` and `CloseLibrary()`, each inside a bracket the caller already
holds, and it needs no lock of its own because that bracket is the ThreadX baton
— one holder at a time across every base. A socket that has not finished after
60 s is reset and reclaimed.

Two cases are still a RESET, and both are the rule rather than an escape hatch:

- **data arrived that the application never read.** RFC 1122 4.2.2.13 is
  explicit that the peer must not be told its data was delivered when it is
  about to be discarded. Every BSD aborts here.
- **`SO_LINGER` on with a zero timeout**, which is the documented way to ask for
  an abortive close and what the option is mostly used for.

And `SO_LINGER` on with a *nonzero* timeout now blocks, which is the documented
way to ask for that. The option previously chose between two flavours of reset;
it now does what its name says.

### 32.4 The follow-on nobody would have predicted, and it is the interesting half

Making `close()` send a FIN broke `tests/clients`' *"send() to a closed peer
eventually fails"* and the conformance suite's `send(): error after peer closes
connection [BSD 4.4]` — sixteen sends in a row, all of them succeeding.

They were right and the change was incomplete. With a RESET, a peer's `close()`
destroyed our socket and the next `send()` failed. With a FIN, the peer's socket
sits in FIN_WAIT_2 **and goes on acknowledging**, because a FIN closes one
direction and NetX Duo's `_nx_tcp_socket_state_data_check()` is called in
FIN_WAIT_1 and FIN_WAIT_2 exactly so that it can. Nobody will ever read that
data. 4.4BSD's `tcp_input` has the rule:

```c
if (so->so_state & SS_NOFDREF && tp->t_state > TCPS_CLOSE_WAIT && tlen)
        tp = tcp_drop(tp, ECONNRESET);
```

which is RFC 1122 4.2.2.13's rule one segment later: unreadable data is a reset.
A parked socket now installs a receive-notify callback that does exactly that.

**The teardown deliberately does not happen in the callback.** It runs from
inside `_nx_tcp_socket_state_data_check()`, which has more to do with both the
socket and the packet after it returns, and tearing the control block down
underneath it for a corner case is not a trade worth making. Sending the RST is
safe — it only builds and transmits a packet — so the callback sends it and then
gives the socket an expired timeout, and NetX Duo's own fast periodic reaches
`_nx_tcp_socket_connection_reset()` on the next 20 ms tick, from the top of the
IP thread with nothing in flight. The sweep collects the block after that.

### 32.5 TIME_WAIT, stated rather than implied

A socket that closes first reaches TIMED_WAIT, and NetX Duo would hold it there
for `2 * NX_TCP_MAXIMUM_SEGMENT_LIFETIME` = **240 seconds**. This does not wait
that long: the sweep reclaims a socket as soon as it *reaches* TIMED_WAIT.

That is a deliberate divergence and it is worth being plain about. Four minutes
of an `AmiSocket` and an ephemeral port per closed connection is not affordable
on the 4 MB floor. It is also what NetX Duo itself does — `nx_tcp_client_socket_
unbind()` collapses TIMED_WAIT to CLOSED whenever an application unbinds — and
what this library has always done. What it costs is the protection TIME_WAIT
exists for, an old duplicate landing on a reused four-tuple; what bounds that is
NetX Duo allocating ephemeral ports in ascending order rather than reusing the
one just released.

### 32.6 What tcpdrill says now, in three arms

Same harness, same 26-case script, three libraries, on the same machine:

| library | cases | checks |
|---|---|---|
| clean `HEAD` | 26, **7 failed** | 200 passed, **10 failed** |
| `HEAD` + these two fixes | 26, **1 failed** | 209 passed, **1 failed** |
| the whole working tree | 26, **0 failed** | **210 passed, 0 failed** |

The one remaining failure in the middle arm is `x01_syn_retransmission_backs_
off`, which is `NX_TCP_RETRY_SHIFT` and belongs to the retransmission timer's
workstream; the third arm has that change in it, which is why it is green there
and not here.

The nine checks that these two commits turn green:

| | |
|---|---|
| `c03` | a close is a `FIN`, and the four-way close completes |
| `x02` | unacknowledged data **is** retransmitted |
| `x03` | and it needs no later `send()` to release it |
| `x04` | eleven seconds of **trying**, where the case used to assert eleven seconds of silence and pass |
| `c11` | a close in CLOSE_WAIT is LAST_ACK |
| `c12` | `CloseSocket()` returns in 3 ms against a peer that never answers, and the FIN retransmits on the stack's own time |

Five new cases cover the close corners: `c08` (one FIN per connection, not a
second one after `shutdown(SHUT_WR)`), `c09` (unread data is an abort), `c10`
(`SO_LINGER {on,0}`), `c11`, `c12`.

**Only the first retransmission interval is asserted, and loosely.** What the
intervals are — flat, doubling, and how many before the socket gives up —
belongs to the timer and to `scripts/retransmit.drill`, so the rest of `x02` and
`x04` is a frame count with a wide bound. Four seconds of a doubling interval is
one retransmission and four seconds of a flat one is eight; both were measured,
and both pass.

### 32.7 Three harness defects, and the one that had been lying

Working on this turned up three things wrong with tcpdrill itself. The second is
the one that matters, because it means some of §27's numbers were luck.

1. **Cases were not isolated once `close()` sent a FIN.** `case_end()` closed
   the socket and gave it 40 ms; a FIN into a peer that has stopped listening
   retransmits for as long as the timer allows, so those frames turned up
   several cases later as *"wanted PA, got FA"* with a sequence number from
   another socket. Every case that leaves unacknowledged data behind (`x02`,
   `x03`, `x04`, `z01`) did the same with the data.
   `scripts/retransmit.drill` describes this problem in its own header and works
   around it per case with an injected RESET; `case_end()` now sets
   `SO_LINGER {on, 0}` before closing, which fixes it centrally and for every
   script.

2. **The harness queued traffic that no case is about.** The stack under test is
   a whole stack: anything in the tree that opens a UDP socket — mDNS, a DHCP
   renewal — puts frames on this wire, and they were queued like everything
   else. The next expectation then failed with `non-TCP frame ether=0x0800` and
   **every assertion after it was one frame out of step**. That is how `c04`,
   `c05` and `a01` failed against an unchanged stack in one run and passed in
   the next, and it is why `c07_passive_open` was failing at `bind()` in some
   runs and not others. `pump()` now drops IPv4 traffic that is not TCP to the
   peer, and counts it in the summary. A malformed segment, or one aimed at the
   peer, still reaches the queue — those are results.

3. **`non-TCP frame ether=0x0800` is not a diagnosis.** It cost an emulator boot
   to find out that the frame was mDNS. A rejected frame now reports its length,
   its IP protocol and its first 34 bytes, and a failing `bind()` reports errno.

Two directives were added for these cases. `txcount MIN MAX` discards what is
queued and asserts how much of it there was, because a retransmission series is
a count rather than a sequence and asserting ten `tx` lines would be asserting
the interval as well. `close within=MS` bounds `CloseSocket()` itself, measured
across the call rather than off the frame it produced — the one thing that must
never wait for a peer.

### 32.8 What it cost, measured on both paths

§24's figures are the ones at risk, because the fix is on the transmit path.
Clean `HEAD` against `HEAD` + these fixes, `tests/trace/run-trace.sh`, 1,048,576
bytes, **two passes each, alternating, with `AMINETXDUO_PERF=1` so the emulator
had the machine to itself** (the second lane commit 81b8f8d added — a throughput
number taken while two other emulators run is fiction):

| | `HEAD` p1 | fixes p1 | `HEAD` p2 | fixes p2 |
|---|---:|---:|---:|---:|
| loopback, not capturing | 351 | 349 | 351 | 350 |
| loopback, capturing | 309 | 307 | 309 | 307 |
| wire, capturing | 204 | 200 | 202 | 200 |
| wire, not capturing | 186 | 146 | 120 | 186 |

**Loopback is -0.6%, and it repeats to the kilobyte across four runs.** That is
what the mechanism predicts: `lo0` does not go through SANA-II at all, so the
only thing that could move it is the close path, and it does not. The capturing
wire arm is -1.5% and also stable.

**The non-capturing wire row is not a measurement and is printed to say so.**
Two runs of the *same* library in the same configuration came out at 186 and
120 KB/s — a 55% spread with nothing changed between them — so no reading of
that row distinguishes a 1% effect from a 30% one. It is SLIRP, and §24.4 saw
the same instability from the other side. The capturing arm is the one to read,
because it runs the same workload behind a bpf channel that paces it.

`retransmitted 0 segments` in our own direction on every flow in every arm,
unchanged.

The concurrency sweep, `tests/curl/run-curlverify.sh -p`: 9 passed, 0 failed,
`AvailMem` delta +0, `p04_parallel_40` green — which is the case §24.5 named as
the one this class of change is guarded against.

### 32.8.1 The wider cover, two arms, nothing moved

Clean `HEAD` and `HEAD` + these fixes, same harnesses, same machine:

| | clean `HEAD` | + these fixes |
|---|---|---|
| conformance, `LOOPBACK` | 130 passed, 0 failed, 12 skipped | **identical** |
| conformance, `HOST 10.0.2.2` | 141 passed, 1 failed | **identical** |
| `tests/clients` | 94 checks, 0 failures | **identical** |
| `tests/curl` A–F | 147 passed, 2 failed | **identical** |
| `tests/curl -p`, 8…48 | — | 9 passed, 0 failed |
| `tests/tools/run-livetools.sh` | 23 ok, harness `FAIL` | **identical** |

The conformance host failure is `accept(): incoming connection from remote host`
and the two curl failures are `a44_cookies_send` (the one §14.7 settled against
a third-party binary) and `f07_ftp_active`; all three are on both arms.

`run-livetools.sh` deserves a sentence of its own, because it is a false red and
somebody will hit it again. All 23 of its checks pass — the lease, the gateway,
the live counters, `ping`, `CheckNetConfig`, the three route commands and
`NetShutdown` — and the run then fails on

    FAIL: no serial log at build/serial-livetools.log -- cannot tell a reboot from a hang

with the serial log present and **zero bytes long**. Other runs in the same
sweep produced empty serial logs too (`serial-cfh3.log`) and others did not, so
it is FS-UAE not flushing the debug port rather than a guest that rebooted; a
clean-`HEAD` control run does exactly the same thing. The check is right to
exist — §25 is a section about a command that rebooted the machine — but "the
file is empty" and "the machine went away" are not the same event and the script
treats them as one.

### 32.9 A third defect, routed here because it lives in the same file

`ObtainSocket()` sets `as_Owner` to the base that took the socket, and
`as_Owner` is what a NetX Duo receive or disconnect callback `Signal()`s.
`bsd_socket_release()` decremented the reference count and **returned early when
another reference was still held** — leaving `as_Owner` pointing at a base that
is about to be freed. The next callback then reads a `struct Task` out of freed
memory and signals it, which on a machine with no memory protection is a write
into whatever now occupies the address.

Every inetd-style handoff takes that path: `ReleaseCopyOfSocket()` plus
`ObtainSocket()`, or `Dup2Socket()` across bases. §34 found it hanging the first
socket-handoff run of the `TCP:` handler, which guards the case locally; the
general fix is three lines in `bsd_socket_release()` and the local guard is now
redundant rather than load-bearing.

`NULL` is the right answer rather than "the other holder", because there is no
way to know which holder that is — one NX socket has one owner and `handoff.c`
says so. Events are still recorded in `as_Events`, so a poll sees them; only the
asynchronous wakeup is lost, and it is lost to a base that no longer exists.

### 32.10 The blocking `accept()` that did not return, diagnosed and not fixed

Reported alongside the above: a blocking `accept()` on an established connection
seen once, in one run of four, never to return. **It is not the same wakeup as
anything in §32.1 and it is not in `src/`.** It is a time-of-check race in the
vendored `nx_tcp_server_socket_accept.c`, and the two lines are next to each
other:

```c
    /* Check if the socket has already made a connection ... */
    if (socket_ptr -> nx_tcp_socket_state == NX_TCP_ESTABLISHED)
        return(NX_SUCCESS);                     /* <- read with NO mutex held */
    ...
    tx_mutex_get(&(ip_ptr -> nx_ip_protection), TX_WAIT_FOREVER);
    ...
    _nx_tcp_socket_thread_suspend(&(socket_ptr -> nx_tcp_socket_connect_suspended_thread), ...);
```

The early-out reads the state **before** taking the protection mutex, and there
is no second check after taking it. So:

1. `accept()` reads SYN_RECEIVED and carries on;
2. the IP thread processes the client's final ACK, moves the socket to
   ESTABLISHED, looks for `nx_tcp_socket_connect_suspended_thread` and finds
   nothing, because nobody has suspended yet;
3. `accept()` takes the mutex — skipping the LISTEN block, since the state is
   now ESTABLISHED — and suspends;
4. nothing will ever resume it.

Step 2 needs the IP thread to run between steps 1 and 3, which is exactly what
happens when it is *already holding* `nx_ip_protection` to process that ACK: our
thread blocks on the mutex, yields the baton, and the transition completes while
it waits. That is why it is intermittent and why it happens under load.

**Not fixed here, and the reason is the shape of the fix rather than its size.**
The obvious repair — slice the wait and call again — is unsafe: on a timeout
`_nx_tcp_server_socket_accept()` runs `_nx_tcp_connect_cleanup` and winds the
socket back to `NX_TCP_LISTEN_STATE`, so the next call re-enters the LISTEN
block and **sends a second SYN+ACK** on a half-open connection. Only two repairs
are actually safe, and both are a change of mechanism rather than a patch:

- call `nx_tcp_server_socket_accept()` with `NX_NO_WAIT` and do the waiting in
  `bsd_accept()` — no suspension means no cleanup and no state damage, at the
  cost of turning an event-driven accept into a polled one; or
- wait on the establish notification `bsd_events_attach()` already installs,
  which is event-driven but has to drop out of ThreadX context to park, the way
  `select.c` does.

The workaround both the handler and its test already use — `WaitSelect()` before
`accept()` — is sound, because it makes the socket ESTABLISHED before the
unlocked check runs and the suspension is never reached.

### 32.11 Two things noticed in the receive path, not changed

§29's Roadshow comparison reports the disagreement between our own `NetTrace`
(55% faster than Roadshow) and the stack-agnostic Aminet curl (12% slower), and
names the receive call pattern as the difference. Two things stand out in
`src/bsdsocket/` while reading it for this work, both reported rather than
touched:

- **Every call adopts and orphans a ThreadX thread.** `bsd_nx_enter()` /
  `bsd_nx_leave()` brackets each `recv()`, each `send()`, and *each poll pass
  inside `WaitSelect()`* — `netx_call.c` describes the cost as an
  `AllocSignal()`, a `_tx_thread_create()`, a baton acquire and their inverses.
  curl's pattern is many small `recv()` calls with a `select()` between them, so
  it pays the bracket twice per chunk where `NetTrace` pays it about once per
  4,096 bytes. That is a per-call constant that scales with the number of calls
  and not with the bytes, which fits "we win `time_connect` and lose everything
  after the first byte".
- **`bsd_poll_sets()` re-enters that bracket on every wakeup**, and walks
  `0..nfds` each time; a `WaitSelect()` that times out runs it twice.

Nothing in the copy path looks wasteful: `nx_packet_data_extract_offset()`
scatters straight into the caller's buffers, a partially drained packet is
parked on the socket rather than copied, and there is no bounce buffer anywhere.
§29.3's next experiment — a bpf capture of both clients comparing `recv()`
counts — would say directly whether the call count is the whole of it.


## 33. Nine more NetX Duo flags, weighed one at a time (2026-07-26)

§28 turned on three macros that had never been written down. This is the rest of
the survey: forty-six `NX_ENABLE_*` / `NX_DISABLE_*` symbols exist in the
vendored tree, sixteen were set, and these nine were the ones worth an
afternoon. **Four ship, four are rejected with the reason rather than with
silence, and one ships as half of what was asked for because the other half was
measured at 5% of loopback throughput.**

| | | |
|---|---|---|
| `NX_ENABLE_TCP_KEEPALIVE` | **on** | `setsockopt(SO_KEEPALIVE)` was answering yes and doing nothing |
| `NX_TCP_RETRY_SHIFT` / `NX_TCP_MAXIMUM_RETRIES` | **1 / 6** | the retransmit timer neither backed off nor had a ceiling |
| `NX_ENABLE_TCP_MSS_CHECK` | **on** | one comparison per incoming SYN |
| `NX_ENABLE_IP_ID_RANDOMIZATION` | **off**, seeded instead | measured: 5.2% of loopback, ~400 µs per datagram |
| `NX_ENABLE_LOW_WATERMARK` | off | §24.7's argument, unchanged; three changes must land together |
| `NX_DISABLE_ARP_AUTO_ENTRY` | off | it does not close the poisoning path it looks like it closes |
| `NX_ENABLE_ARP_MAC_CHANGE_NOTIFICATION` | off | a notification with nothing that could act on it |
| `NX_ENABLE_PACKET_DEBUG_INFO` | off | right idea, wrong lifetime — belongs behind a debug option |
| `NX_ENABLE_DUAL_PACKET_POOL` | off | §24.8 settled the one-pool question already |

### 33.1 Keepalive: the option that was already saying yes

`src/bsdsocket/options.c` accepted `SO_KEEPALIVE`, stored it in a socket flag
and reported it back through `getsockopt()`. Nothing acted on it, because
`NX_ENABLE_TCP_KEEPALIVE` was not defined and the whole of
`nx_tcp_periodic_processing.c`'s keepalive block was compiled out.

**An API that answers correctly with no implementation behind it is worse than
one that returns `ENOPROTOOPT`**, and it is the fourth defect of that exact
shape found in this tree in a week. A program that asks for keepalive and is
told yes has no way to discover that its half-open connections will never be
reaped.

**One thing had to change in our code, and it is not optional.**
`nx_tcp_socket_create.c:166` sets `nx_tcp_socket_keepalive_enabled = NX_TRUE`
**unconditionally** under this define. Turning it on alone would have put every
socket in the machine on a two-hour keepalive timer whether anything asked or
not — which is not what `SO_KEEPALIVE` means and not what 4.4BSD, POSIX or any
other stack does. `src/bsdsocket/socket.c` now clears it at create, next to the
ISN seed, and `options.c` is the only thing that sets it.

The NetX Duo defaults are BSD's and are left alone: 7200 s idle, 75 s retry, ten
retries.

#### Proved on the wire

A two-hour idle timer cannot be observed inside an emulator run, so
`AMINETXDUO_TCP_KEEPALIVE_INITIAL` exists to build an arm with a five-second one
— `nx_tcp.h` guards the macro with `#ifndef`, so it reaches it without
`nx_user.h` holding a number nobody ships. `tests/tcpdrill/scripts/keepalive.drill`
is four cases against that arm, and a keepalive probe is unmistakable on the
wire: it is an ACK carrying `tx_sequence - 1`, a deliberately backward sequence
number the peer is obliged to answer (RFC 1122 4.2.3.6).

```
---- k01_keepalive_off_by_default
  ok   notx 10000                                   twice the idle timer, silent
---- k02_keepalive_probes_when_asked
  ok   opt keepalive 1
  ok   tx A seq=0 ack=1 after=4000 within=8000   [+5005ms]
---- k03_probe_answered_restarts_the_timer
  ok   tx A seq=0 ack=1 after=4000 within=8000   [+4870ms]
  ok   rx A seq=1 ack=1
  ok   tx A seq=0 ack=1 after=4000 within=8000   [+4987ms]
---- k04_keepalive_can_be_turned_off
  ok   opt keepalive 0
  ok   notx 10000
4 case(s), 0 failed; 34 check(s) passed, 0 failed
```

k01 is the case that matters most and is the one that would not have been
written if the vendored default had not been read: it asserts that a socket
which never asked stays silent for twice the idle timer.

### 33.2 The retransmission timer: backoff, and the ceiling NetX Duo does not have

§27 measured SYN retransmissions at **890 ms and then 1002 ms** — flat, forever.
`NX_TCP_RETRY_SHIFT` defaults to 0, so the shift in

```c
timeout = nx_tcp_socket_timeout_rate << (timeout_retries * timeout_shift);
```

is a no-op and the interval is `NX_IP_PERIODIC_RATE / NX_TCP_TRANSMIT_TIMER_RATE`
— one second. There is no RTT estimator anywhere in the vendored tree either, so
the interval was not merely constant but constant at a number nobody chose for
this path.

**`NX_TCP_RETRY_SHIFT 1` on its own is a trap, and the trap is that NetX Duo has
no maximum RTO.** The expression above is a plain shift with no clamp;
`NX_TCP_MAXIMUM_RETRIES` is the only thing bounding it. At its default of ten,
doubling gives intervals of 1, 2, 4 … 1024 seconds and the socket does not give
up for 2^11 − 1 = **2047 seconds**. A one-second stall would have become a
thirty-four-minute one.

So the two are set together. Six retries with a shift of one gives

```
1  2  4  8  16  32  64 seconds        abandon at 127 s
```

and each of those three numbers is defensible rather than round:

* the largest single interval is **64 s**, against RFC 6298 §2.5's rule that a
  maximum RTO "MAY be placed provided it is at least 60 seconds";
* the connection is abandoned at **127 s**, against RFC 1122 §4.2.3.5's R2 of
  "at least 100 seconds" for data;
* and it is **12.7× the 10 s** this stack gave up after before, not 200×.

The last is the one that would otherwise bite: the same counter bounds SYN
retransmission, so a `connect()` to a host that is not answering now blocks for
127 s rather than 10 before the stack resets the socket. That is what TCP is
supposed to do and what every other stack does, but it is a visible behaviour
change and it is why the number is 6 and not 8.

#### Proved on the wire

`tests/tcpdrill/scripts/retransmit.drill`, against the shipping flags:

| | measured |
|---|---|
| SYN, four retransmissions | **1185, 1984, 4008, 7997 ms** |
| data, three retransmissions | **915, 2004, 4008 ms** |
| first interval, on its own | **988 ms** |

Doubling, from one second, on both paths. Three cases, twenty-four checks, none
failed. What it was before this change is in §27's own transcript: 890 then 1002.

#### The side effect, which cost two runs to understand

**Backoff makes sockets live longer, and that broke twelve of tcpdrill's
twenty-one cases.** `case_end()` closes the socket without completing the
exchange, so the FIN it sends is never acknowledged and is retransmitted for as
long as the timer allows. With a flat one-second timer and ten retries that was
ten seconds of noise which mostly fell between cases. With 1-2-4-8-16-32-64 it
is 127 seconds, and every later case saw an earlier one's FIN arrive in the
middle of its own expectation — `wanted PA, got FA`, with a sequence number from
another socket entirely.

That is a harness isolation problem rather than a stack defect, and it is not
fixed here. The two scripts written for this section end **every** case with a
RESET from the peer, which tears the socket down in any state and leaves nothing
retransmitting, so each case measures the timer without depending on how the
previous one finished.

**And a second source of noise, from the same afternoon, is worth recording
because it looked identical.** The mDNS responder (§30) landed while these
measurements were being taken, and its multicast announcements are non-TCP IPv4
frames that tcpdrill's strict matcher fails on — `non-TCP frame ether=0x0800`,
six of twenty-one cases, in a run that had nothing to do with mDNS. The arms
here are built with `-DAMINETXDUO_MDNS=OFF` for that reason. Both of these are
worth a look from whoever owns the harness: strict "nothing else was on the
wire" is the right default for a conformance test and needs a way to say "except
this".

### 33.3 The MSS check: one comparison, and a peer that cannot lie

Without `NX_ENABLE_TCP_MSS_CHECK`, `nx_tcp_packet_process.c` takes whatever MSS a
peer's SYN carries. A peer advertising 1 makes every segment we send one byte of
payload behind forty bytes of header, from a socket the application believes is
working; `NX_TCP_MAXIMUM_TX_QUEUE` (8) then bounds the connection at eight bytes
in flight. **That is a denial of service that costs the other end one packet, and
nothing in a trace would look like an error.**

With it, a SYN whose MSS is below `NX_TCP_MSS_MINIMUM` (128, the default, kept)
is answered with a RESET and counted in `nx_ip_tcp_invalid_packets`. A peer that
offers **no** MSS option is unaffected — the code substitutes the
interface-derived default before this check runs — so nothing conformant is
refused.

**It is not demonstrated on the wire here, and that is stated rather than
implied.** The check is on the passive-open path only (the branch that handles a
connection request), and tcpdrill's `c07_passive_open` fails in the current tree
for an unrelated reason — `bind()` on the listen port — so there is no case that
can drive a SYN into it yet. What is shipped is a compiled-in comparison whose
effect is legible in ten lines of vendored source; when passive open works, one
case with `mss=1` will settle it.

### 33.4 The IP identification field: 5% of loopback, and the free half

Without `NX_ENABLE_IP_ID_RANDOMIZATION`, `nx_ip_header_add.c:151` uses
`ip_ptr -> nx_ip_packet_id++`: a global counter that `nx_ip_create()` zeroes and
that increments once per transmitted datagram. Two consequences, of very
different sizes:

1. **it is a fingerprint** — monotonic, boot-zeroed, machine-wide, and the rate
   it climbs at is a packet counter for the whole machine readable from any one
   flow;
2. **it is RFC 6274 §5.1's idle scan** — an off-path attacker who can send to
   this machine and read the ID it answers with learns how many packets it sent
   in between, which is how a host is used as a zombie to scan a third party.

The define fixes both. Two arms out of one tree, A1200, 524,288 bytes:

| | counter | randomised | |
|---|---:|---:|---:|
| **loopback**, no capture | 347 KB/s | 329 KB/s | **−5.2%** |
| loopback, capturing | 305 KB/s | 290 KB/s | −4.9% |
| **wire**, capturing | 171 KB/s | 167 KB/s | −2.3% |

**The mechanism is not in doubt, because the two paths differ by exactly the
ratio of datagrams they send.** Loopback puts about 130 datagrams a second on
the wire and the wire path about 70 — we are the receiver there and send mostly
ACKs — and 5.2/2.3 is that ratio. It works out at roughly **400 µs per
transmitted datagram**.

That is `NX_RAND`, which `nx_port.h` maps to `ami_random_rand()`: a SHA-256 hash
DRBG with a `Forbid()`/`Permit()` pair per draw and a SHA-256 pair per 32 bytes
of output, so one refill every eight calls. It is the right generator for what
it was chosen for — TLS key material, ECDHE privates, TCP sequence numbers — and
much too expensive to spend on a 16-bit header field once per packet. NetX Duo
offers no way to pick a cheaper source for this one field: it is the same
`NX_RAND` macro everywhere.

**What ships instead is the half that is free.** `src/netstack/` seeds
`nx_ip_packet_id` from the DRBG **once**, when the `NX_IP` is created. One draw
at startup, nothing per packet, and it removes (1): the counter no longer starts
at zero, so the absolute value says nothing about uptime or about how much this
machine has sent. **It does not remove (2)** — idle scan reads the delta between
two observations, not the value — and saying so is the point, because the cheap
half looks like a complete answer and is not.

`-DAMINETXDUO_IP_ID_RANDOMIZATION=ON` pays the 5% and closes the second one. On
a network where an idle scan is a real threat that is a good trade; on the
14 MHz floor target this stack is built for, it is not the default.

### 33.5 The four that were rejected, and what would change each answer

The full reasoning is in `port/netxduo-amiga/inc/nx_user.h`, where it will be
read by whoever wonders why the flag is not set. In brief:

**`NX_DISABLE_ARP_AUTO_ENTRY`** looks like the safe choice and is not, for a
reason specific to the mechanism rather than to the idea. Today any ARP we see
for a sender we have **no entry for** creates one. Disabling that does **not**
close the poisoning path that matters: `nx_arp_packet_receive.c` updates an
**existing** entry from any ARP on the wire whether this is defined or not, and
this define does not touch that code. What it does remove is the free entry for
the gateway, so the next packet to an unresolved next hop costs an ARP request,
a packet out of `NX_ARP_MAX_QUEUE_DEPTH` (2, deliberately small) and a round
trip. Security nothing, latency something. What would change it: an ARP cache
that distinguishes "learned from a request addressed to us" from "learned from
anything on the wire".

**`NX_ENABLE_ARP_MAC_CHANGE_NOTIFICATION`** is a notification and nothing else:
`nx_arp_packet_receive.c:418` calls it **after** writing the new address, so
nothing it does can refuse the change, and there is no handler in this tree that
would act on it. It becomes worth having the day something can act — a static
ARP pin for the gateway, or a warning surfaced in `ShowNetStatus` — and not
before.

**`NX_ENABLE_PACKET_DEBUG_INFO`** is not rejected on merit. It records the file
and line each packet was allocated at, which is directly aimed at the
packet-ownership defects this project keeps finding. It is rejected as a
**permanent** setting: two pointers in every one of up to 256 pool packets, on a
machine whose pool is sized from `AvailMem()`. It belongs behind a build option
next to the debug log level, and that option does not exist yet.

**`NX_ENABLE_DUAL_PACKET_POOL`** has the right premise and the wrong shape for
this machine. §24.8 already settled that there is one pool here on purpose: a
second pool takes memory permanently away from the 4 MB floor to guard against
an exhaustion that §24.3's arithmetic is what actually prevents. It is also the
wrong half of the problem — an ACK that cannot be allocated is a symptom of a
data pool already empty, and the data is what was lost.

**`NX_ENABLE_LOW_WATERMARK`** is unchanged from §24.7, which found it and
reported it rather than switching it on. It still needs three things together:
the define, an `nx_packet_pool_low_watermark_set()` call from `src/netstack/`
(because `nx_packet_pool_create()` never touches the field, so a zeroed
watermark means the guard is compiled in and can never fire), and
`NX_TCP_MAXIMUM_RX_QUEUE` raised, because at its default of 20 and 1440-byte
segments it binds at about 28 KB — **before** a 32 KB window does — and the
tail-drop it would then perform costs a retransmission this stack has no SACK to
recover cheaply. It also changes IPv4 fragment reassembly and UDP receive. That
is a piece of work with its own measurement.

### 33.6 Regression cover

Everything §28.5 measured, re-measured with all four changes in.

| | §28.5 | here |
|---|---|---|
| conformance, loopback tier | 130 passed, 0 failed, 12 skipped | **130 passed, 0 failed, 12 skipped** |
| conformance, network tier | 141 passed, 1 failed, 0 skipped | **141 passed, 1 failed, 0 skipped** |
| `tests/clients` | 94 checks, 0 failures | **94 checks, 0 failures** |
| `tests/curl` groups A–F | 147 passed, 2 failed, 149 cases | **147 passed, 2 failed, 149 cases** |
| `tests/curl` concurrency sweep | 9 passed, 0 failed | **9 passed, 0 failed**, `AvailMem` delta +0 |
| `tests/tools/run-routes.sh` | PASSED | **PASSED** |
| `tests/tools/run-dnscache.sh` | PASSED | **PASSED** |
| `tests/tcpdrill` retransmit / keepalive | — | **3/3 and 4/4, 58 checks, 0 failed** |
| `tools/ci.sh` | all green | **all green** |

**One scare, chased down rather than explained away.** An intermediate run of
`tests/clients` reported 94 checks with **2 failures** — `send() to a closed peer
eventually fails`, sixteen sends and none of them refused. That is exactly the
shape a longer retransmission timer would produce, and exactly the shape the
`CloseSocket()`-sends-a-FIN change landing the same hour would produce, so it
was worth an arm rather than an argument: `tests/clients` was re-run against the
shipping build **and** against one built from the same tree with
`-DNX_TCP_RETRY_SHIFT=0 -DNX_TCP_MAXIMUM_RETRIES=10`. Both came back **94 checks,
0 failures**. It was transient, it was not the timer, and the two macros are
`#ifndef`-guarded so that arm can be built again in one command the next time
somebody needs to ask.

## 34. `TCP:` — a socket that AmigaDOS commands can open (2026-07-26)

§30.7 scoped a telnet server and stopped at the wall in front of it: AmigaOS has no
socket-as-file-handle, so a descriptor cannot be handed to `SystemTagList()`, `Open()` or
`Read()`. The mechanism that gets through that wall is a DOS handler, and the half of it
this project already had was the other one — `ObtainSocket()`, `ReleaseSocket()` and
`ReleaseCopyOfSocket()` in `src/bsdsocket/handoff.c` at LVOs −0x090/−0x096/−0x09c.

The handler is now built: `src/bsdsocket/tcp_handler.c`, published as `TCP:` from the first
`OpenLibrary("bsdsocket.library")` onwards. Everything below is from a run.

(Numbered 34 because §27 appears twice in this document and 28-33 are all taken; this
document is written by several hands at once and the numbers are claimed, not sorted.)

### 34.1 What it is worth, in the only currency that counts

The claim is that a socket becomes an *ordinary* file handle. The only way to show that is
to give one to a program that has never heard of a network, so the run uses **Commodore's
own `Type` and `Copy`** — the 1496- and 5580-byte binaries out of the AmigaOS 3.1 `C:`
drawer, unmodified — and the Shell's own `>` redirection, which is `dos.library` and
nothing else. `tests/tools/run-tcphandler.sh`, on an A1200 with the A2065 on SLIRP:

```
===== SYS:Type TCP:10.0.2.2/amitest =====
AmiNetXDuo daytime, line one
and line two
----- rc 0 -----

===== SYS:Copy TCP:10.0.2.2/amitest TO DH0:copied.txt =====
----- rc 0 -----

===== SYS:Type DH0:copied.txt =====
AmiNetXDuo daytime, line one
and line two
----- rc 0 -----

===== Echo >TCP:10.0.2.2/7001 "AmigaDOS redirection reached the socket" =====
----- rc 0 -----
```

`DH0:copied.txt` is compared byte for byte against what the server sent, CRLFs and all, and
matches. What `Echo` wrote is read out of the **host's** log rather than ours:

```
[ 131.23] echo   connection from 127.0.0.1:62164
[ 131.59] echo   got 40 bytes: b'AmigaDOS redirection reached the socket\n'
```

`amitest` is a *service name*, resolved out of `DEVS:Internet/services`, so the name path
is in the picture too — deliberately not a well-known one, because a run that passed
because port 13 happened to be open somewhere would prove nothing about `getservbyname()`.

The other half of the syntax — no host, meaning "wait for somebody" — needs no host at all,
because both ends can be `TCP:` handles held by stock commands:

```
===== &SYS:Type TCP:2400 >DH0:listened.txt =====
===== Echo >TCP:localhost/2400 "a listening TCP: handle received this" =====
----- rc 0 -----

===== SYS:Type DH0:listened.txt =====
a listening TCP: handle received this
----- rc 0 -----
```

which the handler's own log shows as two sessions and two sockets:

```
[INFO] TCP: open 'TCP:2400'
[INFO] TCP: open 'TCP:localhost/2400'
[INFO] TCP: 'TCP:2400' is socket 1
[INFO] TCP: 'TCP:localhost/2400' is socket 0
```

And the sequence §30.7 actually wanted, run end to end by `tests/tools/tcphandoff.c`:

```
listening on 2300
the peer command is running
WaitSelect on the listener returned 1
accepted a connection
parked under id 65536
TCP:OBTAIN= opened
the handed-over command returned 0
the file handle is closed
the accepted socket is closed
```

That is `accept()` → `ReleaseCopyOfSocket(fd, UNIQUE_ID)` → `Open("TCP:OBTAIN=65536")` →
`SystemTagList("Echo …", SYS_Output = that handle)`, with `Copy TCP:localhost/2300 TO
DH0:handoff.txt` at the far end. Two stock AmigaDOS commands end up talking to each other
over a socket that neither of them opened, and `DH0:handoff.txt` holds what the handed-over
command wrote. Nothing in `TcpHandoff` is linked against our code; it reaches the library
through its published LVOs.

### 34.2 The syntax is Roadshow's, and the parse is not quite its template

`Documentation/Reference/tcp-handler.doc` in the Roadshow demo gives:

```
Open("TCP:[HOST]=<name or address>]/[PORT=<port number>]",...)
Open("TCP:OBTAIN=<number>",...)
TEMPLATE  H=HOST,P=PORT=S=SERVICE/K,O=OBTAIN/K/N
```

with two examples that its own template cannot both satisfy: `Type TCP:localhost/daytime`
puts a bare word where `PORT/K` says a keyword is required, and `TCP:<service name>` is
documented as equivalent to `TCP:service=<service name>` even though `HOST` is the first
positional. So the template is not what the handler really does, and the parse implemented
here is the one that satisfies both examples: components are split on `/`, a component
containing `=` is a keyword (`H`/`HOST`, `P`/`PORT`/`S`/`SERVICE`, `O`/`OBTAIN`), and bare
components go **service first, host second** — one bare word is a service (a listener), two
are host then service (a connection). Case-insensitive. Everything else is a name error.

`OBTAIN` ignores every other parameter, as documented.

### 34.3 One process per connection, and why not one process with `WaitSelect()`

A handler answers a `DOSPACKET` when it can and not before, and the sender is asleep in
`DoPkt()` meanwhile. A single-process handler therefore queues every packet it cannot
answer yet and drives them all from one `WaitSelect()`. That works — for the packets. It
does nothing for the two things that genuinely block and are not sockets:
`gethostbyname()` and `connect()`. One name lookup would stall every other file handle the
handler owns, and there is no way to make a DNS query selectable here.

So `TCP:` is shaped like `con-handler`: a **control process** owns the device node and
answers `FINDINPUT`/`FINDOUTPUT`/`FINDUPDATE` by starting a **session process**, handing it
the packet, and forgetting about it. The session opens its own `bsdsocket.library` — the
only correct way to get a descriptor table, because every opener gets its own child base
(§3.1) — connects, points the `FileHandle`'s `fh_Type` at its own port, and replies. Every
later packet for that handle goes straight to the session, which may block for as long as
it likes because nobody else is behind it.

**Moving `fh_Type` is legal and load-bearing.** `dos.library` sets it to the device's port
just before sending the packet and re-initialises the whole file handle on every retry "in
case handler played with it" (v40 `dos/bcplio.c`, `findstream`). One port per open file is
also the *only* way to implement `ACTION_WAIT_CHAR`, which is on Roadshow's packet list:
Commodore's own source says so in a comment above the implementation —

```
/* DOESN'T pass fh_Arg1! - no error reporting! */
waitforchar (REG(d1) BPTR scb, REG(d2) LONG timeout)
{
        port = ((struct FileHandle *) BADDR(scb))->fh_Type;
        return sendpkt1(port,ACTION_WAIT_CHAR,timeout);
}
```

— the packet carries a timeout and nothing else. A handler serving many files from one port
literally cannot tell which file `WaitForChar()` is asking about. With a port per file the
question is unambiguous, and the answer is one `WaitSelect()` on one descriptor with one
timeout, which is what `WaitSelect()` is for.

**Neither process uses `pr_MsgPort` for DOS packets.** Both make their own DOS calls —
`CreateNewProc()` duplicates the parent's current directory, the resolver reads
`DEVS:Internet` — and `dos.library`'s `DoPkt()` replies land on `pr_MsgPort`. Sharing the
two would put a reply and an incoming packet in the same queue and require a `pr_PktWait`
hook to tell them apart. A second `MsgPort` costs nothing and deletes the problem. The one
exception is deliberate: the control process forwards the `FIND` packet to the new session
by `PutMsg`-ing it to the session's `pr_MsgPort`, and that is the only message that port
ever receives — the session takes it before it makes any DOS call of its own.

### 34.4 The packet list, established rather than assumed

§30.7 guessed nine from the Roadshow document. The handler logs every packet type it does
not recognise, and the run says the real list, for `Type`, `Copy`, the Shell and
`SystemTagList()`, is:

| packet | who sends it | answer |
|---|---|---|
| `ACTION_FINDINPUT` / `FINDOUTPUT` / `FINDUPDATE` | `Open()` | start a session |
| `ACTION_READ` / `ACTION_WRITE` | `Read()`, `Write()`, `FGetC()` | the socket |
| `ACTION_END` | `Close()` | close and exit |
| `ACTION_IS_FILESYSTEM` | `MatchFirst()`, `Copy` | `DOSFALSE`, **`dp_Res2 = 0`** |
| `ACTION_LOCATE_OBJECT` | `Copy`, locking its source | refuse |
| `ACTION_WAIT_CHAR` | `WaitForChar()` | `WaitSelect()` |
| `ACTION_SEEK` | anything that measures a file | fail, `ERROR_SEEK_ERROR` |
| `ACTION_DISK_INFO` | `Info()` | a large, valid, imaginary disk |
| `ACTION_FLUSH`, `ACTION_CHANGE_SIGNAL` | DOS housekeeping | `DOSTRUE` |
| `ACTION_DIE` | whoever wants `TCP:` gone | remove the node and exit |

`ACTION_STACK`, which the Roadshow document lists, has no constant in the NDK, is not in
`dos/dosextens.h` at all, and nothing sent it in any run. It is not implemented.

**`ACTION_IS_FILESYSTEM` returning `DOSFALSE` with `dp_Res2` set to zero is the single
answer that makes `Type TCP:…` work**, and it took reading `dos.library` to see why.
`MatchFirst()` goes through `FunkyMatchFirst()` (v40 `dos/patternhack.c`):

```
if (mystricmp(pat,"*") == SAME || … ||
    (strchr(pat,':') && !isfilesystem(pat)))
{
        res = getresult2();
        if (res && res != ERROR_ACTION_NOT_KNOWN)
                return res;
        …
        strcpy(anchor->ap_Info.fib_FileName,pat);
```

A path with a colon on a device that is not a filesystem is returned *verbatim*, with no
`Lock()` and no `Examine()`. That is why a handler with no directory structure at all can
be the argument to a command whose template is `FROM/A/M`. Answer `IS_FILESYSTEM` with an
error in `dp_Res2` and `Type` reports that error instead of opening anything. `Copy` reaches
the same conclusion by a different route — it calls `IsFileSystem()` on both its source and
its destination explicitly (v40 `copy.c`) — which is why `Copy TCP:… TO …` also works, and
why the `Lock()` it does on its source can simply be refused.

### 34.5 What a program that only knows `Read()` sees

| what happened | what `Read()` does |
|---|---|
| peer closed, all data delivered | returns 0 — ordinary EOF |
| connection reset | returns −1, `IoErr()` set |
| nothing to read yet | blocks; there is no idle timeout |
| name lookup or `connect()` failed | never happens — `Open()` failed |

A reset is **not** reported as EOF on purpose. EOF would turn a truncated transfer into a
successful `Copy` with a short file and a return code of 0, which is the one outcome nobody
can act on. AmigaDOS has no error code for "connection reset by peer", so the mapping table
in `tcp_handler.c` says what each errno is being reported *as* rather than pretending to be
a translation, and the real errno goes to the serial log next to it.

Two of those choices were made by reading `Fault()`'s own table (v40 `dos/fault.c`).
`ERROR_BAD_STREAM_NAME` (206) is semantically exactly right for a malformed `TCP:` name —
and its text is **"invalid window description"**, because 206 is what `CON:` returns for a
bad window spec. `Type TCP:` printed that, and it is useless. Name errors are therefore
reported as `ERROR_OBJECT_NOT_FOUND` (205), whose text is "object not found":

```
===== SYS:Type TCP:10.0.2.2/nosuchservice =====
TYPE can't open TCP:10.0.2.2/nosuchservice
object not found
----- rc 10 -----
```

The reason is on the serial line for whoever is debugging rather than using:

```
[WARN] TCP: no service 'nosuchservice'
[WARN] TCP: cannot parse 'TCP:'
```

### 34.6 Two defects this turned up in code that was already here

**A base can be freed while a socket it owns is still referenced.** `ObtainSocket()` sets
`as_Owner` to the obtaining base, which is right — that is the task NetX Duo's callbacks
must signal. But a socket taken through `TCP:OBTAIN=` outlives the session that took it:
the program that released a *copy* still holds the original descriptor. When the session
closes its library, the base is freed and `as_Owner` is left pointing into freed memory, so
the next receive or disconnect callback does `Signal()` on a task read out of it. The first
run of the hand-off hung exactly there, after `Copy` had already been fed and had written
its file.

The local half of the fix is in `tcp_handler.c`: the session reads the reference count
*before* the close (after it, the block may be gone), and if the socket survives with
`as_Owner` still pointing at the base about to be freed, it clears it — which is precisely
what `handoff.c` already does to a *parked* socket, and `bsd_event_post()` already treats a
NULL owner as "there is no task to wake". **The general fix belongs in
`bsd_socket_release()`** and is not made here: `src/bsdsocket/socket.c` is under active
change for the retransmission work. Any inetd-style handoff has this hazard, not just
`TCP:`.

**A blocking `accept()` was observed not to return.** In the second of four runs, `accept()`
on a listening descriptor never woke although the peer's `Open()` had plainly succeeded —
the connection was established and `Copy` was sitting on it, and the first run had gone all
the way through the same code. Replacing the blocking wait with `WaitSelect()` on the
listener followed by an `accept()` that cannot block made it reproducible-good;
`WaitSelect` returned 1 promptly in both runs since, and the listener half of §34.1 passes
through the same path. This is not
something the handler introduced; it is the `bsd_accept()` path, and it is written down
here because "wait for readiness, then take it in a call that cannot block" is now the
shape used in both `tcp_handler.c` and `tests/tools/tcphandoff.c`, and the underlying
wakeup should be looked at on its own.

### 34.7 Lifetime, and the one thing the library will not do any more

`TCP:` is published from the first `OpenLibrary()` rather than from `bsd_lib_init()`,
because that runs in the opener's own Process (`CreateNewProc()` wants one) — and it has to
be at open time rather than at first use, because DOS must find the device node before it
can route an `Open("TCP:…")` and `Type` does not open `bsdsocket.library`. Roadshow states
the same rule: the device appears when the library is initialised. In practice the library
is already resident and referenced by then, because `AddNetInterface` deliberately leaks an
`OpenLibrary()` reference to keep the stack up (§22).

A session holds an `OpenLibrary()` reference only while a file handle is open, so an idle
handler does not pin the netstack. The consequence is an invariant worth stating: **open
count zero implies the handler is idle**, which is why `ACTION_DIE` can never arrive while a
session is running.

`bsd_lib_expunge()` now **declines** while the handler process exists. Its code lives in the
segment expunge is about to hand back for `UnLoadSeg()`, it holds no open count, and there
is no way to prove it is not executing. `ACTION_DIE` is the supported way to take `TCP:`
down — it removes the DOS entry, replies inside `Forbid()` so the caller cannot free the
segment out from under the last few instructions, and exits — and after it, expunge
succeeds normally.

### 34.8 What is not there

* **No `UDP:`.** Roadshow has none either; a datagram is not a stream and `Read()` has no
  way to say where one ended.
* **Name resolution is synchronous inside the session.** A slow DNS server delays that one
  `Open()` and nothing else, which is the whole point of a process per connection, but it
  is still a blocking call and there is no timeout keyword to shorten it. Roadshow's
  template has none either, so none was invented.
* **A blocked `Read()` cannot be interrupted.** Ctrl-C goes to the Shell, which is asleep
  in `DoPkt()`; the handler cannot see it. `PIPE:` behaves the same way.
* **One connection per listening handle.** `TCP:<service>` accepts one and closes the
  listener. A file handle is one stream and there is nowhere to put a second.
* **No IPv6 literal syntax.** `TCP:host/service` resolves through `gethostbyname()`, which
  is v4; a `[::1]`-style literal has no place in a name whose separator is `/`.

### 34.9 Where this leaves the two things that wanted it

The telnet server from §30.7 is now the small half of itself: `accept()`, a password, and
`SystemTagList("", SYS_Input, fh1, SYS_Output, fh2, SYS_Asynch, TRUE)` with handles made by
`TCP:OBTAIN=`. The security recommendations in §30.7 are unchanged and none of them got
cheaper. Note the one wrinkle a shell will hit that `Echo` did not: `SYS_Input` and
`SYS_Output` must be *different* file handles (`dos.library` says so explicitly), so a
server needs **two** `ReleaseCopyOfSocket()` ids for the one connection — and the second
`ObtainSocket()` will move `as_Owner` to the second session, which is the same ownership
question §34.6 is about and should be settled before, not during.

Dropbear needs the identical bridge and should use this one. Nothing about it is
telnet-specific and nothing about it is ours-only: it is `Open()`, `Read()`, `Write()`,
`Close()`.

## 35. An SSH handshake was 97% arithmetic, and 84 seconds became 12 (2026-07-26)

§31 left `ssh` from a 14 MHz 68020 working and slow: **84.07 s** for one
connection to a stock OpenSSH 10.2, with the optimistic kex guess already
disabled, no session resumption in the protocol to soften a second connection,
and OpenSSH's default `LoginGraceTime` of 120 s only 43% away. It also left one
question unanswered — is P-256 with our assembly faster on this part than
portable-C curve25519 — and one assumption unchecked: that the whole cost is
public-key arithmetic.

Both are now measured. **The same connection, same server, same command line, is
12.28 s.** The cipher is unchanged, the protocol is unchanged, nothing in
`third_party/dropbear` is patched, and the client still negotiates
`curve25519-sha256` / `ssh-ed25519` / `chacha20-poly1305@openssh.com` with a
server that was given no compatibility settings.

### 35.1 The profile, taken from inside the guest

§31.5 tried to split the handshake by timestamping the server's log host-side
and got a split that contradicted itself, and drew the right conclusion from
that: **trust the guest's own clock**. `clients/dropbear/dbprofile.c` is that
conclusion applied one level down. It attaches with `-Wl,--wrap` — the mechanism
`clients/dropbear/build.sh` already uses for `open`/`read`/`write`/`close`, so
the submodule stays byte-identical to its tag — and times each primitive on
`ReadEClock()` inside the process that is doing the handshake.

One real connection, authenticated with a real key, running a real command:

| primitive | calls | ms | % of wall |
|---|---:|---:|---:|
| curve25519 scalar multiply | 2 | 23,374 | 27% |
| ed25519 sign (client auth) | 1 | 19,739 | 23% |
| **ed25519 verify (host key)** | 1 | **39,402** | **46%** |
| `sha512_process` *(nested in the two above)* | 9 | 13 | 0% |
| `sha256_process` | 33 | 5 | 0% |
| `chacha_crypt` | 80 | 74 | 0% |
| `poly1305_process` | 22 | 24 | 0% |
| `select()` — waiting for the network | 42 | 1,462 | 1.7% |
| **public-key subtotal** | | **82,620** | **97%** |
| whole process | | 84,517 | 100% |

**Nothing unnamed is hiding in it.** §31 asserted that the entire wall clock was
public-key arithmetic on the strength of the cipher A/B; this is that assertion
measured, and it holds at 97%. The remaining 3% is 1.7 s of network wait and
about 0.4 s of everything else in an SSH client.

The `select()` row was not a formality. §29 has just shown that two instruments
can disagree by 67% on the same wire because of the receive call pattern, so an
instrument that could only see crypto would have been unable to tell "the
arithmetic is everything" from "the arithmetic is everything the instrument can
see". Dropbear's read pattern is neither `NetTrace`'s nor curl's, and it spends
1.7% of a login in `select()`.

**The single largest row is the one nobody would have guessed.** Not the key
exchange — the *host key signature check*. `ed25519 verify` alone is 39.4 s,
more than both curve25519 scalar multiplications together. That is the fourth
wrong prediction this project has recorded, and it is wrong in a useful
direction: verification is the one operation a client cannot avoid, cache or
defer.

### 35.2 The other half of the cost model, counted on the build host

A wall clock cannot say how much work a primitive did.
`clients/dropbear/tweetnacl-count.sh` derives a counting copy of
`third_party/dropbear/src/curve25519.c` into `build/` — the two field routines
renamed, counting macros of the original names inserted below them — and runs it
natively. 2^255−19 arithmetic executes the same multiplies on any machine, so
this needs no emulator slot, and the queue is the scarcest resource here.

| primitive | mul | sqr | total field multiplies |
|---|---:|---:|---:|
| curve25519 keygen | 1,783 | 1,274 | **3,057** |
| curve25519 shared secret | 1,783 | 1,274 | **3,057** |
| ed25519 sign | 4,863 | 254 | **5,117** |
| ed25519 verify | 9,741 | 510 | **10,251** |
| **one handshake** | | | **21,482** |

All four RFC 7748 / RFC 8032 vectors pass in the same run, so the counts are of
code that is doing the right thing.

Divide: **82,620 ms over 21,482 multiplies is 3.85 ms each, about 54,600 cycles
at 14 MHz.** That is the number the whole section turns on, because the useful
content of a 2^255−19 field multiply on a 32-bit machine is sixty-four
multiplies.

### 35.3 Why it costs 54,600 cycles, which is the actual finding

Dropbear's 25519 is **TweetNaCl** — the smallest correct implementation in
existence, 100 tweets, and never intended to be the fastest. Its field element
is:

```c
typedef long long i64;
typedef i64 gf[16];          /* sixteen 16-BIT limbs, in 64-BIT slots */

sv M(gf o,const gf a,const gf b)
{
  i64 i,j,t[31];
  FOR(i,31) t[i]=0;
  FOR(i,16) FOR(j,16) t[i+j]+=a[i]*b[j];
  ...
```

256 iterations, and every one of them is a **software 64×64 multiply** — `a[i]`
and `b[j]` are `long long`, so GCC cannot know the operands fit in 32 bits and
emits the full expansion — plus a 64-bit load, a 64-bit accumulate and a 64-bit
store. 54,600 cycles over 256 iterations is 213 cycles each, which is exactly
what that costs.

The 68020 has `MULU.L Dn,Dh:Dl`: 32×32→64 in **one instruction**, measured at
32.06 cycles by `tests/perf/cpucal`. The whole of §35.4 is the observation that
16-bit limbs in 64-bit slots are the wrong shape for a machine with that
instruction, and that this is a question about the **representation** and not
about instruction selection — the same distinction §11.6 drew for the RSA limb
loop and §18.4 drew for SHA-256.

### 35.4 `src/crypto68k/c68k_25519.c`: eight 32-bit limbs

The same mathematics over `uint32_t[8]`, which is the shape `c68k_p256.c`
already uses. A field multiply is 64 `MULU.L` in the schoolbook plus 8 in the
reduction; reduction is by folding, because 2^256 ≡ 38 (mod 2^255−19), and
values stay below 2^256 and are only canonicalised when serialised.

**No assembly.** §18.4 established that this GCC leaves nothing on the table for
a loop whose whole content is a multiply and an add, and the loop here is
`(uint64_t)a[i] * b[j] + t[i+j] + c`, which compiles to one `MULU.L` and an add
chain. Assembly is a separate question and should be asked against a measurement
of this, not instead of one.

Three things beyond the representation, all of them algorithm rather than code:

- **A dedicated squaring**, 36 multiplies instead of 64, because every
  off-diagonal product appears twice.
- **Addition-chain inversion** — ref10's, 254 squarings and **11** multiplies —
  against TweetNaCl's square-and-multiply over every exponent bit, which costs
  254 squarings and **251** multiplies. That is 240 field multiplies saved per
  inversion and there are three inversions in a handshake.
- **A dedicated Edwards doubling** (4M+4S) instead of reusing the generic
  addition (8M+1 by a constant) for `add(p,p)`, which is what TweetNaCl does.

**Not done, deliberately, and named because it is the next lever:** the Ed25519
base point still has no precomputed table, and both scalar multiplications are
bit-at-a-time. §35.7 prices that.

#### The bug, because it survived every published vector

The first `fe_fold()` ran one carry-propagation pass and dropped whatever came
out of the top limb, on the reasoning that a value near 2^256−1 could not arise.
It arises constantly: with lazy reduction 0 is routinely carried as 2^256−38 and
1 as 2^256−37, adding 38 to either carries straight through all eight limbs, and
dropping that carry loses **exactly 38**. `fe_sub` had the mirror-image defect.

The symptom was an Ed25519 doubling of the identity returning 37 where it owed
−1 — and it is 38 away, which is what named it. What is worth recording is what
found it: not a published vector, but `fe_sqr` checked against `fe_mul` on random
inputs. Every RFC vector exercises the same handful of values, and two routines
that share a broken helper agree with each other. `tests/crypto68k/host/`
therefore runs 20,003 of those, including the all-ones and 2^256−38 cases where
the carry lives, alongside RFC 7748 §5.2/§6.1 and RFC 8032 §7.1 with every
signature also mutated three ways and required to be **refused**.

### 35.5 The measurement

`clients/dropbear/amiga_25519.c` `--wrap`s the four functions `curve25519.c`
exports onto the new implementation. The TweetNaCl bodies are still linked and
are simply never called, so the A/B is one linker flag rather than two source
trees, and `third_party/dropbear` remains unpatched.

**Both binaries in ONE emulator run**, in the measurement lane, so the host load,
the SLIRP scheduling and the server process are shared and the only thing that
differs between the two rows is the binary (`run-fsuae.sh -E`). §18.6 records
what host contention does to a figure taken minutes apart; this avoids the
question rather than arguing about it.

| | stock TweetNaCl | `crypto68k` | |
|---|---:|---:|---:|
| curve25519 scalar multiply (×2) | 23,374 ms | 2,626 ms | **8.90×** |
| ed25519 sign | 19,739 ms | 2,631 ms | **7.50×** |
| ed25519 verify | 39,402 ms | 5,124 ms | **7.69×** |
| public-key subtotal | 82,620 ms | 10,482 ms | **7.88×** |
| **the whole connection** | **85.10 s** | **12.28 s** | **6.93×** |

Repeated back to back in the same run: 12.28 s and 12.28 s. A later run with the
shipping binary — no profiler linked — gives **12.18 s** and **11.74 s**, running
`echo; uname -a; date` and returning `rc 0` with the right output.

**Against `LoginGraceTime`**: a connection goes from 71% of a stock server's
120-second patience to **10%**. That is the difference between a client that
works and a client that works on servers that have not been reconfigured.

The rows that did not move are as informative as the ones that did.
`chacha_crypt` is 74 ms out of 85,000 and 66 ms out of 12,000; §31.5's
conclusion that the cipher is invisible at this payload size survives a sevenfold
change in everything around it.

### 35.6 P-256 instead: the question §31.6 asked, answered

§31.6 proposed that the answer might be to negotiate the other half of
Dropbear's algorithm list — `ecdh-sha2-nistp256`, `ecdsa-sha2-nistp256`,
`rsa-sha2-256` — because `src/crypto68k/` accelerates P-256 by 10.8× over AmiSSL
and accelerates 25519 by nothing. `clients/dropbear/localoptions-p256.h` is that
build: `DROPBEAR_CURVE25519 0` **and** `DROPBEAR_ED25519 0`, because turning off
only the first moves the key exchange and leaves the host key and the client
signature on TweetNaCl. `sshd-testserver.sh` grew an ECDSA host key and an ECDSA
client key so the arm has something to verify against.

| | wall clock |
|---|---:|
| curve25519 / ed25519, TweetNaCl | 85.10 s |
| **ecdh-sha2-nistp256 / ecdsa-sha2-nistp256** | **149.62 s** |
| curve25519 / ed25519, `crypto68k` | 12.28 s |

**P-256 is 1.8× WORSE than the thing it was proposed to replace, and 12× worse
than the answer.** Its profile says why: three `ltc_ecc_mulmod` calls, 138.8 s
between them, one of them (the ECDH) 62.8 s on its own.

The reasoning in §31.6 was sound and the premise was wrong. `crypto68k`'s P-256
is fast; **Dropbear's** P-256 is `ltc_ecc_mulmod` over libtommath, which is not
`crypto68k` and is slower per scalar multiplication than TweetNaCl's curve25519
by a factor of five. §31.6's own caveat — that our speed lives in the
representation and wiring `nx_crypto`'s limb layout to libtommath's `mp_int`
may be a rewrite rather than a shim — was the load-bearing sentence, and this
result is what makes it decisive rather than cautionary. **Route A was never a
cheap experiment with an expensive follow-up; it was an expensive rewrite with
nothing in front of it.** Route B needed one new file and no bridge at all,
because the field code and the curve code are the same file.

For the record, in case anyone revisits it: §15's figures scale to about 4.2 s of
arithmetic for a P-256 handshake **if** `crypto68k`'s assembly could be reached
through Dropbear's `ecc_key`/`mp_int`. That is genuinely faster than 10.5 s. It
also costs interoperability — a modern OpenSSH does not have to offer either
`ecdh-sha2-nistp256` or an ECDSA host key, and increasingly does not — and it
buys less than §35.7 does for less work.

### 35.7 What is left, and what the floor actually is

At 12.28 s the split is 10.5 s of public-key arithmetic, 1.1 s in `select()`
and about 0.5 s of everything else. The cost model from §35.2 still applies and
now reads **0.54 ms per field operation**, consistent to 5% across all three
primitives — which is the check that the model is a model and not a coincidence.

The remaining arithmetic, in field operations:

| | now | with a base-point table and a 4-bit window |
|---|---:|---:|
| curve25519 (×2, Montgomery ladder — already near optimal) | ~5,120 | ~5,120 |
| ed25519 sign (one fixed-base multiplication) | ~4,620 | ~715 |
| ed25519 verify (one fixed-base, one variable-base) | ~9,480 | ~4,106 |

A signed-window table over the base point turns Ed25519 signing from 256
doublings and 256 additions into 64 additions, and a 4-bit window turns the
variable-base half of verification from 256 additions into 64. **That predicts
about 5.3 s of arithmetic and a roughly 7-second connection** — another 1.7×,
from tables that cost nothing to look up on a part §18.1 measured as having no
data cache. It is a contained, testable change against the same vectors and it
is the obvious next piece of work.

Below that, the honest answer starts to arrive. A curve25519 scalar
multiplication is irreducibly about 2,500 field multiplications and a field
multiplication is irreducibly 72 `MULU.L` at 32 cycles, which is 2,300 cycles of
pure multiply — 0.16 ms at 14 MHz, against the 0.54 ms measured. So there is
perhaps another 2× available in the field routine itself, in assembly, and
essentially nothing after that. **The floor for this suite on this part is around
two seconds of arithmetic, and a login of three to four seconds.** Not 84, and
not zero.

Which makes the conclusion the opposite of §31's. `ssh` from an Amiga is not
"possible but unpleasant, so make the wait tolerable". At 12 seconds it is
already usable, at 7 it would be unremarkable, and none of that needed a faster
machine — it needed a field element that was the right shape for the one we
have.

### 35.8 The harness, and three things that cost a run each

| | |
|---|---|
| `clients/dropbear/dbprofile.c` | the `--wrap` profiler; `build.sh -p` links it |
| `clients/dropbear/tweetnacl-count.sh` | field-multiply counts, on the build host |
| `clients/dropbear/amiga_25519.c` | the four `--wrap`s onto `crypto68k` |
| `clients/dropbear/localoptions-p256.h` | the no-25519 arm; `build.sh -O` selects it |
| `src/crypto68k/c68k_25519.c` | the implementation |
| `tests/crypto68k/host/test_c68k_25519.c` | the vectors, in `tools/ci.sh host` |
| `build.sh -S` | stock TweetNaCl — the other arm of the A/B, not a fallback |

**`make` did not relink, and that looked exactly like the change not working.**
Dropbear's Makefile makes `dbclient` depend on its own objects; ours arrive
through `LIBS`, which is a variable and not a prerequisite. Editing the shim and
re-running the build script left the old executable in place, so a profiling run
completed and printed nothing. `build.sh` now removes the program when a shim
object is newer than it.

**A constructor runs before this `crt0` has finished setting newlib up.** An
`atexit()` registered from one does not survive and an `fprintf()` from one goes
nowhere — `amiga_dropbear.c`'s constructor gets away with it because it touches
only `dos.library`. Arm on the first real call instead. The linked
`___CTOR_LIST__` did grow by one entry, so "the constructor did not run" was the
wrong diagnosis and cost the time it takes to check a symbol table.

**`printf()` to stdout produced nothing under `ClientRun`** while Dropbear's own
`fprintf(stderr)` came through in the same transcript. An instrument should use
the channel that is demonstrably wired up rather than the one that ought to be.

Verified on `turo@playhouse2` with the pinned Linux toolchain: `tools/ci.sh` —
host, all four cross configurations, conformance — all green.

## 36. Does Roadshow run a faster timer? Asked from outside, and answered (2026-07-26)

29.5 is the only place in this document where Roadshow beats us on the wire by
a factor rather than a few per cent: its `ping` reports a minimum round trip of
**4.32 ms** against our **7 ms**, and on one run its *maximum* was below our
minimum. Two explanations fit that equally well and they have opposite
consequences:

* **a faster periodic timer.** Ours is 50 Hz (16.6, read off the running
  system). If Roadshow ran at 100 Hz, everything it does on a timer would land
  on a 10 ms grid where ours lands on 20.
* **a cheaper receive path**, which is the same direction 29.3's curl result
  points in and would make this the third independent measurement of the same
  thing.

**Both were tested. The answer is the second, and it does not go the way 29.5
reads.** Nothing of Roadshow was disassembled, decompiled or inspected; what
follows is entirely the behaviour of a running system observed from below it,
in the same sense that 27.1 characterised SLIRP's DHCP interception.

### 36.1 The instrument: their stack, our wire

27.1 established that a host-side packet injector is impossible here — SLIRP
terminates the guest's TCP and re-originates it, so nothing on the host ever
sees a guest sequence number — and built `tests/tcpdrill/tapdev.c` instead: an
Exec device created at run time with `MakeLibrary()`/`AddDevice()` that
implements enough of SANA-II for a TCP/IP stack to open it, configure it, take
it online and run its readers against it. **Every frame it is handed is
timestamped with `ReadEClock()` inside `BeginIO`** — the instant the stack
handed it over, 1.4 µs resolution — and every frame the stack receives is one
the harness composed.

Nothing about that device is ours. `tests/compare/tickprobe.c` installs it,
then opens whatever `bsdsocket.library` is in `LIBS:`; every call into the
stack is a published LVO. So the stack is a parameter:

```
tests/compare/run-tickprobe.sh -s ours|roadshow
```

**Two things had to change for a foreign stack to come up on it**, and both are
recorded because each cost a boot:

* **`S2_ONEVENT`.** `tapdev.c` answered it `IOERR_NOCMD`, which is fine for
  ours — we never issue it — and wrong for a stack that watches its link for a
  living. It now completes at once if the named event has already happened and
  is otherwise **held**, which for a device with no wire means forever, and
  released by `S2_OFFLINE` and by `tap_remove()`.
* **`STATE=ONLINE`.** Roadshow's own `AddNetInterface.doc` says interfaces
  "switch automatically to 'up' state" and that `online` is the *alternative*
  which "tells the underlying network interface driver to go online first".
  Without it the first run got `AddNetInterface` returning 0, an interface
  configured with the right address, and a device that had never been sent
  `S2_ONLINE`. The two stacks' interface files are therefore different and
  both are in the tree (`tests/compare/tick-if.ours`,
  `tests/compare/tick-if.roadshow`) — a stack should be measured with a
  configuration it agrees is well formed.

There is **no `-n` and no `a2065.device`** in any run here: SLIRP, the emulated
card and the host's networking are all outside the measurement.

### 36.2 How a timer rate is visible from outside

A frame a stack sends *because a timer fired* leaves on that timer's grid, so
every **interval** between two such frames is a whole number of grid steps.
For a candidate spacing T,

```
R(T) = | mean_k exp( 2*pi*i * gap_k / T ) |
```

is near 1 when every interval is a whole number of T and near 0 otherwise, and
sweeping T draws a comb with a tooth at the grid and at every submultiple of
it. `tests/compare/tickphase.py` does the sweep and prints the harmonic ratios
next to the teeth, because **"the gaps are multiples of 10 ms" settles
nothing** — quantisation at 20 ms implies quantisation at 10.

Intervals rather than absolute times, deliberately: absolute phase coherence
across a minute would need the tick source exact to a part in 10^6, and a
400 ms interval tolerates a hundred times worse.

The event sampled is the **delayed ACK**: timer-driven on every TCP there has
ever been, provokable once per round trip, and the harness controls exactly
when the segment that arms it arrives. 160 of them per run.

**Two design decisions are the difference between a measurement and a
fiction**, and the second one is checked in the output rather than asserted:

* **`Delay()` is never used to pace anything.** It is one 20 ms AmigaDOS tick,
  the same order as the thing being measured, and sleeping on it would alias
  the experiment into agreeing with whatever the system tick is. Every wait is
  a `timer.device` `UNIT_MICROHZ` request.
* **The injections are spaced by a pseudo-random 0..400 ms**, so the arming
  segments land at uniformly distributed phases. The analyser runs the same
  comb over the **injection** intervals and prints it every time: they must
  show no tooth anywhere. In every run reported below they do not —
  `R(T) < 0.75` across 1..260 ms — and the harness's own loop is therefore not
  what any grid below is made of.

Nought..400 ms rather than nought..20, for a reason that is easy to get wrong
and was got wrong on the first run: intervals that are all nearly equal are
congruent modulo almost anything. The analyser now says so itself when the
intervals span fewer than three grid steps.

### 36.3 What the machine offers, timed by the harness

Before either stack is open, 100 back-to-back `timer.device` requests on each
unit, timed with the same E-Clock:

| | measured | asked for |
|---|---|---|
| `UNIT_VBLANK` | **19.853 / 19.848 / 19.880 ms** per frame (50.30–50.37 Hz) | — |
| `UNIT_MICROHZ` | 5.373 ms | 5.000 ms |

Two things to take from it. The vertical blank in this emulator is **not** a
crystal: it moves 0.16% between boots. And a `DoIO()` round trip costs about
0.37 ms here, which is why nothing in this section is paced by one.

### 36.4 The timer: ours is the finer of the two

Same instrument, same machine, same harness binary, `-x` throughout so no other
agent's boot shares the emulator.

**Ours** — `bsdsocket.library` built from `b0dd15f` in a private build
directory (24.9 and 29.8 both record measurements lost to another workstream
rebuilding the instrument underneath them), plus two runs from `build/cm` that
agree with it:

```
held acks: 79 intervals, 299.8..902.1 ms (spread 602 ms)
    T (ms)      R      T / strongest
      20.0311  0.9918    1.0000
     100.1330  0.9825    4.9989
      25.0402  0.9772    1.2501
      16.6919  0.9714    0.8333
      10.0155  0.9675    0.5000
    -> strongest grid: 20.0311 ms  (49.922 Hz), R = 0.9918
```

**20.031 ms, 49.92 Hz**, reproduced at 20.0311 / 20.0329 / 20.0381 ms across
three boots. That is the 50 Hz tick 16.6 read off the startup line, now
measured from the wire instead — and the comb's `× 4.999` tooth at 100.13 ms is
NetX Duo's fast periodic processing, five ticks, which is what actually
releases the acknowledgement.

**Roadshow 1.15**, two boots:

```
held acks: 159 intervals, 219.2..441.8 ms (spread 223 ms)
    T (ms)      R      T / strongest
     220.3562  0.9999    1.0000
     110.1781  0.9997    0.5000
      73.4521  0.9994    0.3333
    -> strongest grid: 220.3562 ms, R = 0.9999
```

**220.356 and 220.275 ms** — the cleanest fit in this whole document,
`R = 0.9999`, and stable to 366 ppm across boots.

| | finest grid any timer-driven frame lands on | |
|---|---:|---|
| AmiNetXDuo | **20.031 ms** | resolves to the individual tick |
| Roadshow 1.15 | **220.316 ms** | 11.00 × our tick, 2.20 × our ACK grid |

**So the hypothesis is the wrong way round.** Nothing Roadshow puts on the wire
is quantised more finely than 220 ms; our own timer-driven frames resolve to a
20 ms grid, and our delayed ACKs are released on a 100 ms one. Whatever their
tick is, what it *delivers to the wire* is eleven times coarser than what ours
delivers.

### 36.5 What this does not settle, stated rather than buried

**Roadshow's tick itself is not observable from here, and it is not claimed.**
220.316 ms is `11 × 20.029` and equally `22 × 10.014`; a 100 Hz tick with a
protocol timer counting 22 of them fits the data exactly as well as a 50 Hz
tick counting 11. Nothing in any run separates them, because **every** frame
Roadshow emits on a timer lands on that one grid — its intervals are 1× or 2×
220 ms and never anything between, so there is no finer structure to fit. The
analyser prints the warning itself rather than letting the fit look better than
it is.

Two things were tried to break the tie and neither did:

* **The machine's vertical blank**, on the theory that a grid which is an exact
  multiple of it must be VBlank-derived. Measured at 19.85–19.88 ms and moving
  0.16% between boots (36.3), it is not precise enough: `220.316 / 11` is
  20.029 and `/22` is 10.014, and neither is within 0.8% of the frame period or
  half of it.
* **The retransmission grid**, which would be a second population with a
  different period and hence a common divisor. Roadshow retransmits an
  unanswered SYN once in 20 seconds, at 5.740 s on one boot and 5.859 s on the
  other, and that gap runs from an *application* instant to a *timer* instant —
  it is not a whole number of ticks and cannot be used. Getting two
  timer-to-timer intervals out of it needs a 45-second window per boot and was
  not run.

**What is settled is the thing the question was actually about.** A tick that
cannot produce a wire event more finely spaced than 220 ms cannot be the reason
their ICMP round trip is 2.7 ms shorter than ours — and 36.6 shows it is not
the reason for anything else either.

### 36.6 The competing explanation, and it does not go their way

An ICMP echo reply is **not** timer-driven: it is generated because a request
arrived. The time from handing an echo request to the device to the stack
handing the reply back is therefore the entire receive-and-reply path — the
SANA-II copy hooks, the shim, the IP and ICMP code, and the transmit — with the
wire, SLIRP, the emulated card and the application all removed.

64 samples at 56 bytes (what every `ping` sends) and 32 at 1400:

| payload | | AmiNetXDuo | Roadshow 1.15 |
|---|---|---:|---:|
| 56 B | min / p50 / p90 | **1.789 / 1.793 / 2.127 ms** | 2.209 / 2.219 / 2.478 ms |
| 1400 B | min / p50 / p90 | **2.563 / 2.566 / 3.278 ms** | 4.206 / 4.208 / 4.483 ms |
| | fixed cost | **1.761 ms** | 2.136 ms |
| | per byte | **0.575 µs** | 1.480 µs |

**We are faster on both sizes, and 2.6× cheaper per byte.** At 14 MHz,
0.575 µs/B is about eight cycles per byte for a copy in, a checksum and a copy
out; theirs is about twenty-one.

**Neither distribution is quantised.** p50 minus min is 4 µs on ours and 10 µs
on theirs; the comb finds nothing anywhere in 1..260 ms. An ICMP round trip on
either stack is not paced by a timer, which on its own disposes of the timer
explanation for 29.5 — a 20 ms grid cannot produce a 7 ms round trip with a
2 ms spread.

The same run gives one more arrival-driven figure, for TCP rather than ICMP:

| | AmiNetXDuo | Roadshow 1.15 |
|---|---|---|
| one data segment in, ACK out, when the ACK is not held | **2.03 ms**, 80 of 160 segments | never — all 160 were held |

That is 16.6's `NX_TCP_ACK_EVERY_N_PACKETS 2` seen from outside: on a stream of
lone segments we answer every second one in about 2 ms, and Roadshow answers
none of them until its 220 ms timer comes round. **On this workload our
acknowledgement latency is two orders of magnitude better than theirs**, which
is worth setting against 29.3's bulk-throughput loss: the two are not measuring
the same thing.

### 36.7 So where are the 2.7 ms?

Not above SANA-II. Through the same device, on the same machine, our ICMP
turnaround is **0.43 ms faster** than Roadshow's at 56 bytes and **1.64 ms
faster** at 1400, and neither is timer-paced. Yet on the real wire, through
`a2065.device` and SLIRP, 29.5 has us 2.7 ms *slower*. The difference is
therefore in what this instrument replaces, and the candidates are named rather
than guessed at:

* **`a2065.device` and the interrupt path** — how many `CMD_READ`s each stack
  keeps queued, what it costs to re-post one, and what the driver does between
  the card's interrupt and the reply. Ours posts 32 IPv4 reads (16.4);
  Roadshow's default is 32 and this harness asked it for 8.
* **The two `ping` commands**, which are different binaries with different
  clocks and were never compared against each other on a fixed round trip.

**The next measurement is a small one**: the same `tickprobe` phases run over
the real A2065 with the emulator's own frame dump alongside (16.3), so the
turnaround measured here can be subtracted from the turnaround measured there
and the remainder attributed to the driver. That is one boot per stack.

### 36.8 Two by-products worth recording

**Our retransmission now backs off, and 27.5 is out of date.** That section
measured a flat ~1 s with no backoff and named `NX_TCP_RETRY_SHIFT` as the
one-line half of the fix. Measured here on `b0dd15f`, an unanswered SYN is
retransmitted at

```
802.3, 1982.5, 4006.4, 7992.7 ms
```

— 1, 2, 4, 8 seconds, and in units of the 20.031 ms tick measured in the same
run those are 40.05, 98.97, 200.01, 398.99. `nx_user.h` now carries
`NX_TCP_RETRY_SHIFT 1`; the change is another workstream's and is noted here
because a reader comparing 27.5's numbers with these would otherwise think one
of them was wrong. Roadshow's first SYN retransmission comes at 5.74–5.86 s.

**The a2065 frame dump was not used for any of this and here is why.** 16.3
records that it has no timestamps at all, and a grid measurement is nothing but
timestamps. The dump remains the right instrument for loss and ordering; for
timing the only clock below every line of software is the E-Clock read inside
`BeginIO`, which is what this section is built on.

### 36.9 Running it

```
tests/compare/run-tickprobe.sh -s ours     [-b BUILD]     # 160 ACKs, 96 pings
tests/compare/run-tickprobe.sh -s roadshow [-R DIR]
tests/compare/tickphase.py build/tickprobe-tick-*.txt
```

About 100 seconds of emulator per run, one boot, no network. `run-tickprobe.sh`
passes `-x` to `tools/fsuae-run.sh` unconditionally: a quantisation histogram
taken while another agent's boot shares the machine is a histogram of that
agent.

## 37. Hours of Fitz, and the socket that never came back (2026-07-26)

Every harness in this tree runs for seconds or minutes. §29 measured throughput
against Roadshow and AmiTCP_NG over a few megabytes; §27's tcpdrill asserts on
packets and moves a few hundred bytes; the longest thing here before today was
§14's curl suite at about four minutes. **Nothing in this project has ever
tested what happens after an hour.**

A report on English Amiga Board (thread 122501) says that matters: weeks of
long-term testing with **Fitz** — a cross-platform network file server and
mounter — made **both AmiTCP 4 and Roadshow return `EAGAIN` on a *blocking*
socket**, which should be impossible, after which the connection was finished
and the only thing left to do was close it. The reporter blames mbuf
fragmentation and sequence-number overrun.

This section builds the harness that would find that, points it at the same
program, and reports what it found — which is not what it went looking for.

**Headline: a blocking socket in this stack cannot return `EAGAIN` the way the
report describes, and the reason is arithmetic rather than luck. But the run
that established it found two defects that are worse, and one of them puts the
machine on a clock: `AvailMem` falls by a steady 1009 bytes a second under
connection churn, the packet pool is gone in 115 seconds, and every leaked
socket is one NetX Duo will never let go of again.**

### 37.1 The specific suspect, traced to the end

`src/bsdsocket/errno.c:63` maps `NX_NO_PACKET` to `AMI_EWOULDBLOCK`
unconditionally, and `NX_NO_PACKET` does not only mean "nothing to read": it is
also what NetX Duo returns when the **packet pool is exhausted**. On a blocking
socket that mapping would produce exactly the reported symptom.

It does not, and the reason is one line: `bsd_wait_option()`
(`src/bsdsocket/select.c:228`) hands NetX Duo `NX_WAIT_FOREVER` for a blocking
socket with no `SO_RCVTIMEO`/`SO_SNDTIMEO`, and with a non-zero wait option
**NetX Duo does not have a path that returns `NX_NO_PACKET`** — it suspends:

| | |
|---|---|
| `_nx_tcp_socket_receive.c:231` | `else if ((wait_option) && (_tx_thread_current_ptr != &(ip_ptr -> nx_ip_thread)))` — suspends; the `NX_NO_PACKET` at :263 is the `else` |
| `_nx_tcp_socket_send_internal.c:1006` | the same guard; `NX_WINDOW_OVERFLOW` (:1086) and `NX_TX_QUEUE_DEPTH` (:1098) are its `else` |
| `_nx_packet_allocate.c:178` | `if (wait_option)` suspends unconditionally; `NX_NO_PACKET` at :268 is the `else` |

So every one of those returns needs either `wait_option == 0` — a **non**-blocking
socket, where `EWOULDBLOCK` is correct — or the calling thread to **be the IP
thread**. And it never is. Every vector in this library brackets itself with
`bsd_nx_enter()`, which calls `tx_amiga_adopt_thread()` to make the *calling
Exec task* a TX_THREAD of its own (`src/netstack/netstack.c:143`), so an
application's `recv()` always runs on a thread that is not the IP thread.

The code in `src/bsdsocket/` that **does** run on the IP thread is the
callbacks, and **none of them enters a vector**: the five notify hooks
(`select.c:68–169`) do nothing but `bsd_event_post()`; `bsd_listen_callback`
and `bsd_tcp_disconnect_callback` the same; `bsd_tcp_urgent_notify`
(`oob.c:303`) walks the receive queue and posts; `bsd_oob_ip_filter` patches
six bytes of a header; and `bsd_raw_filter` (`raw.c:144`) copies with
`NX_NO_WAIT` and does a `tx_semaphore_put`. Not one of them can suspend, and
not one of them calls `bsd_recv()`.

**That invariant is load-bearing and nobody had written it down.** It is the
only thing standing between six unconditional `EWOULDBLOCK` mappings and the
reported defect. If any future vector is ever called from a NetX Duo callback,
`recv()` on a blocking socket starts returning `EAGAIN` and there is no comment
anywhere that says why it used to be safe.

### 37.2 Three sites where a blocking socket *can* return `EWOULDBLOCK`

Traced rather than assumed, and none of them is the pool:

| site | when |
|---|---|
| **`transfer.c:294`** `if (sent == 0 && len > 0) return bsd_fail(base, AMI_EWOULDBLOCK);` | **the NetX Duo status is discarded before the decision is made.** Every first-iteration failure of `nx_packet_allocate()` or `nx_packet_data_append()` `break`s out of the loop and lands here, whatever it was. With `NX_WAIT_FOREVER` the reachable ones are `NX_WAIT_ABORTED` (should be `EINTR`) and `NX_POOL_DELETED` (should be `ENOBUFS`) |
| **`socket.c:1521`** `accept()` maps `NX_NOT_CONNECTED` to `EWOULDBLOCK` | `_nx_tcp_connect_cleanup` sets `NX_NOT_CONNECTED` on a suspended accept whose listener is torn down by another task. A blocking `accept()` then reports `EAGAIN` for a socket that is gone |
| **`transfer.c:640`** `bsd_recv_raw`: `if (packet == NX_NULL) return bsd_fail(base, AMI_EWOULDBLOCK);` | `bsd_raw_receive()` returns `NX_NULL` when `tx_semaphore_get(wait)` fails for **any** reason, `TX_WAIT_ABORTED` included, and the caller cannot tell which |

`transfer.c:277`, `:332`, `:386`, `:452`, `:553` and `oob.c:238`, `:272` carry
the same unconditional mapping and are unreachable on a blocking socket **only**
because of §37.1's invariant. None of the ten consults `ASF_NONBLOCK` before
answering.

The measured control is in the harness: `tests/endurance/` records every errno
**with the socket's blocking state at the time**, because that pairing is the
whole question. In the loopback arm, 2,796 errno events were recorded, all on
blocking sockets, and **not one of them was `EWOULDBLOCK`** — they were
`ECONNREFUSED`, `EINVAL`, `EPIPE` and `EIO`, which is a different section
(§37.4).

### 37.3 The harness, and Fitz

`tests/endurance/` is two workloads sharing one instrument.

**Fitz is the realistic one, and it is the program from the report.** MIT
licensed with full source, symmetric between Amiga and Unix, no central server.
`tests/endurance/fetch-fitz.sh` downloads it and `build.sh` builds **two**
m68k binaries from the same sources: the released one, and one with
`-DADEBUG=5`. The second is the point. Fitz's client treats `EAGAIN` on its
blocking socket as retryable —

```c
static BOOL checkretry(FitzClient *fc)          /* src/amiga-client.c:583 */
{
    if (Errno() == EAGAIN)
    {
        db(WARN,("* EAGAIN\n"));
        Wait(newtimereq(fc, 0, 20000));         /* 20 ms */
        ...
        return TRUE;
    }
    return FALSE;
}
```

— and retries it **ten times** (`MAXRETRY`) before `send_all()`/`recv_all()`
give up and the connection is abandoned. That is the reported symptom seen from
the application's side, and on the released binary it is silent. Built with
debug at WARN, the same code prints `* EAGAIN` and `* recv error err=-1 len=N
errno=E` through `kprintf()` to the serial port, which `tools/fsuae-run.sh`
already captures. **A count of zero in that log is a result; an inference from
a connection that died is not.**

Three things had to be solved to build it, all recorded because they will
recur:

- **`src/kprintf.asm` is vasm source** and this tree has no vasm.
  `tests/endurance/fitz-kprintf.c` reimplements `kprintf()` and `mysnprintf()`
  on `RawDoFmt()`, including `mysnprintf()`'s deliberately non-C99 return
  value, which Fitz's own header calls out and its callers depend on.
- **`__udivdi3` is not in this toolchain's `libgcc.a`** — checked with `nm`
  across every archive it ships, not assumed — and Fitz's `ds_to_unix()` needs
  one. Supplied in the same file, on 32-bit limbs, because the obvious
  `unsigned long long` version needs `__lshrdi3` and `__ashldi3`, which are
  missing for the same reason.
- **The NDK's `inline/bsdsocket.h` will not compile under GCC 15**
  (`'asm' specifier ... conflicts with 'asm' clobber list`, on
  `SetSocketSignals`). `tests/conformance/compat` first on the include path
  fixes it, exactly as it does for `tests/clients`.

Nothing in Fitz's own sources is changed. The value of running somebody else's
program is that it is somebody else's.

(What it found in eleven minutes is §37.10; the run was stopped early and why
is recorded there.)

**The arrangement is Amiga-as-client.** Neither this Mac nor the Linux build
host has FUSE, and Fitz's Makefile splits the Unix side into `fitz-serve` (no
FUSE) and `fitz-mount` (FUSE) for precisely that case. So `fitz-serve` runs on
the host, the guest runs `fitz mount 10.0.2.2:17711 FITZ:`, and the workload is
AmigaDOS `Open`/`Write`/`Read`/`Close` against a mounted volume — which Fitz's
handler turns into blocking `send()`/`recv()` pairs on one long-lived TCP
connection. Both directions are covered: every file is written to the share and
read back, byte for byte.

**`Endurance` is the reproducible one**, and it is also the instrument. It
drives sockets directly with read and write sizes redrawn log-uniformly for
every single call on both sides independently, so the two ends never agree
about framing; it verifies every byte against a position-addressable pattern
(`byte(o) = pat[o & 8191] ^ (o >> 13)`, period 2 MB, so a splice that repeats
or drops a block is caught as well as an altered byte); and every payload
carries its own stream offset in a header, so a framing desync shows up
immediately rather than as corruption later.

And every `sample` seconds it appends one CSV row through `NetStackQuery()`
(§34's private LVO, which is why a program that has not linked `src/netstack`
can read the running stack at all): packet-pool free count and the pool's own
empty-request/empty-suspension counters, `AvailMem` **total and largest
contiguous block**, live socket count, TCP retransmissions, receive drops,
checksum errors, SANA-II allocation failures. Both output files are opened,
appended and closed per line, for §16.9's reason: a run that has to be killed
must not lose its last twenty lines.

### 37.4 The listener that stopped accepting, and did it every time

The loopback arm collapses two seconds into every run, always the same way,
and it reproduces with **one** responder, one driver and nothing else running.
One line on the serial log:

```
[WARN] bsdsocket: relisten failed, status 71
```

71 is `0x47` = **`NX_INVALID_RELISTEN`**. In `bsd_accept()`
(`src/bsdsocket/socket.c:1613-1665`) the order is:

```c
sock->as_Incoming = NULL;                      /* unconditional */
...
spare = bsd_socket_alloc(...);
if (spare != NULL) {
    if (nx_tcp_socket_create(...) == NX_SUCCESS) {
        status = nx_tcp_server_socket_relisten(ip, sock->as_ListenPort, ...);
        if (status == NX_SUCCESS || status == NX_CONNECTION_PENDING)
            sock->as_Incoming = spare;         /* the ONLY place it is set */
        else
            ... AMI_WARN("relisten failed, ...");
    }
}
```

`as_Incoming` is cleared before its replacement is secured, and when the
relisten does not take there is nothing that puts it back. The listener is
then left with `as_Incoming == NULL`, and the very first check in
`bsd_accept()` is

```c
if ((sock->as_Flags & ASF_LISTENING) == 0 || sock->as_Incoming == NULL)
    return bsd_fail(SocketBase, AMI_EINVAL);
```

**so every subsequent `accept()` on that socket returns `EINVAL` for the
lifetime of the socket, and there is nothing the application can do about it
except close the listener and build another one.** Measured: 1,951 consecutive
`EINVAL`s over 400 seconds in the two-connection run and 673 in the
single-connection one, both starting at the accept that followed the failed
relisten, and both with exactly one `relisten failed` on the serial log in
front of them.

Two more observations from the same two seconds, recorded because they are
probably the same root cause and are certainly not separate bugs to chase
independently:

- **The one connection that *is* accepted is unusable.** Its first `recv()`
  fails immediately — `EDESTADDRREQ` on the server side, which is
  `bsd_errno_from_nx(NX_NOT_BOUND)`, and `EIO` on the client side, which is
  `bsd_errno_from_nx()`'s fallback for a status the table does not name. An
  accepted socket that NetX Duo does not consider bound.
- **A blocking `connect()` to that listener afterwards never returns.** In the
  single-connection run the driver went into `connect()` at `t = 2` and was
  still there when the stall detector fired at `t = 122`; in the
  two-connection run the same call came back `ECONNREFUSED` about twice a
  second. Same listener state, two different answers, and one of them is an
  unbounded hang on a blocking call with no timeout.

**A hypothesis that looked airtight and is wrong, recorded so nobody spends the
hour twice.** `_nx_tcp_server_socket_relisten()` matches a listen request only
when its socket designation is empty:

```c
/* nx_tcp_server_socket_relisten.c:138 */
if ((listen_ptr -> nx_tcp_listen_port == port) &&
    (!listen_ptr -> nx_tcp_listen_socket_ptr))
```

and `bsd_accept()`'s success path never calls
`nx_tcp_server_socket_unaccept()` on the socket it has just promoted — though
both of its *failure* paths do, unaccept → relisten → accept, which is the
NetX Duo server idiom. So "the listen request still points at the accepted
socket, therefore relisten can never match" is the obvious conclusion, and it
predicts the failure exactly.

It is wrong, because `unaccept()` is not the only thing that clears the slot.
`nx_tcp_packet_process.c:650` does it too, when the SYN arrives:

```c
/* Clear the server socket pointer in the listen request.  If the
   application wishes to honor more server connections on this port,
   the application must call relisten with a new server socket
   pointer.  */
listen_ptr -> nx_tcp_listen_socket_ptr =  NX_NULL;
```

So by the time `accept()` returns, the slot is normally already empty and the
relisten normally succeeds — and the measurement agrees: in the `leak` arm the
same responder accepted **three or more** connections in a row with no accept
error and no `relisten failed` at all. **The failure is intermittent, not
structural, and what distinguishes the runs that hit it is not established.**

What *is* established, and is what matters: **when the relisten does not take,
the listener is finished for good**, because `as_Incoming` has already been
cleared and has exactly one assignment. 1,951 consecutive `EINVAL`s in one run
and 673 in another, each behind exactly one `relisten failed` line. A
listening socket that reports no error at `listen()` time and refuses every
connection afterwards is the shape the EAB report calls a permanently degraded
socket — and this one is ours rather than inherited.

`tests/endurance/run-endurance.sh` reproduces it in both its `loop` and
`churn` shapes. The recovery is a separate question from the trigger and is
cheaper: `bsd_accept()` could put the original socket back when the relisten
fails, exactly as its `bsd_fd_alloc()` failure path already does, instead of
leaving the listener with nothing. Both are in `src/bsdsocket/socket.c`, which
is another workstream's.

**Where the honesty line is.** The `relisten failed` warning and the permanent
`EINVAL` after it are library behaviour and a plain reading of the code: the
warning is the library's own, and `as_Incoming` has exactly one assignment.
What provoked the *first* accept to hand back an unbound socket is not settled
— it could be an ordering mistake in the harness, which spawns its responder
and its driver as separate Processes half a second apart. Those two are worth
separating deliberately rather than assumed to be one bug, and only the second
is claimed here.

### 37.5 1009 bytes a second, and 830 sockets NetX Duo will not let go of

The two-connection run then spent 400 seconds in the failure state above, and
what it recorded there is the second defect:

| t (s) | `AvailMem` | largest block | pool free | pool empty req | live sockets |
|---:|---:|---:|---:|---:|---:|
| 25 | 9,027,560 | 6,879,048 | 157 | 0 | 65 |
| 55 | 8,996,936 | 6,879,048 | 99 | 0 | 123 |
| 85 | 8,967,368 | 6,879,048 | 41 | 0 | 179 |
| **115** | 8,937,800 | 6,860,568 | **1** | **16** | 235 |
| 235 | 8,815,304 | 6,738,072 | 1 | 248 | 467 |
| 431 | **8,617,832** | **6,540,600** | 1 | 622 | **841** |

- **`AvailMem` falls by 409,728 bytes in 406 seconds — 1009 bytes/s — and
  never recovers.** The largest contiguous block falls with it, so this is
  consumption, not fragmentation.
- **The live socket count climbs 1.91 a second and never falls**: 776 in
  406 s. 409,728 / 776 = **528 bytes each**, and `sizeof(AmiSocket)` is 520.
  One socket structure leaked per socket created, plus allocator overhead.
- **The packet pool is gone in 115 seconds.** From `t = 25` (65 sockets, 157
  free) to `t = 115` (235 sockets, 1 free) is 170 sockets against 156 packets:
  about **one packet leaked per leaked socket** — the SYN that is never
  released, because the socket holding it is never deleted.
- After that, `pool_empty_requests` and `nsx_IpSendDropped` rise together, 622
  each, while `pool_empty_suspensions` stays at **0**. Nothing is waiting for
  the pool; the IP thread asks with `NX_NO_WAIT`, is refused, and **the frame
  is silently dropped**.

The library says so itself, 830 times in ten minutes:

```
[WARN] bsdsocket: nx_tcp_socket_delete refused (66); leaking 520 bytes
       rather than corrupting the created list
```

66 is `0x42` = **`NX_STILL_BOUND`**. `bsd_socket_destroy()`
(`src/bsdsocket/socket.c:786-820`) calls `nx_tcp_client_socket_unbind()` and
then `nx_tcp_socket_delete()`; the unbind does not take, the delete refuses,
and the code does the only safe thing left — it leaks the block rather than
free memory NetX Duo still has on `nx_ip_tcp_created_sockets_ptr`, which its
own comment correctly says would be worse. **The leak is the safe half of a
bug whose unsafe half was already anticipated.** Two more lines from the same
log say where the sockets are:

```
[WARN] bsdsocket: close did not complete in 60 s (state 7); resetting
[WARN] bsdsocket: close did not complete in 60 s (state 8); resetting
```

State 7 is `FIN_WAIT_2`, state 8 is `CLOSING`. Worth reading next to §32:
`close()` sending a FIN instead of a RESET landed in this tree **today**, and a
socket that resets never had a `FIN_WAIT_2` to get stuck in.

**What it is not.** The obvious explanation — that closing a socket leaks — is
wrong, and `mode leak` exists to say so. It does the smallest repeatable
thing: create a socket, put it through one lifecycle, close it, in two arms
(one `connect()` refused by a port with nothing on it, one full
connect/exchange/close against a live listener). **11,915 lifecycles in ten
minutes, and nothing moved**: `AvailMem` 9,325,864 → 9,360,736, live sockets
5 → 10, pool 222 → 217, and not one `NX_STILL_BOUND`. An ordinary socket
lifecycle is clean.

Nor is it the dead listener by itself. A third run put **one** client against
**one** listener in exactly the §37.4 failure state for eight and a half
minutes: 673 `EINVAL`s, one `relisten failed`, and **`AvailMem` 9,320,552 →
9,359,304 — up — live sockets 5 → 3, pool 222 → 221, and no `NX_STILL_BOUND`
at all.**

| run | conns | listener state | lifecycles | `AvailMem` drift | leaked sockets |
|---|---:|---|---:|---:|---:|
| `mode leak` | — | live, healthy | 11,915 | **+34,872** | 0 |
| churn, `-c 1` | 1 | dead (§37.4) | ~3 | **+38,752** | 0 |
| soak, `-c 2` + hogs | 2 | dead (§37.4) | ~830 | **−409,728** | 776 |

So the trigger is narrower than "close" and narrower than "§37.4", and the
measured difference between the third row and the second is that in the third
the client kept dialling — about twice a second, and getting `ECONNREFUSED`
back — where in the second it went into `connect()` once and never came out.
The leaking lifecycle is a **refused** `connect()` to a port that has a listen
request on it, as opposed to one with nothing on it at all, which `mode leak`
row 1 does 11,915 times without leaking a byte.

That is where to look. It is not proven here: the leak has been measured, its
rate pinned to `sizeof(AmiSocket)`, and both innocent explanations cleared with
controls, and the remaining step needs `src/bsdsocket/socket.c`, which is
another workstream's.

**What it costs, if it is ever reached in the field.** 1009 bytes/s is
3.6 MB/hour against a machine with about 9 MB free: out of memory in **under
three hours**, and out of packet pool — hence dropping every frame it tries to
send — after **two minutes**. That is the "hours of mixed traffic" failure the
report describes, arrived at from a different direction.

**And, incidentally, the best test of the suspect this section started with.**
For 316 consecutive seconds the packet pool sat at **1 free of 256** while
blocking sockets were being used throughout, and across 2,796 recorded errno
events **not one was `EWOULDBLOCK`**. An exhausted pool does not produce
`EAGAIN` on a blocking socket in this stack. It produces dropped frames.

### 37.6 Sequence-number wrap: the arithmetic, and what NetX Duo does about it

The report's second suspicion. Three separate questions, and they have three
different answers.

**Is it reachable?** A 32-bit sequence space is 4,294,967,296 bytes. §24.4
measures 159 KB/s on the wire and 283 KB/s on loopback, so one direction of
one connection wraps after **7.5 hours on the wire** or **4.2 hours on
loopback** — and Fitz over a mounted share is slower still, 68 KB/s of payload
combined at 14 MHz, which is **17 hours**. That is inside a long test but outside every test this project has
ever run — and it is *exactly* the duration the EAB report describes. So the
suspicion is well-formed: it is the first thing that becomes reachable at that
timescale and at no shorter one.

**Where does a connection start?** `bsd_tcp_seed_isn()`
(`src/bsdsocket/socket.c:322`) gives every socket a uniform random 32-bit ISN —
that is §28.4's fix for the `|`-instead-of-`+` bias. A uniform ISN means the wrap
point is uniformly distributed over the connection's life, so a connection
carrying 4 GB wraps **exactly once**, whatever its ISN, and one carrying less
than 4 GB wraps with probability equal to its size over 4 GB. You cannot
arrange a wrap; you can only buy one with bytes.

**Does NetX Duo handle it?** Yes, and deliberately. Every sequence comparison
in the two files that matter uses the RFC 1982 signed-difference idiom rather
than an unsigned compare —
`nx_tcp_socket_state_data_check.c:366, 384, 636, 678, 696, 718, 738, 785, 887,
893` are all `(INT)(a - b)` — and
`nx_tcp_socket_state_ack_check.c:262–398` carries an explicit `wrapped_flag`
with a full case analysis of an ACK and a queued segment falling on either side
of the wrap. This is not a place where wrap was forgotten.

**So the honest state of this is: the arithmetic says one wrap per 4 GB, the
code says wrap is handled, and no run in this project has yet put 4 GB through
one connection.** The harness reports how far it got (`endreport.py` prints it
against the 4096 MB figure) so the question stays open with a number on it
rather than as a worry.

### 37.7 "mbuf fragmentation" is not a thing that can happen here

The report's first suspicion, and it is worth answering literally rather than
translating it into the nearest thing we do have.

**There are no mbufs in the shipped `bsdsocket.library` at all.** All eleven
`mbuf_*` LVOs — `-0x270` through `-0x2ac` — are `bsd_enosys`/`bsd_enosys_ptr`
stubs in `src/bsdsocket/bsdsocket_vectors.c:132-142`, unconditionally, and
`src/mbuf/` is not linked into the library. The emulation exists (and
`tests/mbuf_bpf` exercises it), but nothing in this stack's data path has ever
allocated one: `src/mbuf/`'s own header says so in its first paragraph — an
mbuf here does not own, wrap or reference an `NX_PACKET`, it comes out of a
**private slab allocator** with its own storage, and conversion is an explicit
copy at the API boundary. An application that never calls `mbuf_get()` cannot
fragment a pool it never touches, and in this build it cannot call it anyway.

**The resource that plays mbuf's part is the `NX_PACKET` pool**, and it cannot
fragment either: it is a fixed count of fixed-size blocks on a free list, so
it is either exhausted or it is not. That is why the timeline records
`nss_PoolFree` and `nss_PoolEmptyRequests` rather than anything shaped like a
fragmentation metric — and why §37.5's answer to "does the pool run out" is a
straight line to 1 rather than a distribution.

So the phenomenon the report names cannot occur here, and the phenomenon it is
standing in for — a stack that runs out of buffers and never gets them back —
**does**, by an entirely different route.

### 37.8 Does the pool scale with memory?

AmiTCP_NG claims to scale with available memory; §24 sizes the receive *window*
from the pool and the live socket count, but whether the **pool itself** adapts
is a different question and this is the answer.

`ami_ns_pool_packets()` (`src/netstack/netstack.c:197`):

```c
avail   = AvailMem(MEMF_PUBLIC);
packets = (avail / AMI_POOL_MEM_DIVISOR) / ami_ns_packet_stride();   /* /16 */
clamp(packets, AMI_POOL_MIN_PACKETS /* 16 */, AMI_POOL_MAX_PACKETS /* 256 */);
```

So: **yes between the floor and the ceiling, and not at all above it.** One
sixteenth of free public memory, in packets of 1568 bytes plus header, floored
at 16 and capped at **256**.

The stride is 1628 bytes — `sizeof(NX_PACKET)` is 56 in the default build,
measured with a `_Static_assert` bisect rather than counted off the header —
so the arithmetic puts the two bounds at

| | free public memory | pool |
|---|---:|---:|
| floor binds below | 416,768 B (407 KB) | 16 packets, 26,048 B |
| ceiling binds above | **6,668,288 B (6.4 MB)** | 256 packets, **416,768 B** |

and every machine measured in this document is above the ceiling:

```
[INFO] netstack: 10009928 bytes free, pool = 256 x 1568
```

So an A1200 with 8 MB of Fast RAM and an A4000 with 128 MB get **the same
416,768-byte pool**, it is sampled **once, at stack start**, and it is never
revisited. A machine that frees 100 MB after the stack comes up gets nothing
for it, and neither does one that is given a 256 MB Zorro III card.

That is a defensible design for the floor — §24.3 makes the same argument about
the 4 MB machine — and an undefended one for the ceiling. 256 is the number
`AMI_POOL_MAX_PACKETS` has always had; nothing in this document measured it.

### 37.9 What is in the tree

| | |
|---|---|
| `tests/endurance/endurance.c` | the harness: five workloads and the timeline sampler |
| `tests/endurance/fitz-kprintf.c` | `kprintf`, `mysnprintf` and 64-bit division for Fitz |
| `tests/endurance/fetch-fitz.sh` | fetch and unpack Fitz (not vendored, deliberately) |
| `tests/endurance/build.sh` | `Endurance`, host `fitz-serve`, and Fitz for m68k twice |
| `tests/endurance/run-fitz.sh` | the Fitz arm |
| `tests/endurance/run-endurance.sh` | the synthetic arm and the probes |
| `tests/endurance/endreport.py` | reads a timeline and says what trended |

Modes: `fitz` (files over a mounted share), `loop` (synthetic, both ends here),
`wire` (synthetic, a host peer), `leak` (one socket lifecycle, repeated) and
`watch` (sample only, while somebody else makes the traffic).

**What it deliberately does not capture, and why.** Retransmissions come from
`nsx_TcpRetransmits`, which the library already counts. **Duplicate ACKs do
not, and cannot**: they exist only in a packet trace, and `NetTrace` cannot
supply one for a run of hours — it owns its own workload and exits with it, it
drains the capture channel from inside that workload's loop, and the channel is
two buffers of at most `BPF_MAXBUFSIZE` = 32 KB which do not wrap but **drop**
(`src/bpf/bpf_channel.c:391`). At `SNAP=96` that is a few hundred records. A
long run's dup-ACK count would have to come from FS-UAE's own A2065 frame log
through `tests/trace/a2065pcap.py` and `tcpaudit.py`, which is hours of traffic
in hex on the host's disk. Named here rather than left as a gap somebody
rediscovers.

Two notes for whoever runs it next, both learned the expensive way:

- **`tools/fsuae-reap.sh` kills any `fs-uae` older than 15 minutes** and cannot
  tell a long tenant from an orphan. Both run scripts print the `-a` value that
  makes it safe.
- **A multi-hour run holds an emulator slot for multiple hours**, and
  `fsuae-run.sh`'s `.fsuae.perfwait` handshake means a queued `-x` measurement
  waits `AMINETXDUO_LOCK_WAIT` (40 minutes) and then *proceeds anyway*,
  silently contended. The runs here were shortened and given `-k 56` rather
  than left to block somebody else's measurement.

## 38. The Amiga answers an SSH connection (2026-07-26)

Dropbear's client has run here since §35. The server had never been built.

### It did not link, and the nine missing symbols were the interesting part

`clients/dropbear/build.sh -P "dbclient dropbear"` failed on `clearenv` and
`nanosleep` at compile time and then on nine symbols at link time:
`setuid`, `setgid`, `initgroups`, `getgrnam`, `chown`, `chmod`, `waitpid`,
`sigaction`, `environ_ptr`.

Not one of them was a build flag. They are the calls a server makes that a
single-user machine with no processes has no answer for, and each one has a
true answer rather than a convenient one:

| call | answer | why that is the truth and not a shortcut |
|---|---|---|
| `setuid`/`setgid`/`initgroups` | 0 for id 0, `EPERM` otherwise | `getuid()` is 0 and `getpwnam()` reports uid 0, so `svr_switch_user()` is switching from 0 to 0 — a no-op, which POSIX also reports as success. The comparison is there so a future nonzero uid gets refused rather than appearing to work. |
| `getgrnam` | `NULL` | Unlike `getpwnam`, which has one honest answer, there is no group that could be meant. |
| `chmod` | `SetProtection()` | Real. The RWED bits are active low, so the conversion is inverted. |
| `chown` | `ENOSYS` | Telling a caller it gave a file away would be worse than telling it we cannot. |
| `waitpid` | `ECHILD` | Not a stub: `fork()` always fails, so there has never been a child. |
| `sigaction` | 0, installs nothing | Failing is worse. `commonsetup()` treats an error as fatal, so `-1` refuses to start the server over handlers that cannot fire — `SIGCHLD` needs a child, and bsdsocket raises no `SIGPIPE`. |
| `environ_ptr` | defined, empty | `<unistd.h>` makes `environ` a macro for `(*environ_ptr)` and nothing in the toolchain defines it, so *touching* environ was a link failure. |

`clearenv` was the odd one. newlib really does have one, in
`lib_a-environ.o`; `<stdlib.h>` simply never declares it. Writing our own is a
multiple definition, and writing it in terms of `environ` does not work either
for the reason in the table. It needed a declaration, not an implementation.

`nanosleep` is `Delay()`, rounded up to a whole 20 ms tick. The server sleeps
250–350 ms after a failed password so that a bad user and a bad password cost
the same; one tick of granularity is coarser than that and still far finer
than the scheduler and round-trip variance the delay is hiding behind.

### Linking was not the same as working

`svr-main.c` forks per connection and treats a failed fork as *log a warning
and drop the connection*. So the honest `ENOSYS` above would have produced a
server that accepted every connection and instantly hung up — the exact
failure mode this project keeps finding, a well-formed thing that never runs.

`DEBUG_NOFORK` sets `fork_ret` to 0 instead of calling `fork()`, which takes
the child branch in the same process: it closes the listening sockets and runs
the session inline. One connection per invocation, then exit. That is the only
shape a machine without fork can offer, and it is a real server rather than a
broken one. Upstream's `DEBUG_` prefix is a misnomer here.

### Testing it needs loopback, and that is not a compromise

FS-UAE's SLIRP is outbound-only. Nothing on the host can open a connection
*into* the guest, so a server on the Amiga is unreachable from here by
construction. Running both ends inside the guest is not a way around that
limitation — it is the only arrangement that exercises `accept()` under
emulation at all, and it puts a real key exchange over the loopback interface
as a bonus.

`ClientRun` grew two directives to express it, because a list of synchronous
`SystemTagList()` calls cannot have one command still running while the next
starts:

    &<command>   start it, do not wait; output to DH0:server.txt
    wait <n>     Delay(n seconds)

### Two defects between accept and a session

**`accept()` did not exist.** `amiga_dropbear.c` wraps every bsdsocket call
into the merged descriptor space, and `accept` was the one call dbclient never
makes. It refuses an out-of-window descriptor the same way `socket()` does,
which matters more on a listener — that is where high descriptors come from.

**`pipe()` handed out one pair.** Enough for dbclient, not for a server:
`svr-main.c` makes a childpipe per connection and `common-session.c` then makes
`ses.signal_pipe`, so the second call failed and the session died with
`Early exit: Signal pipe failed` — *after* a successful accept, which made it
read as an accept problem. Eight pairs now, released when both ends close,
since freeing on the first would hand a live end to the next caller.

### `SYS:/.ssh/authorized_keys` is not a path

Every ported program builds a path by writing `"%s/.ssh/..."` after the home
directory `getpwnam()` gave it. On AmigaOS a home is a device or assign name
ending in a colon, so that produces `SYS:/.ssh/authorized_keys` — and a colon
followed by a slash means the *parent of the root*, which does not exist. The
`Lock()` fails and the program reports the file as missing rather than as
misspelled.

`amiga_fix_path()` collapses `":/"` to `":"` once, where paths enter: `stat`,
`lstat`, `mkdir`, `unlink`, `chmod` and Dropbear's `open` wrapper. The sequence
has no other valid meaning in an AmigaOS path, so there is nothing to lose.

This is general, not a Dropbear fix. Any ported program that joins `$HOME` to a
subpath hit it.

### The result

    [2250456] Not backgrounding
    [2250456] Child connection from 127.0.0.1:59349
    [2250456] Pubkey auth succeeded for 'amiga' with ssh-ed25519 key
              SHA256:6b5CjYkFkDq2vMunRKHPApC/W1sNI1S7LJWDfmB4xsU from 127.0.0.1:59349

Our `dropbear` accepting from our `dbclient`, through our `bsdsocket.library`,
on one 14 MHz 68020. 22.02 s for the exchange, which is *both* halves of the
cryptography on one CPU — §35 measured the client half alone at about 12 s, and
this is consistent with that.

The server also found `authorized_keys` through the home directory, which means
it resolved a path that does not exist as written. That is `amiga_fix_path()`
being exercised rather than asserted.

### What still does not happen

The shell. Auth succeeds and the session opens; `execv()` fails, so the command
never runs and the client gets a clean close with nothing on stdout. That is
what `TCP:` (§34) was written for, and it is the next piece.

Also not done: host-key entropy on a machine with no clock and no user input,
which is a real question for a server and not for a client.

### What shipped

v0.4.0 carries `Clients/curl` and `Clients/ssh` and **not** the server. It
links, it accepts, it authenticates, and it cannot yet give anybody a shell —
which is not a thing to put in an archive labelled as an SSH server.

### 37.10 The Fitz arm: eleven minutes, 224 MB, and one counter that moved

**Stopped early, deliberately, and that is the first thing to say.** The run
was two hours at `-k 56`; it was killed at eleven minutes because a timing
measurement had been queued behind it on the exclusive lane and
`AMINETXDUO_LOCK_WAIT` would have let that measurement start *anyway*,
contended, at the 40-minute mark. A soak loses elapsed time when it is
restarted; a contended timing produces a number that looks valid and is not.
The soak is the one that yields. What follows is therefore 11 minutes, not the
hours this section is about, and it is reported as what it is.

The workload: `fitz-serve` on the host, `fitz mount 10.0.2.2:17711 FITZ:` on
the guest, two `Endurance` filer Processes writing files of log-uniform size
(1 byte to 512 KB) to the share and reading every one back, in chunks whose
size is redrawn for every `Write()` and every `Read()`.

| 660 s, `-k 56`, `fitz-debug` (`ADEBUG=5`) | |
|---|---|
| payload moved | **112 MB out, 112 MB in** |
| transactions | 2,909 |
| **corrupted bytes** | **0** |
| **`* EAGAIN` on Fitz's blocking socket** | **0** |
| Fitz `send`/`recv` failures, `recv maxretry` | **0** |
| errno events recorded by the harness | **0** |
| TCP connections opened, whole run | **1** |
| live sockets | 4, constant |
| packet pool free (of 256) | 222 → 221, range 198–222, **`pool_empty_requests` 0** |
| `AvailMem` | 9,127,448 → 9,094,320, oscillating over a 66 KB band, **no trend** |
| largest free block | 7,053,024 → 7,020,048, same band, **no trend** |
| TCP retransmissions | **0** |
| TCP checksum errors, IP receive drops, IP send drops | **0** |

Twenty-three samples, and the only column with a slope is inbound segments
dropped as out-of-window.

**The one counter that moved: `nsx_TcpReceiveDropped`, 0 → 1,301, a steady
~2.0/s.** It is worth being careful about what that is, because it is easy to
read as loss and it is not: our own retransmission count is **zero** for the
whole run, no checksum failed, and every one of 224 MB was verified against
the pattern. The counter's dominant increment site is
`nx_tcp_socket_packet_process.c:211`, which drops a segment judged outside the
receive window **and sends an immediate ACK** (RFC 793 §3.9). Two conditions
above it can produce that, and the one that fits a mounted file share is the
data branch with `rx_window == 0`: Fitz reads on demand, so between a request
going out and the application's next `Read()` the socket's advertised window
can genuinely be zero, and the peer's data arriving in that gap is refused and
re-sent by *the peer* — which our retransmit counter, by definition, does not
see. That is work redone on the wire, at about half a segment per transaction.
It is an observation with a rate attached, not a defect, and it is recorded
because §16.4 and §24.4 both report "zero drops" from workloads that drained
continuously and would not have produced it.

**And the `-k` assumption, established rather than relied on.** The premise of
running at a raised clock is that the failure being hunted is a function of
bytes and packets rather than seconds. The same workload at both clocks:

| A1200 profile | payload | combined rate |
|---|---:|---:|
| 14 MHz, stock | 12 MB in 180 s | 68 KB/s |
| 56 MHz, `-k 56` | 224 MB in 660 s | **347 KB/s** |

**5.1× for a 4× clock**, so the Fitz path is CPU-bound end to end and the clock
buys bytes at least linearly. The figure is above 4× rather than at it because
the 14 MHz arm is a three-minute run in which mount setup is a much larger
share of the window; the defensible claim is "at least linear", not "5×".
Eleven minutes at `-k 56` is worth about **an hour** of a stock A1200, which is
what makes a soak of this shape affordable at all.

**Where it got to on the questions this section asks.** 112 MB in one direction
on one connection is 2.7% of the 4,096 MB a 32-bit sequence number covers, so
§37.6's wrap was not approached. `AvailMem` and the packet pool are flat, and
the socket count is constant at 4 — which is the useful negative next to
§37.5: **the leak does not touch a workload that opens one connection and keeps
it.** Fitz's mount is exactly that, and eleven minutes of it moved 224 MB
without losing a byte or a packet.

**Not concluded.** Eleven minutes is not hours, and the report this section
comes from describes a failure at several hours. The run should be re-armed —
and it is more useful re-armed *after* the `src/bsdsocket/socket.c` fixes for
§37.4 and §37.5 land, because then it tests the stack somebody would ship
rather than the one that was measured.

## 39. The bracket was 790 µs, and curl takes 108 of them (2026-07-26)

§29.3 left this project with two instruments disagreeing about the same wire:
our own `NetTrace` made this stack **55% faster** than Roadshow 1.15 and the
stack-agnostic Aminet curl made it **12% slower**, five runs out of five, with
the gap scaling with the body rather than sitting in setup. §32.11 read the
receive path while fixing something else and named a candidate without changing
it: `bsd_nx_enter()`/`bsd_nx_leave()` bracket every `recv()`, every `send()`
and every poll pass inside `WaitSelect()`, and `src/bsdsocket/netx_call.c`
prices one bracket at an `AllocSignal()`, a `_tx_thread_create()`, a baton
acquire and their inverses. curl reads small and selects between reads;
`NetTrace` reads 4,096 bytes through one `WaitSelect()` loop. A per-call
constant is exactly the shape of "we win the first byte and lose everything
after it".

**The bracket is real, it cost 790 µs, it is now 268 µs — and it was never the
reason we lose to Roadshow.** One 1.2 MB curl fetch takes **108** brackets, not
the thousands the mechanism assumed, so the whole of that saving is 76
milliseconds of a 17-second fetch and the throughput A/B is a flat line. The
same change moves `NetTrace`'s loopback figure by 12.8%, which is what 380
brackets at 522 µs comes to — so the mechanism is right about everything except
the client that loses. That is the sixth predicted bottleneck this project has
had overturned by measurement in three days, and this section is written that
way round on purpose.

### 39.1 What one bracket costs, on a 14 MHz 68020

`tests/perf/bracket_test.c`, new here, and it is a sibling of
`tests/perf/perf_test.c` rather than a part of it: it needs no netstack
singleton, no configuration file and no SANA-II device, so it builds on every
profile. `ReadEClock()` is the time base (709,379 Hz, 1.409 µs a tick) and the
measurement's own cost is calibrated out of every figure, the same way §24's
census does it. Only the A1200 profile is quoted, for the reason
`tests/perf/cpucal.c` measures.

Five runs of the unmodified port, the pair timed in halves:

| | |
|---|---:|
| `tx_amiga_adopt_thread` + `tx_amiga_orphan_thread` | **776–790 µs** |
| ... the adopt half | 224–242 µs |
| ... the orphan half | 539–564 µs |
| ... `AllocSignal()` + `FreeSignal()` inside it | **16–19 µs** |
| ... one `_tx_amiga_wake_scheduler()` poke | **202–204 µs** |
| ... the nested path, `tx_thread_identify()` | 20 µs |
| ... one `Forbid()`/`Permit()` pair, for scale | 9.6 µs |

Two of those rows decide what the fix is.

**The signal is not the cost.** `netx_call.c` named `AllocSignal()` first and it
is 2% of the bracket. What is expensive is `_tx_thread_create()` on the way in
and `_tx_thread_terminate()` + `_tx_thread_delete()` on the way out.

**The scheduler poke is a quarter of it.** `_tx_amiga_wake_scheduler()`
`Signal()`s the scheduler Task, which on Exec is an immediate switch to a
higher-priority task that finds nothing to dispatch and goes back to sleep —
two context switches, 202 µs, at the end of *every* orphan.

### 39.2 Two changes, and what each is worth

**Skip the poke when there is nothing to dispatch.** `_tx_thread_execute_ptr ==
TX_NULL` means no ThreadX thread is ready, so waking the scheduler cannot
dispatch anything. It cannot lose a dispatch either: every path that makes a
thread ready afterwards wakes the scheduler itself, from an interrupt
(`tx_thread_context_restore.c`) or from whoever holds the baton
(`_tx_thread_system_return`). Read inside the `Forbid()` that lowers the system
state, so the answer cannot go stale between the test and its use. The
adopt/orphan pair goes **790 → 596 µs**, all of it out of the orphan half
(551 → 367).

**Keep the TX_THREAD between calls.** The baton must be given back per call —
an adopted task that holds it across application code stops the IP thread, the
timer and every other socket user, and that is not negotiable. The
*registration* is a different thing and is repeatable: a base belongs to one
task (`library.c` records it in `sb_Task`) and that task gets the same
`TX_THREAD` every time. `tx_amiga_adopt_suspend()`/`tx_amiga_adopt_resume()`
leave the thread `TX_SUSPENDED` between brackets — on no ready list,
dispatchable by nobody, with the baton free — and only hand the baton back and
forth. Measured: **268 µs**, resume 134 and suspend 134.

So the bracket is **2.9× cheaper** and the invariants are untouched:
`NX_THREADS_ONLY_CALLER_CHECKING` still sees a real `TX_THREAD`, and the baton
is still taken and released on every call.

**The lifetime this does not close, stated because it is the reason to think
twice.** A Task that exits without closing the library leaves a suspended
`TX_THREAD` in ThreadX's created list whose `tx_thread_amiga_task` points at
freed memory. Nothing dispatches a suspended thread and nothing else resumes
one, so it is inert — but only for as long as that stays true.
`bsd_child_destroy()` releases it, which is the one path that runs on the
owning task with every socket already closed; a base torn down by anyone else
gets `tx_amiga_discard_thread()`, which removes the registration and leaves the
Exec signal alone because only its owner may free it. A base used from a second
task now fails with `ENETDOWN` rather than working by accident: there is
nowhere to put a second adoption, and `src/bsdsocket/tcp_handler.c` already
states the rule ("a SocketBase belongs to one task").

### 39.3 What it does to a transfer, before anyone else's client is involved

256 KB over loopback, sent by a native ThreadX thread and drained by an Exec
Task, at four read sizes and four bracket policies. `per-call` is what
`transfer.c` does; `cached` is the same with the change above; `on-miss`
brackets only the calls that actually reach `nx_tcp_socket_receive()`; `once`
is one bracket for the whole transfer and is the floor.

| read | per-call | cached | on-miss | once |
|---:|---:|---:|---:|---:|
| 512 | 300 KB/s | **374** | 460 | 512 |
| 1,460 | 411 | **451** | 480 | 535 |
| 4,096 | 473 | **492** | 492 | 548 |
| 16,384 | 517 | **524** | 493 | 557 |

At 512-byte reads the change is worth **+25%**, and the per-call column is
already 852 ms where the unmodified port was 947. The two columns converge as
the read grows, which is the whole claim: this is a per-CALL constant. At
16 KB reads it is worth 1%.

`on-miss` is in the table because it is the next idea and it is **not taken**:
it drops the bracket for a `recv()` that can be served out of the packet
already parked on the socket, which is arithmetic and a copy — but it also
moves `nx_packet_release()` outside the baton, and releasing a packet can
resume a thread suspended on the pool. That is precisely what the baton exists
to serialise. It is worth 86 KB/s at 512-byte reads and it is not worth
that.

### 39.4 And then the count, which ends the theory

`-DAMINETXDUO_NXCENSUS=ON` counts the brackets `bsdsocket.library` takes and
times them, reporting both to the serial log when a base closes. It exists
because §29.3's follow-up experiment was named and never run, and because
inferring a call count from a throughput delta is how a prediction gets
confirmed instead of tested.

One fetch of 1,200,000 bytes over `http://` with the Aminet `curl.020`
8.22.0-DEV — the third-party binary, nothing of ours in it — repeated twice per
boot:

| | brackets | in `enter` | in `leave` |
|---|---:|---:|---:|
| per-call (`-DAMINETXDUO_NXCACHE=OFF`) | **108** | 35 / 34 ms | 2,376 / 2,378 ms |
| cached | **108** | 24 / 22 ms | 2,311 / 2,272 ms |

**108 brackets for 1.2 MB.** curl is not making thousands of short reads with a
`select()` between them: it is averaging **11 kB per library call**, because on
this machine it spends most of the fetch outside the stack — writing the body
out — while the receive queue fills behind it. §32.11's "twice per chunk" is
wrong by more than an order of magnitude.

**The `leave` column is not bracket overhead and must not be read as any.** The
uncontended suspend is 134 µs (§39.1); 21 ms is what releasing the baton
actually does — it hands the CPU to the ThreadX scheduler, which dispatches the
IP thread, which does all the inbound processing it could not do while an
application task held the baton. That is the stack working, charged to whoever
happened to open the gate.

So the honest accounting of the bracket in one curl fetch is `enter` plus the
suspend: **35 ms before, 24 ms after, out of ~17,000 ms**. 0.2% of a fetch.

### 39.5 The A/B on the honest instrument, which is a flat line

`tests/compare/run-compare.sh -s ours|roadshow -w curl`, `AMINETXDUO_PERF=1`
(the measurement lane of 81b8f8d), A1200, 1,200,000 bytes, `CurlCheck`'s own
50 Hz tick count, both fetches of each boot:

| | fetch 1, ticks | fetch 2, ticks | fetch 2 |
|---|---:|---:|---:|
| ours, clean `HEAD` (`b0dd15f`) | 863 | **825** | 16.50 s = 72.7 kB/s |
| ours, with §39.2 | 861 | **825** | 16.50 s = 72.7 kB/s |
| Roadshow 1.15 | 782 | **746** | 14.92 s = 80.4 kB/s |

**Nothing moved.** Two ticks on the first fetch and zero on the second, against
a predicted 76 ms — which is four ticks, i.e. below what this instrument can
see. The census says why, and the two agree to within the resolution of the
clock. Roadshow is **10.6% ahead**, in the same direction and about the same
size as §29's 14%, so the finding reproduces; the bracket is not what it is
made of.

**The absolute numbers in that table must not be quoted against §29's.** Every
arm here is roughly 1.6× slower than the same workload in §29 (10.72 s ours,
9.39 s Roadshow). The `-x` lane takes the *emulator* alone; it does not take
the Mac, and other workstreams were building on it throughout. The three arms
above were taken inside one hour under the same conditions, so the comparison
between them holds and the comparison with §29 does not.

### 39.6 The other instrument, which does move

`NetTrace` is the arm §29.3 said was 55% faster than Roadshow, and it reads
4,096 bytes at a time through one `WaitSelect()` loop -- so 524,288 bytes is
about 128 receives and twice that many poll passes, call it 380 brackets where
curl makes 108 for more than twice the payload. If the diagnosis has any
predictive power left, this is where it has to show.

`run-compare.sh -w bench`, one `NetTrace` binary staged unchanged against all
three libraries, `AMINETXDUO_PERF=1`, 524,288 bytes, the command's own elapsed
time (which *is* the transfer -- `NetTrace` does one and exits):

| | loopback | | wire | |
|---|---:|---|---:|---|
| ours, clean `HEAD` | 1.58 s | 324 KB/s | 4.46 s | 115 KB/s |
| ours, with §39.2 | **1.40 s** | **366 KB/s** | 2.98 s | 172 KB/s |
| Roadshow 1.15 | 2.08 s | 246 KB/s | 4.52 s | 113 KB/s |

**Loopback moves by 180 ms, and 380 brackets at 522 µs saved is 200.** That is
the mechanism doing exactly what it says on the workload it was described for,
and it is the closest thing to a confirmation in this section.

**The wire row is not a measurement**, for the reason §32.8 gives: 2.68 s and
2.98 s came out of the same library in the same boot, so a 33% column is noise.
Loopback is the solid one and always has been.

**So the two instruments still disagree, and now they disagree more.** After
this change `NetTrace` makes us 1.49× faster than Roadshow on loopback and the
Aminet curl makes us 10.6% slower on the wire. The gap between them did not
close, which was the outcome that would have proved the diagnosis; what closed
instead is the question of *why* they differ -- they are counting different
things, 380 brackets against 108, and the one that counts more of them
improved by exactly what the arithmetic predicts. The bracket was a real cost
on `NetTrace`'s pattern and never a large one on curl's.

### 39.7 What is left, and where it is not

* **It is not the adoption.** Priced, reduced by 2.9×, and worth 0.2% of the
  workload that loses.
* **It is not the copy path.** §32.11 read it and found no bounce buffer;
  nothing here contradicts that.
* **It is not `select()`.** curl makes 108 library calls in total for 1.2 MB,
  so `WaitSelect()` cannot be a large share of anything, and §35 put it at 1.7%
  of an SSH handshake from the other direction.
* **The IP thread costs 2.3 s per 1.2 MB**, measured as the time an application
  task does not get back after releasing the baton. That is 1.9 µs a byte, and
  it is the largest single stack-side number in this section. Whether Roadshow's
  equivalent is cheaper is the question §29.5's ICMP result already hinted at
  ("our per-packet path costs more than Roadshow's") and nothing here has
  measured it.
* **The tick task stalled for 743–761 ms in every arm**, ours and the control
  alike — `tick: stalled 745 ms, dropping 29 of 37 ticks (cap 8)` — which means
  TCP's own timers stopped advancing for three quarters of a second in the
  middle of a transfer. It is present with and without this change, it is not
  caused by it, and nobody has looked at it.

### 39.8 The instruments, kept

Both switches stay in the tree, and both are off by default:

* `-DAMINETXDUO_NXCENSUS=ON` — the bracket count and its time, per base, to the
  serial log. It puts two `ReadEClock()` calls around the thing it measures, so
  it is not something to ship.
* `-DAMINETXDUO_NXCACHE=OFF` — the control arm, restoring the per-call
  adopt/orphan. Two libraries out of one tree differing in one decision is how
  §39.4's table was taken, and it is the only way to take it again once the tree
  has moved.

`tests/perf/bracket_test.c` is the third, and it is the one to run first the
next time somebody is sure they know what a call costs.

### 39.9 The cover

Every harness in the tree, against the change, on the same machine. Each line
is the figure the same suite gives on clean `HEAD`.

| | |
|---|---|
| conformance, `LOOPBACK` | 130 passed, 0 failed, 12 skipped |
| conformance, `HOST 10.0.2.2` | 141 passed, 1 failed (`accept()`, the SLIRP inbound path of §12) |
| `tests/clients` | 94 checks, 0 failures |
| `tests/tcpdrill` | 26 cases, 0 failed; 210 checks, 0 failed |
| `tests/curl` A–F | 147 passed, 2 failed (`a44_cookies_send`, `f07_ftp_active`) |
| `tests/curl -p`, 8…48 | 9 passed, 0 failed, `AvailMem` delta **+0** |
| `tests/libraries` | 8 checks, 0 failures |
| `tests/tools/run-livetools.sh` | 23 ok, harness `FAIL` — the empty-serial-log false red of §32.8.1 |
| `tools/ci.sh` (host + all four cross configs) | green |

The concurrency sweep is the one that matters most for a change of this shape,
because a cached registration that were wrong about task identity would show up
there first and nowhere else. `p05_parallel_48` is green with no memory moved.
