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
- **A data path written for the 68020.** `src/net68k/` replaces NetX Duo's IP
  checksum with the `add.l`/`addx.l` carry chain the machine actually has
  (3.11× on the primitive) and `memcpy` with a `movem.l` loop (1.23×), which is
  most of the 261 → 356 KB/s the throughput row below records. The vendored
  files are untouched: the symbols are simply resolved from our archive
  instead. Same arrangement as `src/crypto68k/`.

On TLS, honestly: `nx_secure` completes a real TLS 1.2 handshake, and with the
optimised arithmetic in `src/crypto68k/` wired into it the client-side cost on a
68020 is **3.2 s** of public-key work — a handshake against a real public HTTPS
server, chain verification and all, measured at **6.8 s**. The gate figure of
185 s was a loopback total dominated by the *server* half, which a client never
performs; that same measurement is now 26.7 s.

It is still off by default, for a reason that is no longer speed: **nothing can
open a TLS connection yet.** `-DAMINETXDUO_TLS=ON` builds the libraries and the
tests but links nothing into `bsdsocket.library`, and a client that is to reach
arbitrary sites needs a trust store nobody has built. See
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

If you have no m68k toolchain, `tools/fetch-toolchain.sh` downloads the pinned
one (GCC 15.2 + NDK 3.9, 93 MB) into `~/.cache/aminetxduo/toolchain`. The build
finds it there without being told. Otherwise the search order is
`-DAMIGA_TOOLCHAIN_ROOT` → `$AMIGA_TOOLCHAIN_ROOT` → that cache →
`m68k-amigaos-gcc` on `$PATH` → `/opt/m68k-amigaos` →
`~/amigaos/tools/m68k-amigaos-gcc`.

The pinned toolchain is a **Linux x86-64** build. On any other host, install or
build one and point `AMIGA_TOOLCHAIN_ROOT` at it; the layout to match is
`<root>/bin/m68k-amigaos-gcc` plus `<root>/m68k-amigaos/ndk-include`.

### Versioning

The version is **compound**: ours, plus the stack we are built on. An `.lha` on
Aminet and a CI artefact both have to say exactly what they are, and "AmiNetXDuo
0.1.0" alone does not answer "on which NetX Duo?".

```
0.1.0+nx6.5.1                                        artefact and archive names
AmiNetXDuo 0.1.0 (NetX Duo 6.5.1, ThreadX 6.5.1)     what a person reads
```

`project(AmiNetXDuo VERSION ...)` in `CMakeLists.txt` is the only place our
version is written down. The NetX Duo and ThreadX versions are **read** from
`third_party/*/common/inc/*_api.h`, never typed in: `cmake/AmiNetXDuoVersion.cmake`
checks them against the pins it records and refuses to configure if a submodule
bump has left them stale, because an artefact that misnames its own contents is
worse than one with no version at all. It generates
`<aminetxduo/version.h>` into the build tree.

`tools/version.sh` answers the same questions from a shell — CI has to be able
to name an artefact before it has built anything, and cannot run an m68k binary
to ask. The `version_scheme` host test keeps the two implementations honest.

Release tags are plain `vX.Y.Z`. The commit count belongs in a binary's version
output and nowhere else — not in a tag, a release name or an archive filename.

## Testing

Everything CI does is `tools/ci.sh`, so it can be run before pushing:

```sh
tools/ci.sh                 # host tests, all four cross builds, conformance build
tools/ci.sh host            # just the host tests (no cross toolchain needed)
tools/ci.sh emulator        # the on-Amiga harnesses under FS-UAE
```

A first run with nothing installed fetches the toolchain itself; a warm run of
the whole of tier 1 is about a minute. The workflows in `.github/` call this
script and add nothing but caching and scheduling, so a green tick there and a
green run here mean the same thing.

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
| TCP throughput | **356 KB/s** loopback, **368 KB/s** to a host over SLIRP (was 261 / 312 before `src/net68k/`) |
| IPv6 (`-DAMINETXDUO_IPV6=ON`) | ICMPv6 + TCP + UDP between two `NX_IP` instances (78 checks); `AF_INET6` sockets over `::1` through the library ABI; ICMPv6 to the host across an emulated A2065, with a router advertisement and stateless autoconfiguration |

