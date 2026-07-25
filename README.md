# AmiNetXDuo

An AmiTCP/Roadshow-compatible `bsdsocket.library` for classic AmigaOS, built on
[Eclipse ThreadX NetX Duo](https://github.com/eclipse-threadx/netxduo).

> **Status: it works, and it is not finished.** The stack gets a DHCP lease,
> answers ARP, pings its gateway, resolves DNS and moves TCP in both directions
> on an emulated 68020/68030 under Kickstart 3.1. `bsdsocket.library` scores
> **125/142** on the [`bsdsocktest`](https://github.com/tbdye/bsdsocktest)
> conformance suite (133/142 on the network tier). Not yet run on real hardware,
> and not yet something to trust with your data.

## Why

AmigaOS has never had a TCP/IP stack in the OS — networking is a third-party
shared library. The options are all compromised: AmiTCP 3.0b2 is free but from
1994, AmiTCP 4.x and Miami are proprietary and effectively unobtainable, and
Roadshow — the one genuinely modern stack — is commercial and closed.

AmiNetXDuo pairs an industrial-grade, MIT-licensed embedded TCP/IP core with
the AmigaOS socket ABI every existing Amiga network program already speaks,
over the SANA-II driver interface every existing Amiga network card already
implements.

What that combination buys, none of which the classic stacks have:

- **MIT licence throughout** — no 4.4BSD or GPL lineage anywhere in the tree.
- **IPv6** — the dual stack builds with `-DAMINETXDUO_IPV6=ON` and works: ICMPv6,
  neighbour discovery, duplicate address detection, stateless autoconfiguration
  from router advertisements, TCP and UDP over IPv6, and `AF_INET6` sockets
  through `bsdsocket.library`. As far as we can establish, an Amiga first — no
  classic Amiga TCP/IP stack has had IPv6. Off by default so the floor build
  stays small.
- NetX Duo's protocol catalogue: DHCP, DNS and AutoIP are in use today; PPP,
  PPPoE, SNTP, mDNS and NAT are vendored and unused so far.

On TLS, honestly: `nx_secure` compiles and completes a real TLS 1.2 handshake,
but that handshake costs **185 s** on a 68020 — so it is off by default and
offload (`catalyst`, `AmiSSL-Tunnel`) is the realistic path. Optimised bignum
arithmetic in `src/crypto68k/` made RSA-2048 **8× faster**, which moves the
bottleneck to elliptic-curve work rather than removing it. See
[docs/RESEARCH.md §9](docs/RESEARCH.md#9-decisions-2026-07-24).

## How it fits together

```
 Application task                          Application task
      │ OpenLibrary("bsdsocket.library", 4)
      ▼
 bsdsocket.library    per-opener child bases · WaitSelect · socket events
      │ native NetX Duo API
      ▼
 NetX Duo core        IPv4/IPv6 · TCP · UDP · ICMP · ARP  (+ DHCP, DNS)
      │ NX_IP_DRIVER
      ▼
 sana2 shim           cooked ↔ Ethernet framing · CopyTo/FromBuff hooks
      │ exec IORequests
      ▼
 SANA-II device       a2065 · ariadne · xsurf · cnet · ppp · uaenet · …

 ThreadX + Exec port  TX_THREAD ↔ struct Task · baton scheduling · 100 Hz tick
```

The two hard parts are [ThreadX on Exec](docs/RESEARCH.md#62-threadx-on-exec-the-central-problem)
— NetX Duo reaches into ThreadX internals to suspend socket callers, so a real
kernel is required rather than a shim — and the
[SANA-II framing mismatch](docs/RESEARCH.md#34-sana-ii--the-driver-contract):
NetX Duo wants Ethernet headers, SANA-II hides them.

## Target

68020 + AmigaOS 3.1 + 4 MB. IPv6, TLS and the `bpf_*` subsystem are build
options so the floor build stays small. Measured code size on m68020 `-O2`:
ThreadX core 27.7 KB, NetX Duo IPv4 core ~73 KB, DHCP client 15 KB, DNS 9.5 KB.

## Building

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake
cmake --build build --parallel
```

The toolchain defaults to `~/amigaos/tools/m68k-amigaos-gcc` (GCC 15.2 for
m68k-amigaos); override with `-DAMIGA_TOOLCHAIN_ROOT=<path>`.

## Testing

`tools/fsuae-run.sh` runs an AmigaOS executable under FS-UAE on a real
Kickstart 3.1 A1200, captures `ami_log()` serial output, and propagates the
program's exit status back to the host:

```sh
tools/fsuae-run.sh -t 90 build/cm/tools/smoke/smoke
```

`-c 68030` selects a full 68030 (which has an MMU, so Enforcer works there;
the 68020 floor runs with no illegal-access checking at all). `-n` attaches an
emulated A2065 on SLIRP for real networking.

`tools/smoke/` holds six diagnostic probes, built by the `smoke_probes` target
and deliberately not registered with `ctest`: a harness self-test, an entropy
pool probe, a ThreadX task lifecycle probe, a kernel-stop survival test, and
two that fault on purpose (`crashtest` jumps to `0x2`, `gurutest` double-frees)
to prove the crash guard and the `Alert()` hook report what they should.

The conformance target is [`bsdsocktest`](https://github.com/tbdye/bsdsocktest),
a 142-test suite for `bsdsocket.library` implementations, vendored as a
submodule:

```sh
tests/conformance/build.sh
tests/conformance/run-fsuae.sh -a "LOOPBACK NOPAGE"
```

### Where it stands

| | |
|---|---|
| conformance, loopback tier | **125/142** (1 fail, 16 skip) |
| conformance, network tier | **133/142** |
| ThreadX-on-Exec soak | 98 checks, 4+ adopted tasks, Enforcer-clean on 68030 |
| TCP throughput | 262 KB/s loopback, 310 KB/s to a host over SLIRP |
| IPv6 (`-DAMINETXDUO_IPV6=ON`) | ICMPv6 + TCP + UDP between two `NX_IP` instances (78 checks); `AF_INET6` sockets over `::1` through the library ABI; ICMPv6 to the host across an emulated A2065, with a router advertisement and stateless autoconfiguration |

Verified on 68020 and 68030. The single remaining loopback failure is a
deliberate disagreement: the suite skips `SOCK_RAW` only on `EACCES`, but
`EACCES` means "you lack privilege", which is untrue on an OS with no privilege
model — `ESOCKTNOSUPPORT` is the honest answer, so that test stays red.

### Debugging

There is no memory protection, so a bad pointer takes the machine down with no
output. `include/aminetxduo/crashguard.h` provides two things worth arming in
any test: `ami_crash_install()` catches CPU exceptions and dumps the exception
name, PC, SR and all registers to the serial log, and
`ami_crash_install_alert_hook()` intercepts Exec `Alert()` so a Guru arrives
decoded ("FREEING MEMORY ALREADY FREED") with the offending task named, rather
than as a hex code on a dead screen.

## Compatibility posture

AmiNetXDuo is an independent implementation of a *published ABI*. No AmiTCP,
AROSTCP, Miami or Roadshow code is used, copied or disassembled. Olaf Barthel's
freely-distributable Roadshow SDK headers and autodocs are used as the ABI
reference only — function offsets, tag values, structure layouts and documented
behaviour.

## Prior art

Two other modern-stack projects appeared in July 2026 and are worth knowing:
[lwip-amiga](https://github.com/rondoval/lwip-amiga) (lwIP + `bsdsocket.library`,
but a custom `netdev` driver ABI rather than SANA-II — PiStorm/Emu68 only) and
[AmiTCP_NG](https://github.com/MW0MWZ/AmiTCP_NG) (a GPL fork of AmiTCP 3.0b2
with a clean-room Roadshow ABI). Full survey in
[docs/RESEARCH.md §2](docs/RESEARCH.md#2-prior-art).

## Licence

MIT. ThreadX and NetX Duo are MIT (© Microsoft / Eclipse ThreadX contributors)
and are consumed as unmodified git submodules.
