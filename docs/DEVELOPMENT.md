# Developing AmiNetXDuo

Build lines, test entry points and what CI runs. Where a table would restate a
script, this names the script instead: a copy here goes stale and nothing catches it.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake
cmake --build build --parallel
```

| Subject | Where | Note |
|---|---|---|
| Toolchain, if none is installed | `tools/fetch-toolchain.sh` | downloads the pinned one into `~/.cache/aminetxduo/toolchain`; assets cover Linux x86-64 and macOS arm64. On any other host `tools/build-toolchain.sh` builds one, or set `AMIGA_TOOLCHAIN_ROOT` to a tree with `<root>/bin/m68k-amigaos-gcc` and `<root>/m68k-amigaos/ndk-include`. NDK 3.2 and 3.9 both work |
| Search order | `cmake/toolchain-m68k-amigaos.cmake` | `-DAMIGA_TOOLCHAIN_ROOT` → `$AMIGA_TOOLCHAIN_ROOT` → that cache → `m68k-amigaos-gcc` on `$PATH` → `/opt/m68k-amigaos` → `~/amigaos/tools/m68k-amigaos-gcc` |
| Options | `CMakeLists.txt` | each carries the comment saying why it exists and what turning it off changes; `cmake -LAH -S . -B build` lists them with defaults |
| `AMINETXDUO_CPU` | `cmake/toolchain-m68k-amigaos.cmake:287` | the one option that is not on/off. `any` (default) builds one binary for every 68k: `-m68000` codegen with `src/net68k`'s inner loops assembled per class and chosen from `AttnFlags` by `n68k_cpu_select()` (`src/net68k/n68k_cpu.c`), called from `bsd_lib_init()` (`src/bsdsocket/library.c:338`), verified by `tests/perf/n68kmv`. `68000`, `68020`, `68040`, `68060` pin it |

## `tools/ci.sh` — everything CI runs

`.github/workflows/ci.yml` and `emulator.yml` call this script. Bare
`tools/ci.sh` runs `host host32 clientshims cross stackframes web conformance
survey` — every non-emulator stage except `analyze`. Stages can be combined.
The release workflow promotes CI's exact candidate after checking its commit
and digest; it does not compile.

| Stage | What it does | Needs |
|---|---|---|
| `toolchain` | resolve or download the pinned `m68k-amigaos-gcc`; warns if the local one is not the pinned one | network |
| `host` | the pre-build gates, then builds the host test targets and runs `ctest`; the count is exact against `HOST_TESTS_EXPECTED` in `tools/ci.sh`, so adding a test turns CI red until that line is raised | nothing |
| `host32` | the targets needing `sizeof(void*) == 4`: the two fuzzers, and the socket files whose static assertions pin the 4.4BSD `iovec`/`msghdr` shape | `-m32` (gcc-multilib) |
| `cross` | every cross configuration in `CROSS_CONFIGS`, warnings fatal (`cmake/ci-warnings.cmake`) | toolchain |
| `stackframes` | the shipping library stack-frame budget; separate because it is invariant across the option matrix | toolchain |
| `web` | httpd's terminal page still matches the TypeScript it is generated from, and vendored xterm.js is untouched | node |
| `analyze` | `tools/analyze.sh` — GCC `-fanalyzer` vs a triaged baseline — then `tools/cppcheck.sh` vs its baseline when cppcheck is installed, skipping that half when it is not. **Not in the default set** | toolchain |
| `conformance` | builds `bsdsocktest` for m68k; running it is tier 2 | toolchain |
| `emulator` | the on-Amiga harnesses in `EMULATOR_TESTS` | `AMINETXDUO_KICKSTART` |
| `cards` | boots every supported network card, one guest each, and proves each carries bytes both ways | ROM, bridge, peer |
| `e2e` | installs the shipped archive on a real Workbench 3.1, reboots, drives it from another machine | ROM, licensed Workbench, LhA, peer |
| `wirequiet` | what the machine puts on the wire when nobody asked it to: every card, a settle, then a window of idle counted off this host's NIC with `tcpdump`. Nothing else asserts on what the guest EMITS, which is how a DHCPv6 client rebinding twenty-five times a second passed every other stage | ROM, bridge, `tcpdump` |
| `ltoprobe` | the one harness needing its own configure, `AMINETXDUO_CRYPTO68K_LTO_PROBE=ON`, which no `CROSS_CONFIGS` arm builds | toolchain |
| `rate` | receive and transmit throughput against `tests/perf/rate-baseline.txt`, the median of five rounds at a 12% tolerance, failing on a percentage drop rather than an absolute. The only stage that measures a byte per second; 0.26.0 and 0.26.1 shipped a 4x receive regression past every other gate. Skips as `no_bridged_rig` without `AMINETXDUO_RATE_IFACE` and `AMINETXDUO_RATE_PEER`, which CI does not define (`docs/BACKLOG.md`) | ROM, bridge, peer |
| `survey` | the Aminet survey gates, `docs/aminet-survey/README.md` | nothing |
| `submodules` `sanitize` `tlsloop` `fetchtls` `console` `matrix` `capture` `bridged` `cards6` `lossgate` `smb` `e2ecards` | the rest, one subject each; `.github/workflows/emulator.yml` is what passes the on-Amiga ones. `tools/check-changelog-prose.sh` is not a stage: `host` runs it | ROM, bridge, peer for the on-Amiga ones |
| `reachability` | whether the machine still answers ARP and a connect while it is doing a TLS handshake, probed from a peer once a second; the gate is the longest stretch of silence, which is the shape the 44 s dropout had. Takes `-k` because the verdict depends on the emulated clock | ROM, bridge, peer |

Environment: `AMIGA_TOOLCHAIN_ROOT`, `AMINETXDUO_CI_BUILD` (default `build/ci`),
`AMINETXDUO_CI_JOBS`, `AMINETXDUO_CI_CROSS` (subset of the cross arms),
`AMINETXDUO_KICKSTART`. The `host` stage also runs the gates that are not
ctest — `tools/check-*.sh`, `tools/test-verdict-selftest.sh` and the
per-harness graders `tests/*/*-verdict-selftest.sh` — each failing the stage
on its own, each guarding something that otherwise runs only where a ROM does,
and each saying in its own header what it checks.

## On-Amiga harnesses

**`tests/HARNESSES` is the index**, and `tools/check-harnesses.sh` keeps it
honest. Each row is `<path> : <runner>@<when> : <note>`; `<when>` is `push`, `nightly`,
`release` or `hand`, the runner a file that invokes it, `chained:<path>`, or `manual` with a
reason code (`peer`, `bridged`, `bench`, `asset`, `windows`, `SLIRP`, `RED`, `BLOCKED`).

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
boots to a black screen with an empty log. **Take no timings above a 68020.** Cycle accounting is off for every model above
it and no configuration key turns it back on; `-k MHZ` moves the 68020's clock
without losing it, and `tests/perf/cpucal` measures which profiles are which.
Those profiles are still valid for correctness work.

## Static analysis

```sh
tools/analyze.sh            # GCC -fanalyzer, cross compiler, vs the baseline
tools/analyze.sh --update   # accept this run as the new baseline
tools/cppcheck.sh           # cppcheck error/warning classes, vs its baseline
tools/cppcheck.sh --style   # print the style classes too, gate nothing
```

Both fail on a finding that is not in their baseline, and both print what they
could **not** cover. `tools/analyze.sh:15` says why it compiles `-D_NO_INLINE`
and what that leaves uncovered.

## Debugging

There is no memory protection, so a bad pointer takes the machine down without
writing anything.

| Instrument | Where | What it gives |
|---|---|---|
| `ami_crash_install()` | `include/aminetxduo/crashguard.h` | name, PC, SR and all registers to the serial log on a CPU exception. `ami_crash_install_alert_hook()` makes a Guru arrive decoded, with the offending task named, rather than as hex on a dead screen |
| `AMI_ERROR`, `AMI_WARN`, `AMI_INFO` | `include/aminetxduo/compat.h:117` | compiled in only with `-DAMINETXDUO_LOG=ON` (`CMakeLists.txt:313`, OFF when shipping; the `log` cross arm builds it). A shipped library writes no serial log: its evidence is the event ring, `ShowNetStatus EVENTS`. In a log build, what prints is the runtime `ami_log_level()`, starting at `AMI_LOG_WARN`; `SetEnv ANXDLOGLEVEL 2` and restart the network to reach `AMI_INFO`. `AMI_DEBUG` and `AMI_TRACE` are per-packet and still need `AMINETXDUO_DEBUG`. Reaching the serial port at all takes a null modem or Sashimi |
| `netstat -h` | `src/tools/netstat.c` | reads the health counters without opening a library, allocating or taking a lock, so it answers while the rest of the stack has stopped answering. `docs/FREEZE-DIAGNOSTIC.md` is what the counters mean |

## Versioning

The version is compound: `<ours>+nx<netxduo>` in artefact and archive names,
`AmiNetXDuo <ours> (NetX Duo <n>, ThreadX <n>)` for a reader. Release tags are
plain `vX.Y.Z`.

| Authority | Answers |
|---|---|
| `project(AmiNetXDuo VERSION ...)` in `CMakeLists.txt` | the only place our own version is written |
| `cmake/AmiNetXDuoVersion.cmake` | reads the NetX Duo and ThreadX versions from `third_party/*/common/inc/*_api.h`, refuses to configure when a submodule bump leaves its pins stale, and generates `<aminetxduo/version.h>` |
| `tools/version.sh` | the same questions from a shell, for CI naming an artefact before anything is built; the `version_scheme` host test checks the two agree |
| `tools/gen_vectors.py` | regenerates the `bsdsocket.library` vector tables from the `.fd`, `.sfd` and pragma sources named in the README's licence section. `tools/ci.sh` runs it with `--check` in the cross stage, so a generated file that has drifted from its generator turns CI red |

## CPU profiling

`tools/profiler/` is the interrupt-driven m68k sampler; the archive ships
`Profile` and `profspin` in `Developer/Profile/` with `tools/profiler/ReadMe`,
which holds the usage, the copy-and-run commands, the report command and the
A1200 validation settings (priority 5, `RATE=200`, `profspin SCALE=4`, for the
validation only). What the ReadMe does not decide for a run:

| Rule | Why |
|---|---|
| `AMINETXDUO_AMIBERRY_EXTRA=cachesize=0`, and the emulator log must say `JIT=0`; `tools/profiler/selftest.sh` first, its containment check passing; an identical unprofiled run beside every profiled one | a JIT run samples the translator; the sampler's own kernels are the only ground truth on that machine; the throughput control |
| `-DAMINETXDUO_LTO=OFF` when the report needs complete names inside `bsdsocket.library` | an LTO build samples fine, but its `ltrans` objects are gone when the report is made |
| one emulator measurement per host at a time; size `SAMPLES` for the transfer plus command start-up and teardown | build and symbolisation can overlap it, throughput and profiles cannot; a buffer sized for the transfer alone fills before `Profile` regains control and the tail reads as sampler loss |
| a real machine: an unprofiled receive control, then the same as `Profile ... C:iperf -s`, from the same Linux iperf 2 client; keep the raw `.prof` with the machine, accelerator, Fast RAM, SANA-II device and version, library build and peer command | an emulator profile describes that emulated machine and nothing else |