Verified on 68020 and 68030. The single remaining loopback failure is a
deliberate disagreement: the suite skips `SOCK_RAW` only on `EACCES`, but
`EACCES` means "you lack privilege", which is untrue on an OS with no privilege
model — `ESOCKTNOSUPPORT` is the honest answer, so that test stays red.

### Continuous integration

Two workflows, deliberately separate, because a green tick on one must never be
read as a claim about the other.

**`.github/workflows/ci.yml` — tier 1, runs on every push.** Needs nothing but
a network connection.

| | |
|---|---|
| toolchain | `tools/fetch-toolchain.sh` — GCC 15.2 + NDK 3.9, pinned by the sha256 of the layer it comes out of, cached |
| cross builds | default, `-DAMINETXDUO_IPV6=ON`, `-DAMINETXDUO_TLS=ON`, `-DAMINETXDUO_CRYPTO68K_ASM=OFF` — all four, because each has broken while the others built |
| warnings | `-Wall -Wextra -Werror` on our sources, vendored code exempt (`cmake/ci-warnings.cmake`) |
| host tests | 5 suites through `ctest`: config parsers (157 checks), mbuf chains (206), BPF filter VM (201), crypto68k vectors (4,964 — RSA-2048 known answers plus a differential against the vendored bignum code), net68k checksum (10,030 — a differential against the vendored checksum over every length, alignment and packet chain) |
| host compilers | GCC on Linux and clang on macOS, so neither becomes the only one that works |
| conformance | `bsdsocktest` is compiled for m68k; running it is tier 2 |

**`.github/workflows/emulator.yml` — tier 2, the on-Amiga harnesses.** These
need a boot ROM, and there are two of them:

*The AROS m68k ROM.* AROS is an open-source AmigaOS reimplementation, APL 1.1,
freely redistributable — and it boots this project's FS-UAE harness. Measured
2026-07-25 against Kickstart 3.1 40.68 on the same binaries, the check counts
are identical: `smoke` 5, `lifecycle` 18, `KernelStop` 8, `ram_driver_test` 32,
`mbuf_bpf_test` 154, `soak_test` 98 — six harnesses, 315 checks, no failures,
one to two seconds slower to boot.
`RemTask()` freeing `tc_MemEntry`, `Forbid()` nesting across `Wait()`,
`timer.device`, `RawPutChar()` serial output and the `Alert()` hook all behave
as the code expects. `tools/fetch-aros-rom.sh` downloads it, and
`tools/fsuae-run.sh` takes the second half through `AMINETXDUO_KICKSTART_EXT`
(AROS is a 512 KB base ROM *plus* a 512 KB extended ROM — booting the base
alone dies in Exec Bootstrap and never reaches DOS).

Two caveats. AROS publishes m68k ROMs only in nightly builds and SourceForge
keeps about two days of them, so the pin in `tools/fetch-aros-rom.sh` rots; the
script then falls back to the newest nightly and says so loudly. Mirror the two
512 KB files and set `AMINETXDUO_AROS_ROM_URL` or `AMINETXDUO_AROS_ROM_DIR` to
stop that. And `OpenLibrary("bsdsocket.library")` fails there — not an AROS
incompatibility: bringing the stack up needs a `DEVS:NetInterfaces` entry naming
a SANA-II device, and the only driver FS-UAE's emulated hardware has is
`a2065.device`. (AROS ships `prm-rtl8029.device`, which may pair with FS-UAE's
NE2000-based cards. Untried, and the obvious next thing to try.)

*Kickstart 3.1 plus `a2065.device`.* Both are Commodore's, neither is
redistributable, and neither is in this repository. This is the only tier that
can run the SANA-II network tests and the `bsdsocktest` conformance suite. It
runs on a self-hosted runner, gated on the repository variable
`AMINETXDUO_KICKSTART_RUNNER`; if that is unset the job does not run and the
workflow summary says in as many words that the network and conformance results
are unverified for that commit. It never silently substitutes the AROS ROM.

**What CI therefore does not cover:** the `bsdsocket.library` ABI end to end,
the conformance score, SANA-II against a real driver, throughput, Enforcer on
68030, and real hardware. Those are the numbers in the table above, and they
still come from a machine with a Kickstart ROM.

**To run the rest yourself** you need FS-UAE, a Kickstart 3.1 ROM
(`AMINETXDUO_KICKSTART`), `a2065.device` (`AMINETXDUO_A2065`) for the network
tier, and Enforcer/MungWall for `tools/enforcer-run.sh`. None of them can be
distributed with the source.

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
