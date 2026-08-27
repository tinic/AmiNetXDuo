# Developing AmiNetXDuo

Build lines, test entry points and what CI runs. Each section stands on its own.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake
cmake --build build --parallel
```

With no m68k toolchain installed, `tools/fetch-toolchain.sh` downloads the
pinned one into `~/.cache/aminetxduo/toolchain` and the build finds it. Assets
cover Linux x86-64 and macOS arm64; on any other host, `tools/build-toolchain.sh`
builds one, or set `AMIGA_TOOLCHAIN_ROOT` to a tree with
`<root>/bin/m68k-amigaos-gcc` and `<root>/m68k-amigaos/ndk-include`. NDK 3.2 and
3.9 both work.

Toolchain search order: `-DAMIGA_TOOLCHAIN_ROOT` → `$AMIGA_TOOLCHAIN_ROOT` →
that cache → `m68k-amigaos-gcc` on `$PATH` → `/opt/m68k-amigaos` →
`~/amigaos/tools/m68k-amigaos-gcc`.

Build options are in `CMakeLists.txt`, each with the comment that says why it
exists and what turning it off changes; `cmake -LAH -S . -B build` lists them
with their defaults. `AMINETXDUO_CPU` is the one that is not a plain on/off:
`any` (the default) builds one binary for every 68k, `-m68000` codegen with
`src/net68k`'s inner loops assembled per class and chosen from `AttnFlags` in
`bsd_lib_init()` (`src/net68k/n68k_cpu.c`); `tests/perf/n68kmv` verifies the
selection. `68000`, `68020`, `68040` and `68060` pin it
(`cmake/toolchain-m68k-amigaos.cmake:261`).

## `tools/ci.sh` — everything CI runs

`.github/workflows/ci.yml`, `emulator.yml` and `release.yml` all call this
script and add only caching, a matrix and a job summary. Bare `tools/ci.sh` runs
`toolchain`, `host`, `host32`, `cross`, `web` and `conformance` — everything
needing neither an emulator nor a licensed ROM. Named stages can be picked in
any combination; the release set is
`tools/ci.sh host host32 cross web analyze conformance`
(`.github/workflows/release.yml:124`).

| Stage | What it does | Needs |
|---|---|---|
| `toolchain` | resolve or download the pinned `m68k-amigaos-gcc`; warns if the local one is not the pinned one | network |
| `host` | the pre-build gates, then builds the host test targets and runs `ctest`; the count is exact against `HOST_TESTS_EXPECTED` in `tools/ci.sh`, so adding a test turns CI red until that line is raised | nothing |
| `host32` | `fuzz_mdns` and `fuzz_tls_crypto`, which need `sizeof(void*) == 4` | `-m32` (gcc-multilib) |
| `cross` | every cross configuration in `CROSS_CONFIGS`, warnings fatal (`cmake/ci-warnings.cmake`) | toolchain |
| `web` | httpd's terminal page still matches the TypeScript it is generated from, and vendored xterm.js is untouched | node |
| `analyze` | `tools/analyze.sh` — GCC `-fanalyzer` vs a triaged baseline. **Not in the default set** | toolchain |
| `conformance` | builds `bsdsocktest` for m68k; running it is tier 2 | toolchain |
| `emulator` | the on-Amiga harnesses in `EMULATOR_TESTS` | `AMINETXDUO_KICKSTART` |
| `cards` | boots every supported network card, one guest each, and proves each carries bytes both ways | ROM, bridge, peer |
| `e2e` | installs the shipped archive on a real Workbench 3.1, reboots, drives it from another machine | ROM, licensed Workbench, LhA, peer |
| `wirequiet` | what the machine puts on the wire when nobody asked it to: every card, a settle, then a window of idle counted off this host's NIC with `tcpdump`. Nothing else asserts on what the guest EMITS, which is how a DHCPv6 client rebinding twenty-five times a second passed every other stage | ROM, bridge, `tcpdump` |
| `reachability` | whether the machine still answers ARP and a connect while it is doing a TLS handshake, probed from a peer once a second; the gate is the longest stretch of silence, which is the shape the 44 s dropout had. Takes `-k` because the verdict depends on the emulated clock | ROM, bridge, peer |

Environment: `AMIGA_TOOLCHAIN_ROOT`, `AMINETXDUO_CI_BUILD` (default `build/ci`),
`AMINETXDUO_CI_JOBS`, `AMINETXDUO_CI_CROSS` (subset of the cross arms),
`AMINETXDUO_KICKSTART`.

The `host` stage also runs gates that are not ctest, each failing the stage on
its own and each existing because the thing it guards runs only where a ROM
does: `tools/test-verdict-selftest.sh` (the verdict logic the on-Amiga
harnesses share), `tools/check-harnesses.sh` (`tests/HARNESSES` is honest),
`tools/check-shipping-config.sh` (every drawer in the archive is built in a
configuration some cross arm compiles), and the per-harness transcript graders
`tests/*/*-verdict-selftest.sh`. Each script's header says what it checks.

## On-Amiga harnesses

**`tests/HARNESSES` is the index**, and `tools/check-harnesses.sh` keeps it
honest. Each row is `<path> : <runner> : <note>`, where the runner is a file
that invokes it, `chained:<path>`, or `manual` with a reason code (`peer`,
`bridged`, `bench`, `asset`, `windows`, `UNWIRED`). Read that file rather than
a list here — a list here would go stale and nothing would catch it.

| Runner | Host | Use |
|---|---|---|
| `tools/amiberry-run.sh` | Linux | the one that reaches a real network. `-N <board>` takes WinUAE's board keys, `-B <interface>` bridges through libpcap so the guest leases from the real DHCP server. Needs `setcap cap_net_admin,cap_net_raw=eip` on the binary, reapplied after every relink, on a mount that is not `nosuid`. Genuinely headless |
| `tools/winuae-run.sh` | Windows | WinUAE |
| `tools/emurun.sh` | Linux | one gated run: preflight refuses (exit 2) before starting, postflight refuses (exit 3) when the guest wrote nothing |
| `tools/tlsgate.sh` | Linux | the TLS gate. `<builddir> [cpu] [repeat] [slow]`, **the verdict is the exit code** |
| `tools/demo.sh` | Linux | a bridged live Amiga running httpd and the browser terminal, printing the address it leased. Asserts nothing |
| `tools/enforcer-run.sh` | any | Enforcer + MungWall, which is how illegal accesses surface on a machine with no MMU |
| `tools/emu-rig-lock.sh` | Linux | arbitrates ports, names and addresses so two runs on one host cannot take the same thing |

A Kickstart must match both the model and the CPU the run asks for; a mismatch
boots to a black screen. A bridged run that silently fell back to SLIRP passes
every check and proves nothing, so `tools/amiberry-run.sh` reads the backend out
of the emulator log and fails when it is not the one asked for.

**Take no timings above a 68020.** FS-UAE turns cycle accounting off for every
model above it, and no configuration key turns it back on. `tests/perf/cpucal`
measures which profiles are which. `-k MHZ` moves the 68020's clock without
losing cycle accounting. Those profiles are still valid for correctness work.

## Static analysis

```sh
tools/analyze.sh            # GCC -fanalyzer, cross compiler, vs the baseline
tools/analyze.sh --update   # accept this run as the new baseline
tools/cppcheck.sh           # cppcheck error/warning classes, vs its baseline
tools/cppcheck.sh --style   # print the style classes too, gate nothing
```

Both fail on a finding that is not in their baseline, and both print what they
could **not** cover. `tools/analyze.sh` compiles with `-D_NO_INLINE`, swapping
the NDK's inline `jsr` stubs for the `clib/` prototypes: without it `-fanalyzer`
cannot see a store made by an `__asm volatile` with a `"memory"` clobber, and
two thirds of its findings on this tree are that one blind spot.

## Debugging

There is no memory protection, so a bad pointer takes the machine down without
writing anything. `include/aminetxduo/crashguard.h`: `ami_crash_install()`
writes name, PC, SR and all registers to the serial log on a CPU exception, and
`ami_crash_install_alert_hook()` makes a Guru arrive decoded, with the offending
task named, rather than as hex on a dead screen.

`AMI_ERROR`, `AMI_WARN` and `AMI_INFO` compile into every image, shipping ones
included; what prints is the runtime `ami_log_level()`, which starts at
`AMI_LOG_WARN`. So a serial capture from a shipped library IS evidence for the
error and warning tier, and is evidence for nothing above it: `SetEnv
ANXDLOGLEVEL 2` and restart the network to reach `AMI_INFO`. `AMI_DEBUG` and
`AMI_TRACE` are per-packet and still need `AMINETXDUO_DEBUG`. Reaching the
serial port at all takes a null modem or Sashimi.

`netstat -h` reads the health counters without opening a library, allocating or
taking a lock, so it answers while the rest of the stack has stopped answering.
`docs/FREEZE-DIAGNOSTIC.md` is what the counters mean.

## Versioning

The version is compound: `<ours>+nx<netxduo>` in artefact and archive names,
`AmiNetXDuo <ours> (NetX Duo <n>, ThreadX <n>)` for a reader. Release tags are
plain `vX.Y.Z`.

`project(AmiNetXDuo VERSION ...)` in `CMakeLists.txt` is the only place our own
version is written. `cmake/AmiNetXDuoVersion.cmake` reads the NetX Duo and
ThreadX versions from `third_party/*/common/inc/*_api.h`, refuses to configure
when a submodule bump leaves its pins stale, and generates
`<aminetxduo/version.h>`. `tools/version.sh` answers the same questions from a
shell, for CI naming an artefact before anything is built; the `version_scheme`
host test checks the two agree.

`tools/gen_vectors.py` regenerates the `bsdsocket.library` vector tables from
the `.fd`, `.sfd` and pragma sources named in the README's licence section.
`tools/ci.sh` runs it with `--check` in the cross stage, so a generated file
that has drifted from its generator turns CI red.

## Provenance and prior art

Claude (Anthropic's Opus 5) wrote the code, under human direction and testing;
every commit records it in its `Co-Authored-By` line. `docs/RESEARCH.md` indexes
the engineering record, one line per finding, with a statement of whether the
tree still agrees with it. The full record is in git history.

Two other modern-stack projects appeared in July 2026.
[lwip-amiga](https://github.com/rondoval/lwip-amiga) combines lwIP with
`bsdsocket.library` over a custom `netdev` driver ABI rather than SANA-II, which
restricts it to PiStorm and Emu68. [AmiTCP_NG](https://github.com/MW0MWZ/AmiTCP_NG)
is a GPL fork of AmiTCP 3.0b2 with a clean-room Roadshow ABI. Neither is
MIT-licensed and neither drives SANA-II, which is why this one exists. On the
conformance suite AmiNetXDuo scores 142 of 142 and Roadshow scores 138.

Most figures in this tree were measured under emulation. AmiNetXDuo has run on
real hardware, an A3000 with an X-Surf-100: a user measured 795 KB/s reading and
939 KB/s writing over Fitz, and found two defects, both since fixed. IPv6 under
WinUAE requires a patch,
[tonioni/WinUAE@d9df1d8](https://github.com/tonioni/WinUAE/commit/d9df1d8357ade4f9631491cf9f482e159554bfeb);
Amiberry needs none, and Amiberry is what every harness here drives.
