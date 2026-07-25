# AmiNetXDuo

An AmiTCP/Roadshow-compatible `bsdsocket.library` for classic AmigaOS, built on
[Eclipse ThreadX NetX Duo](https://github.com/eclipse-threadx/netxduo).

> **Status: early construction.** The research and feasibility work is complete
> (see [docs/RESEARCH.md](docs/RESEARCH.md)); the stack does not run yet. Nothing
> here is usable for networking.

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

- **IPv4 + IPv6 dual stack** — an Amiga first.
- **MIT licence throughout** — no 4.4BSD or GPL lineage anywhere in the tree.
- NetX Duo's protocol catalogue for free: DHCP, DNS, PPP, PPPoE, SNTP, mDNS,
  NAT, AutoIP, and `nx_secure` TLS.

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
tools/fsuae-run.sh -t 90 build/smoke
```

The conformance target is [`bsdsocktest`](https://github.com/tbdye/bsdsocktest),
a 142-test suite for `bsdsocket.library` implementations.

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
