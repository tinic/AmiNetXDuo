# AmiNetXDuo

An AmiTCP/Roadshow-compatible `bsdsocket.library` for classic AmigaOS, built on
[Eclipse ThreadX NetX Duo](https://github.com/eclipse-threadx/netxduo).

> **Status: functional but incomplete.** The stack obtains a DHCP lease, answers
> ARP, pings its gateway, resolves DNS and moves TCP in both directions on an
> emulated 68020/68030 under Kickstart 3.1. `bsdsocket.library` scores
> **125/142** on the [`bsdsocktest`](https://github.com/tbdye/bsdsocktest)
> conformance suite, and 133/142 on the network tier. It has not been run on
> real hardware and is not yet suitable for anything you care about.

## Why

AmigaOS has never included a TCP/IP stack; networking has always been supplied
by a third-party shared library. Each of the existing options has a significant
drawback. AmiTCP 3.0b2 is free but dates from 1994. AmiTCP 4.x and Miami are
proprietary and effectively unobtainable. Roadshow, the only genuinely modern
stack, is commercial and closed.

AmiNetXDuo combines an industrial-grade, MIT-licensed embedded TCP/IP core with
the AmigaOS socket ABI that existing Amiga network software already targets,
over the SANA-II driver interface that existing Amiga network cards already
implement.

That combination provides several things the classic stacks do not:

- **An MIT licence throughout.** There is no 4.4BSD or GPL-derived code anywhere
  in the tree.
- **IPv6.** The dual stack builds with `-DAMINETXDUO_IPV6=ON` and supports
  ICMPv6, neighbour discovery, duplicate address detection, stateless address
  autoconfiguration from router advertisements, TCP and UDP over IPv6, and
  `AF_INET6` sockets through `bsdsocket.library`. As far as we have been able to
  establish, no earlier classic Amiga TCP/IP stack has supported IPv6. It is
  disabled by default so that the minimum build stays small.
- **NetX Duo's protocol catalogue.** DHCP, DNS and AutoIP are in use today. PPP,
  PPPoE, SNTP, mDNS and NAT are vendored but not yet used.
- **A data path written for the 68020.** `src/net68k/` replaces NetX Duo's IP
  checksum with an `add.l`/`addx.l` carry chain, which is 3.11× faster on the
  primitive, and `memcpy` with a `movem.l` loop, which is 1.23× faster. Together
  these account for most of the improvement from 261 to 356 KB/s recorded in the
  throughput table below. The vendored files themselves are unmodified; the
  symbols are simply resolved from our archive instead. `src/crypto68k/` uses
  the same arrangement.

### TLS

`nx_secure` completes a real TLS 1.2 handshake. With the optimised arithmetic in
`src/crypto68k/` wired into it, client-side public-key work on a 68020 costs
**3.2 s**, and a complete handshake against a public HTTPS server, including
certificate chain verification, takes **6.8 s**. The 185 s figure quoted earlier
in development was a loopback total dominated by the *server* half of the
handshake, which a client never performs; that same measurement is now 26.7 s.

`-DAMINETXDUO_TLS=ON` builds **`tls.library`**, which has eight vectors and is
opened only by programs that want it. The certificate chain is verified against
approximately 120 CA roots in `DEVS:Internet/certificates` and the host name is
checked, both by default:

```c
LONG s = socket(AF_INET, SOCK_STREAM, 0);   connect(s, ...);
struct TLSConnection *tls = TLSOpen(TLSBase, SocketBase, s,
                                    TLSA_HostName, (ULONG)"example.com",
                                    TLSA_Error,    (ULONG)&why);
TLSWrite(TLSBase, tls, request, len);
while ((n = TLSRead(TLSBase, tls, buf, sizeof buf)) > 0) { ... }
TLSClose(TLSBase, tls);          /* the descriptor is yours again */
```

`tests/tls/tls_api`, a program that uses only the public library interface and
none of our internal code, fetches a real HTTPS URL this way and passes 26
checks. The full contract is documented in
[`include/aminetxduo/tlslib.h`](include/aminetxduo/tlslib.h), including what
`WaitSelect()` does and does not tell you once TLS sits in the middle.

TLS is nevertheless still disabled by default, and the reason is now neither
speed nor capability: nothing in the distribution *uses* it yet, and a release
built on a machine without a CA bundle would ship a trust store that refuses
every connection. Both are small pieces of work; see
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

The two most difficult parts are
[ThreadX on Exec](docs/RESEARCH.md#62-threadx-on-exec-the-central-problem) and
the [SANA-II framing mismatch](docs/RESEARCH.md#34-sana-ii--the-driver-contract).
NetX Duo reaches into ThreadX internals in order to suspend socket callers, so a
real kernel is required rather than a compatibility shim. And NetX Duo expects
Ethernet headers, whereas SANA-II hides them.

## Target

68020, AmigaOS 3.1, 4 MB. IPv6, TLS and the `bpf_*` subsystem are build options
so that the minimum build stays small. Measured code size on m68020 at `-O2`:
ThreadX core 27.7 KB, NetX Duo IPv4 core approximately 73 KB, DHCP client
15 KB, DNS 9.5 KB.

The shipped libraries, as hunk files rather than link-map text, are 249,636
bytes for a default `bsdsocket.library`, 250,084 with `-DAMINETXDUO_TLS=ON`, and
273,080 for `tls.library`. The TLS pair therefore comes to 523,164 bytes, which
is 1,124 bytes inside 512 KiB. Read that as a measurement and not as headroom.
There is one obvious trimming lever still untouched: `src/tls/CMakeLists.txt`
globs the whole of `crypto_libraries/src`, so `nx_crypto` still carries DES,
3DES, MD5, CCM, GCM and ECJPAKE.

## Building

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake
cmake --build build --parallel
```

If you have no m68k toolchain, `tools/fetch-toolchain.sh` will download the
pinned one (GCC 15.2 with NDK 3.9, a 35 MB `.tar.xz` verified against its own
sha256, 265 MB unpacked) into `~/.cache/aminetxduo/toolchain`. It comes from a
release asset on this repository, falling back to the upstream Docker layer,
pinned by that layer's content digest, if the release cannot be reached. The
build then finds the result without being told where it is. Otherwise the search
order is `-DAMIGA_TOOLCHAIN_ROOT` → `$AMIGA_TOOLCHAIN_ROOT` → that cache →
`m68k-amigaos-gcc` on `$PATH` → `/opt/m68k-amigaos` →
`~/amigaos/tools/m68k-amigaos-gcc`.

The pinned toolchain is a **Linux x86-64** build. On any other host, install or
build one yourself and point `AMIGA_TOOLCHAIN_ROOT` at it; the layout to match
is `<root>/bin/m68k-amigaos-gcc` together with
`<root>/m68k-amigaos/ndk-include`.

Either NDK version works. The sources are written to the spellings that NDK 3.2
and NDK 3.9 agree on, such as `struct timerequest` rather than 3.2's
`struct TimeRequest`, so a toolchain carrying either one will build the tree.

### Versioning

The version number is **compound**: ours, plus that of the stack we are built
on. An `.lha` on Aminet and a CI artefact both have to state exactly what they
are, and "AmiNetXDuo 0.1.0" on its own does not answer the question "built
against which NetX Duo?".

```
0.1.0+nx6.5.1                                        artefact and archive names
AmiNetXDuo 0.1.0 (NetX Duo 6.5.1, ThreadX 6.5.1)     what a person reads
```

`project(AmiNetXDuo VERSION ...)` in `CMakeLists.txt` is the only place our own
version is written down. The NetX Duo and ThreadX versions are read from
`third_party/*/common/inc/*_api.h` rather than entered by hand:
`cmake/AmiNetXDuoVersion.cmake` checks them against the versions it has pinned
and refuses to configure if a submodule bump has left those pins stale, since an
artefact that misnames its own contents is worse than one carrying no version at
all. It generates `<aminetxduo/version.h>` into the build tree.

`tools/version.sh` answers the same questions from a shell, because CI has to be
able to name an artefact before it has built anything and cannot run an m68k
binary in order to ask. The `version_scheme` host test checks that the two
implementations agree.

Release tags are plain `vX.Y.Z`. The commit count belongs in a binary's version
output and nowhere else: not in a tag, a release name or an archive filename.

## Testing

CI runs `tools/ci.sh` and nothing else, so the same checks can be run before
pushing:

```sh
tools/ci.sh                 # host tests, all four cross builds, conformance build
tools/ci.sh host            # just the host tests (no cross toolchain needed)
tools/ci.sh emulator        # the on-Amiga harnesses under FS-UAE
```

A first run with nothing installed will fetch the toolchain itself; a warm run
of the whole of tier 1 takes about a minute. The workflows in `.github/` call
this script and add nothing beyond caching and scheduling, so a green tick there
and a green run here mean the same thing.

`tools/fsuae-run.sh` runs an AmigaOS executable under FS-UAE on an emulated
A1200 with a real Kickstart 3.1, captures `ami_log()` serial output, and returns
the program's exit status to the host:

```sh
tools/fsuae-run.sh -t 90 build/cm/tools/smoke/smoke
```

`-c 68030` selects a full 68030. That processor has an MMU, so Enforcer can be
used with it, whereas the 68020 floor configuration runs with no illegal-access
checking at all. `-m A3000` goes further and gives a 68030 with 8 MB of 32-bit
motherboard RAM. `-n` attaches an emulated A2065 on SLIRP for real networking.

**Take no timings from anything above a 68020.** FS-UAE 3.2.35 turns cycle
accounting off for every CPU model above the 68020, and neither `accuracy`,
`cpu_speed`, `cpu_frequency`, `cpu_multiplier` nor `cpu_cycle_exact` turns it
back on; on the A3000 profile the quickstart also runs after the configuration
file and overwrites what it sets. `tests/perf/cpucal` measures this directly.
The A1200 model is faithful, reporting `ADD.L` at 2.00 cycles and an implied
clock of 13.93 MHz against a real 14.187. The A3000 profile reports an implied
clock of 327 MHz, `MULU.L` at 3.26 cycles where a 68030 charges 44, and Fast RAM
at 425 MB/s. Both `-c 68030` and `-m A3000` remain valid for *correctness* work,
which is what Enforcer wants them for, and the harness prints a warning to that
effect when the profile starts.

For timings at a higher clock, use `-k MHZ` on the 68020 model, where cycle
accounting does work: `-k 25` gives a cycle-accurate 68020 at 24.48 MHz, with
`ADD.L` still at 2.00 cycles and the memory model still behaving correctly
(doubling the clock buys 2.03× on Fast RAM but only 1.49× on Chip). Since a real
A3000 has the same clock with narrower memory and no data cache, that is a lower
bound on one rather than an estimate of one.

One caveat that applies to the faithful model as well: `MULU.L` is charged 32
cycles where a 68020 charges 43, which mildly flatters the crypto assembly in
`src/crypto68k/`.

`tools/smoke/` holds six diagnostic probes, built by the `smoke_probes` target
and deliberately not registered with `ctest`: a harness self-test, an entropy
pool probe, a ThreadX task lifecycle probe, a kernel-stop survival test, and two
that fault on purpose (`crashtest` jumps to address `0x2`, and `gurutest` frees
the same memory twice) in order to confirm that the crash guard and the
`Alert()` hook report what they should.

The conformance target is [`bsdsocktest`](https://github.com/tbdye/bsdsocktest),
a 142-test suite for `bsdsocket.library` implementations, vendored as a
submodule:

```sh
tests/conformance/build.sh
tests/conformance/run-fsuae.sh -a "LOOPBACK NOPAGE"
```

### Current results

| | |
|---|---|
| conformance, loopback tier | **125/142** (1 fail, 16 skip) |
| conformance, network tier | **133/142** |
| ThreadX-on-Exec soak | 98 checks, 4+ adopted tasks, Enforcer-clean on 68030 |
| TCP throughput, 13.9 MHz 68020 | **356 KB/s** loopback, **368 KB/s** to a host over SLIRP (was 261 / 312 before `src/net68k/`) |
| TCP throughput, 24.5 MHz 68020 | **636 KB/s** through the library, 1.78× for a 1.76× clock; conformance unchanged |
| IPv6 (`-DAMINETXDUO_IPV6=ON`) | ICMPv6 + TCP + UDP between two `NX_IP` instances (78 checks); `AF_INET6` sockets over `::1` through the library ABI; ICMPv6 to the host across an emulated A2065, with a router advertisement and stateless autoconfiguration |

Verified on 68020 and 68030. The single remaining loopback failure is a
deliberate difference in behaviour. The suite treats `SOCK_RAW` as unsupported
only when the error returned is `EACCES`, but `EACCES` means that the caller
lacks privilege, which cannot be true on an operating system with no privilege
model. This implementation returns `ESOCKTNOSUPPORT` instead, so that test stays
red.

### Continuous integration

There are two workflows, kept deliberately separate, because a green tick on one
must never be read as a claim about the other.

**`.github/workflows/ci.yml` — tier 1, runs on every push.** It requires nothing
beyond a network connection.

| | |
|---|---|
| toolchain | `tools/fetch-toolchain.sh` retrieves GCC 15.2 with NDK 3.9 from this repository's toolchain mirror release, verified against the asset's sha256, with the upstream Docker layer (pinned by its content digest) as a fallback; cached between runs |
| cross builds | default, `-DAMINETXDUO_IPV6=ON`, `-DAMINETXDUO_TLS=ON` and `-DAMINETXDUO_CRYPTO68K_ASM=OFF`; all four are built, because each of them has broken at some point while the others still worked |
| warnings | `-Wall -Wextra -Werror` on our own sources, with vendored code exempt (`cmake/ci-warnings.cmake`) |
| host tests | 5 suites through `ctest`: config parsers (157 checks), mbuf chains (206), BPF filter VM (201), crypto68k vectors (4,964, being RSA-2048 known-answer tests plus a differential comparison against the vendored bignum code), and net68k checksum (10,030, a differential comparison against the vendored checksum across every length, alignment and packet chain) |
| host compilers | GCC on Linux and clang on macOS, so that neither becomes the only one that works |
| conformance | `bsdsocktest` is compiled for m68k; running it belongs to tier 2 |

**`.github/workflows/emulator.yml` — tier 2, the on-Amiga harnesses.** These
require a boot ROM, and two of them are supported.

*The AROS m68k ROM.* AROS is an open-source reimplementation of AmigaOS,
licensed under APL 1.1 and freely redistributable, and it boots this project's
FS-UAE harness. Measured on 2026-07-25 against Kickstart 3.1 40.68 using the
same binaries, the check counts are identical: `smoke` 5, `lifecycle` 18,
`KernelStop` 8, `ram_driver_test` 32, `mbuf_bpf_test` 154 and `soak_test` 98,
giving six harnesses and 315 checks with no failures, at the cost of one to two
seconds' extra boot time. `RemTask()` freeing `tc_MemEntry`, `Forbid()` nesting
across `Wait()`, `timer.device`, `RawPutChar()` serial output and the `Alert()`
hook all behave as the code expects. `tools/fetch-aros-rom.sh` downloads the
ROM, and `tools/fsuae-run.sh` passes the second half through
`AMINETXDUO_KICKSTART_EXT`, because AROS consists of a 512 KB base ROM *plus* a
512 KB extended ROM; booting the base alone dies in Exec Bootstrap without ever
reaching DOS.

There are two caveats. AROS publishes m68k ROMs only as nightly builds, and
SourceForge keeps roughly two days of them, so the pin in
`tools/fetch-aros-rom.sh` goes stale; the script then falls back to the newest
nightly and reports prominently that it has done so. Mirroring the two 512 KB
files and setting `AMINETXDUO_AROS_ROM_URL` or `AMINETXDUO_AROS_ROM_DIR` avoids
this. The second caveat is that `OpenLibrary("bsdsocket.library")` fails under
AROS. This is not an AROS incompatibility: bringing the stack up requires a
`DEVS:NetInterfaces` entry naming a SANA-II device, and the only driver that
FS-UAE's emulated hardware provides is `a2065.device`. AROS ships
`prm-rtl8029.device`, which may pair with FS-UAE's NE2000-based cards; that has
not been tried yet, and it is the next thing to attempt.

*Kickstart 3.1 with `a2065.device`.* Both are Commodore's, neither is
redistributable, and neither is included in this repository. This is the only
tier that can run the SANA-II network tests and the `bsdsocktest` conformance
suite. It runs on a self-hosted runner and is gated on the repository variable
`AMINETXDUO_KICKSTART_RUNNER`. If that variable is unset the job does not run,
and the workflow summary states in as many words that the network and
conformance results are unverified for that commit. The AROS ROM is never
substituted silently.

**CI therefore does not cover** the `bsdsocket.library` ABI end to end, the
conformance score, SANA-II against a real driver, throughput, Enforcer on the
68030, or real hardware. Those are the figures in the table above, and they
still come from a machine with a Kickstart ROM.

**To run the remainder yourself** you need FS-UAE, a Kickstart 3.1 ROM
(`AMINETXDUO_KICKSTART`), `a2065.device` (`AMINETXDUO_A2065`) for the network
tier, and Enforcer with MungWall for `tools/enforcer-run.sh`. None of these can
be distributed with the source.

### Debugging

There is no memory protection, so a bad pointer will take the machine down
without producing any output. `include/aminetxduo/crashguard.h` provides two
facilities worth enabling in any test. `ami_crash_install()` catches CPU
exceptions and writes the exception name, PC, SR and all registers to the serial
log. `ami_crash_install_alert_hook()` intercepts Exec `Alert()` so that a Guru
arrives decoded, as "FREEING MEMORY ALREADY FREED" for instance, with the
offending task named, rather than as a hexadecimal code on a dead screen.

## Compatibility posture

AmiNetXDuo is an independent implementation of a *published ABI*. No AmiTCP,
AROSTCP, Miami or Roadshow code has been used, copied or disassembled. Olaf
Barthel's freely distributable Roadshow SDK headers and autodocs are used solely
as an ABI reference, for function offsets, tag values, structure layouts and
documented behaviour.

## Prior art

Two other modern-stack projects appeared in July 2026 and are worth knowing
about. [lwip-amiga](https://github.com/rondoval/lwip-amiga) combines lwIP with
`bsdsocket.library`, but uses a custom `netdev` driver ABI rather than SANA-II,
which restricts it to PiStorm and Emu68.
[AmiTCP_NG](https://github.com/MW0MWZ/AmiTCP_NG) is a GPL fork of AmiTCP 3.0b2
with a clean-room Roadshow ABI. There is a fuller survey in
[docs/RESEARCH.md §2](docs/RESEARCH.md#2-prior-art).

## Licence

MIT. ThreadX and NetX Duo are MIT-licensed as well (© Microsoft and the Eclipse
ThreadX contributors) and are consumed as unmodified git submodules.
