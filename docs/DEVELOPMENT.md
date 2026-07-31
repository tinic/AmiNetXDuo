# Developing AmiNetXDuo

Everything about building, testing and measuring the stack. The
[README](../README.md) is for people who want to *use* it; this is for people
working on it.

The engineering record — every decision, every measurement, and the ones that
were later overturned — is in [RESEARCH.md](RESEARCH.md). This file is the
working reference distilled out of it.

---

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

 ThreadX + Exec port  TX_THREAD ↔ struct Task · baton scheduling · 50 Hz tick
```

The two most difficult parts are
[ThreadX on Exec](RESEARCH.md#62-threadx-on-exec-the-central-problem) and
the [SANA-II framing mismatch](RESEARCH.md#34-sana-ii--the-driver-contract).
NetX Duo reaches into ThreadX internals in order to suspend socket callers, so a
real kernel is required rather than a compatibility shim. And NetX Duo expects
Ethernet headers, whereas SANA-II hides them.

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
on. A released `.lha` and a CI artefact both have to state exactly what they
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
tools/ci.sh                 # host tests, all cross builds, static analysis, conformance
tools/ci.sh host            # just the host tests (no cross toolchain needed)
tools/ci.sh analyze         # just the static analysis
tools/ci.sh emulator        # the on-Amiga harnesses under FS-UAE
```

A first run with nothing installed will fetch the toolchain itself; a warm run
of the whole of tier 1 takes about four minutes, three of which are the analysis
stage. The workflows in `.github/` call this script and add nothing beyond
caching and scheduling, so a green tick there and a green run here mean the same
thing.

### Static analysis

The `analyze` stage runs two tools against baselines of triaged findings, and
fails when a finding appears that is not in one:

```sh
tools/analyze.sh            # GCC -fanalyzer, cross compiler, vs the baseline
tools/analyze.sh --update   # accept this run as the new baseline
tools/cppcheck.sh           # cppcheck error/warning classes, vs its baseline
tools/cppcheck.sh --style   # print the style classes too, gate nothing
```

Both script headers explain what is in their baseline and why none of it is a
defect. Two things about them are worth knowing before reading a result:

* `tools/analyze.sh` compiles with `-D_NO_INLINE`, which swaps the NDK's inline
  `jsr` stubs for the `clib/` prototypes. Without it `-fanalyzer` believes
  `ReadEClock(&ev)` leaves `ev` uninitialised — an `__asm volatile` with a
  `"memory"` clobber is not a store it can see — and two thirds of its findings
  on this tree are that one blind spot.
* Both scripts print what they could **not** cover: units too complex for the
  analyser to finish, and units that would not compile under `_NO_INLINE`. At
  GCC's default exploration limit 48 of 213 units gave up silently — including
  `netdb.c`, `nettrace.c` and `telnet.c`, three of the five files the
  memory-safety audit found defects in. The limit is raised until three units
  are left, and those three are named on every run.

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

`tools/amiberry-run.sh` is the same idea under Amiberry on Linux, and it is the
only harness that can put the guest on a real network. `-N` takes WinUAE's board
keys, so all nine ethernet cards are available rather than only the A2065, and
`-B <interface>` bridges the card onto a host NIC through libpcap instead of
SLIRP — the guest then takes a lease from the real DHCP server and answers pings
from other machines. That needs `sudo setcap cap_net_admin,cap_net_raw=eip` on
the Amiberry binary, reapplied after every relink, and the binary must not live
on a `nosuid` mount. `tests/netstack/run-amiberry.sh` is the bring-up test on
top of it:

```sh
tests/netstack/run-amiberry.sh                        # A2065 on SLIRP
tests/netstack/run-amiberry.sh -N ne2000_pcmcia -B ens18
```

A bridged run that quietly fell back to SLIRP passes every check and proves
nothing, so the harness reads the backend out of the emulator log and fails the
run if it is not the one asked for. docs/RESEARCH.md 78 is the write-up,
including the three ways to ask for a bridge and silently not get one.

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
| conformance, loopback tier | **130/142** (0 fail, 12 skip) |
| conformance, network tier | **142/142** (0 fail, 0 skip) on a bridged real network — Roadshow 4.364 scores 138. Over FS-UAE's SLIRP it is 141/142: that emulator has no inbound path, so the accept-from-remote test cannot run there at all |
| client access patterns | **94/94** (`tests/clients`) — the call sequences curl, wget, nc, ftp and telnet actually issue, each group named for the program and file it came from |
| ThreadX-on-Exec soak | 98 checks, 4+ adopted tasks, Enforcer-clean on 68030 |
| TCP throughput, 13.9 MHz 68020 | **518 KB/s** loopback, **394 KB/s** to a host over SLIRP, both through the library (was 454 / 381 before the nest-counter critical section and the loopback checksum, and 261 / 312 before `src/net68k/`) |
| TCP throughput, 24.5 MHz 68020 | **636 KB/s** through the library, 1.78× for a 1.76× clock; conformance unchanged |
| IPv6 (default; `-DAMINETXDUO_IPV6=OFF` to omit) | ICMPv6 + TCP + UDP between two `NX_IP` instances (78 checks); `AF_INET6` sockets over `::1` through the library ABI; ICMPv6 to the host across an emulated A2065, with a router advertisement and stateless autoconfiguration; `SOCK_RAW`/`AF_INET6`, so `ping` and `traceroute` take an IPv6 target (docs/RESEARCH.md 67) |

Verified on 68020 and 68030. **The loopback tier has no failures**, and it
cannot reach Roadshow's number by construction: nine of the 142 need a remote
peer, so 133 is that tier's ceiling and the comparison has to be made on the
network tier.

Test 41, `accept(): incoming connection from remote host`, was the last red and
was carried for a long time as "the emulator rather than us". That was wrong on
both counts. It was ours: the helper connects, writes and closes in one breath,
so on a real network the connection is already in `CLOSE_WAIT` by the time the
application looks at it, and both halves of the accept path demanded
`NX_TCP_ESTABLISHED` exactly. `bsd_readable()` never reported the listener
readable, `WaitSelect()` slept out its timeout, and the test failed with no
diagnostic — while the connection was complete and its 30 bytes sat queued the
whole time. Fixed in `bsd_incoming_ready()`; docs/RESEARCH.md 63.6.

It took a bridged network only because FS-UAE cannot provide one: `uae_slirp_redir`
is accepted and echoed in its log, and `uae_slirp_redir()` is an empty function
in every slirp backend it ships, so no inbound path exists there whatever the
configuration says. Any peer that connects, writes and closes in one breath hits
the same window on real hardware.

Roadshow 4.364 scores 138 with four known deviations of its own and no skips,
which places it on the network tier with a helper connected and a working
`SOCK_RAW` — so the figure to have compared against all along was our 133, not
our loopback score.

### Measuring throughput

`tests/perf/perf_test` is the instrument: it prices every primitive on the data
path, counts how often the path touches each one, and then runs the same
transfer end to end over loopback and over a simulated wire. It takes 13 s of
host wall clock and reproduces to the KB/s between emulator sessions, so an
A/B is two builds and two runs.

```sh
cmake --build build/cm --parallel --target perf_test
AMINETXDUO_RUN_TAG=perf ./tools/fsuae-run.sh -t 900 build/cm/tests/perf/perf_test
```

Its last section sweeps the TCP receive window, because a single figure cannot
tell "this default is right" from "nothing here responds to the window". Read
the header of `perf_test.c` before quoting anything it prints.

Two things decide whether a number from it means anything:

* **Only the 68020 profiles have cycle accounting.** `-k MHZ` moves that
  model's clock without losing it; a 68030 or anything above is a correctness
  profile and not a timing one. `tests/perf/cpucal` measures which is which.
* **An unlocked CPU is for regime testing, never for absolute figures.**
  `AMINETXDUO_FSUAE_EXTRA="accuracy = -1;uae_cpu_speed = max;uae_cpu_cycle_exact = false"`
  gives a machine where the CPU is not the bottleneck, which is the only way
  to ask "would this be different on a PiStorm?". `cpucal` in that mode reports
  an implied 568 MHz and charges `MULU.L` 2.64 cycles against a real 68020's
  43. Nothing measured there is a claim about hardware.

Throughput on the floor target is a linear function of clock — 1.78× for a
1.76× clock — and a flat function of receive window above 8 KB. The working
out is [RESEARCH 64](RESEARCH.md#64-the-68020-is-cpu-bound-and-the-window-has-nothing-left-to-give-2026-07-28).

## Continuous integration

There are two workflows, kept deliberately separate, because a green tick on one
must never be read as a claim about the other.

**`.github/workflows/ci.yml` — tier 1, runs on every push.** It requires nothing
beyond a network connection.

| | |
|---|---|
| toolchain | `tools/fetch-toolchain.sh` retrieves GCC 15.2 with NDK 3.9 from this repository's toolchain mirror release, verified against the asset's sha256, with the upstream Docker layer (pinned by its content digest) as a fallback; cached between runs |
| cross builds | default, `-DAMINETXDUO_IPV6=OFF`, `-DAMINETXDUO_TLS=OFF` and `-DAMINETXDUO_CRYPTO68K_ASM=OFF`; all four are built, because each of them has broken at some point while the others still worked |
| warnings | `-Wall -Wextra -Werror` on our own sources, with vendored code exempt (`cmake/ci-warnings.cmake`) |
| host tests | 5 suites through `ctest`: config parsers (157 checks), mbuf chains (206), BPF filter VM (201), crypto68k vectors (5,724, being RSA-2048 known-answer tests, a differential comparison against the vendored bignum code, and the AES, SHA-256 and RFC 8439 record-path vectors), and net68k checksum (10,030, a differential comparison against the vendored checksum across every length, alignment and packet chain) |
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

### Third-party clients

Two programs by Stefan "Bebbo" Franke open this library and were written
without reference to our source, which is why they are worth running: BebboSSH
(`bebbossh`, `bebboscp`) and bebboget, an HTTPS downloader with its own TLS.
Both live in the local store -- `~/amiga-assets/bebbossh` and
`~/amiga-assets/bebboget`, or `AMINETXDUO_BEBBOSSH_DIR` / `AMINETXDUO_BEBBOGET_DIR`.
Both are GPLv3+ and neither is vendored or linked; they are separate programs we
install and run.

    tests/bebbossh/run-bebbossh.sh -x      # six transfers each way, two ciphers
    tests/bebbossh/run-bebbossh.sh -x -L   # ... with both ends in the guest
    tests/bebbossh/run-bebbossh.sh -x -I   # a login session, on a real console
    tests/bebbossh/run-bebbossh.sh -x -I -L  # ... logging IN to bebbosshd
    tests/bebbossh/run-bebbossh.sh -E      # the same under Enforcer + MungWall
    tests/bebboget/run-bebboget.sh -x -1   # bebboget beside our own fetch

`-L` is the arm whose throughput can be set beside our own SSH figures: it runs
`bebbosshd` in the guest too, so both ends of the connection are on the one
emulated CPU. Without it only the client half of the crypto is, and the number
is about twice as large for that reason alone.

`-I` is the terminal arm. It opens a real `CON:` as the client's input, because
an SSH client decides it has a terminal by asking `IsInteractive()` and none of
the terminal handling runs if the answer is no. It compares the size the Amiga
console reports against the size the remote `stty size` sees, resizes the window
mid-session to check that `window-change` follows, and reads back the termios
the `pty-req` asked for. docs/RESEARCH.md §78.9 has the numbers.

These stay out of CI at either tier. They need `a2065.device`, they need a
Workbench `locale.library` — without which BebboSSH does not start at all, and
the way it fails looks exactly like a bug in this stack — and the BebboSSH arm
needs an `sshd` *and* an `sftp-server` on the build host, since `bebboscp`
speaks SFTP rather than the old `scp -f`/`-t` protocol. Set
`AMINETXDUO_LOCALE_LIBRARY` if `install/test/run-workbench-fsuae.sh` has not
already unpacked a Workbench 3.1 set into `build/wb31-sys`.

Both harnesses print a verdict line and score on that rather than on the
emulator's exit status, and both compare every transferred byte against its
source. docs/RESEARCH.md §78 has the results.

## Debugging

There is no memory protection, so a bad pointer will take the machine down
without producing any output. `include/aminetxduo/crashguard.h` provides two
facilities worth enabling in any test. `ami_crash_install()` catches CPU
exceptions and writes the exception name, PC, SR and all registers to the serial
log. `ami_crash_install_alert_hook()` intercepts Exec `Alert()` so that a Guru
arrives decoded, as "FREEING MEMORY ALREADY FREED" for instance, with the
offending task named, rather than as a hexadecimal code on a dead screen.

## How the arithmetic compares

`src/crypto68k/` is measured against AmiSSL, the AmigaOS OpenSSL port, both
implementations run on identical inputs back to back in one process with every
answer checked to agree. AmiSSL is not a soft target: its 68020-40 build
assembles Howard Chu's `bn_m68k.s`, so this is our assembly against theirs.

| operation | result |
|---|---|
| RSA-2048 public | dead heat, within the measurement's own uncertainty |
| RSA-2048 private, CRT | ours 1.22× (1.54× against OpenSSL's default, which blinds) |
| ECDSA P-256 verify | ours 1.69× |
| ECDH P-256 | ours 3.03× |
| k·G, the ECDHE keygen | ours 10.8× |
| AES-128-CBC, HMAC-SHA256 | dead heat, and 1.28× |

Handshake arithmetic for a two-certificate chain comes to 850 ms against 2,525.

Two of those rows deserve their explanation rather than the number alone. **We
win the elliptic-curve operations because OpenSSL is constant-time and we are
not** — `ossl_ec_wNAF_mul` forces a Montgomery ladder for any scalar that might
be secret, which is 5,120 field operations against our comb's ~760. That was
demonstrated rather than assumed, by setting `BN_FLG_CONSTTIME` and watching
`k·G` move 0.07%, because the ladder was already running. It is a trade suited
to this machine's threat model, not an engineering victory, and it should be
read that way.

**The bulk path is a dead heat because neither tree has a byte of m68k AES or
SHA-256 assembly**, which is the one place real leverage is still sitting: TLS
costs about 7× plaintext on the wire, and that is where the bytes actually go.

## Ciphersuites

Three are offered, ChaCha20-Poly1305 first:

| | |
|---|---|
| `0xCCA8` / `0xCCA9` | ECDHE-RSA / ECDHE-ECDSA with **ChaCha20-Poly1305** |
| `0xC027` / `0xC023` | ECDHE-RSA / ECDHE-ECDSA with AES-128-CBC and HMAC-SHA256 |

The AEAD is there because the CBC pair no longer reaches a growing share of the
web — Google's front end refuses a ClientHello offering only those — and it is
*first* because it is also the cheaper record path here: **1.72× the CBC pair on
send and 1.73× on receive** over 16 KiB, measured on the 68020. AES-GCM would
restore the same reach and is deliberately not offered: GHASH is a carry-less
multiply the part cannot do, and `nx_crypto`'s bit-serial one costs 344.6 ms per
KB against AES-CBC's 21.9.

RFC 7905's record framing is not GCM's, and `nx_secure` has no hook for the
difference, so two of its record files are copied and edited in
[`src/tls/rfc7905/`](../src/tls/rfc7905/README). See
[RESEARCH.md §54](RESEARCH.md#54-chacha20-poly1305-reach-first-and-speed-as-well-2026-07-27).

Two measurement notes, since both would otherwise flatter us. FS-UAE charges
`MULU.L` 32 cycles where a 68020 charges 43, and `DIVU.L` 51.8 where the manual
says 78, so every figure above carries a correction derived from per-operation
multiply counts. And a contended host inflates *cold* handshake timings — the
resumed column does not move, so a cold number measured while other emulators
are running should not be quoted.

## Session resumption

A handshake is not too slow in the abstract; it is too slow for the patience of
the CDN at the other end. At 13.9 MHz a two-certificate chain completes in 6.8 s
and a three-deep chain takes around 23 s, while Cloudflare closes somewhere
between 11.3 and roughly 20 s.

Resumption removes the public-key work altogether, so the second connection to a
host costs about half a second whatever the first one cost:

| host | chain | cold | resumed |
|---|---|---|---|
| `tls-v1-2.badssl.com` | 2, RSA, `0xC027` | 6,807 ms | **596 ms** |
| `ecc256.badssl.com` | 2, ECDSA, `0xC023` | 23,419 ms | **595 ms** |

It survives both a new process and a reboot, because the cache lives in
`DEVS:Internet/tlssessions` as well as in the library. `www.iana.org` — three
certificates behind Cloudflare, which cannot complete a cold handshake at
13.9 MHz at all — was seeded once at 24.5 MHz, the machine rebooted with only
the 436-byte session file carried across, and then fetched in **0.5 s at
13.9 MHz**, chain verified.

Tickets rather than session IDs, chosen on evidence: probing ten trials per host
gave tickets 40/40 and session IDs 2/40. Session IDs still work where a server
offers them. A rejected ticket falls back to a full handshake.

The cache is keyed on host, port, and a fingerprint of the trust decision
itself — which trust store, by the identity of its root set rather than merely
its presence; whether the chain and host name were verified at all; whether the
certificate validity dates were checked; and how deep a chain the caller was
willing to accept. A session can therefore only be resumed by a connection whose
trust parameters are identical to the one that established it.

That key is narrower than it first shipped, and the difference was a real
defect: the original recorded *that* verification had happened rather than *what
against*, so a session established under one trust store was resumed by a caller
presenting a different one, or none, and the second connection verified nothing.
The curl suite caught it — `--cacert` pointing at a store that signed nothing in
the chain returned HTTP 200 in 1.64 s where a cold handshake takes 5.68 s.

The security trade is the ordinary one every TLS session cache has made since
1996: master secrets and tickets sit in the clear in library memory and on disk,
so forward secrecy is given up for resumed sessions and anyone taking the disk
can decrypt captured traffic for them. On a machine with no memory protection
the in-memory half changes little.

TLS is nevertheless still disabled by default, because a *first* connection to a
CDN-fronted host still does not complete at 13.9 MHz — resumption helps only
once there is something to resume. Raise the clock and the cold handshakes
complete too: `www.iana.org` in 11.3 s at 24.5 MHz, and `example.com`, at four
certificates, in 9.8 s at 56 MHz. See
[docs/RESEARCH.md §9](RESEARCH.md#9-decisions-2026-07-24) and §13.

Nothing here can be taken down by a peer that is slow, rude or absent:
`tests/tls/run-hangup.sh` stands four badly-behaved servers on the host — reset,
FIN, silence, and non-TLS bytes — and each produces a legible error and `rc 10`
with the machine carrying on.

## What an adversarial client suite found

`tests/curl/` pointed curl at `bsdsocket.library` as an adversary rather than
as something to be tested: 149 hermetic cases over HTTP mechanics, connection
reuse, byte-exactness, failure paths, behaviour under repetition, TLS and FTP,
against host-side servers including four deliberately rude ones and a local PKI
covering chain depths 2/3/4 plus expired, self-signed and untrusted roots.

**It has been removed along with curl itself.** What it found has not been, and
is why this section stays: three defects in two days, none of which the
conformance suite could see, and the pattern in them is worth more than the
count. Two presented as *slowness* and were something worse underneath:

- **The SANA-II receive window was 4 frames**, that constant being a window
  rather than a queue length. A concurrency sweep lost 87 of 232 transfers. The
  case that had never failed got 2.5x faster once fixed -- TCP had been hiding
  the loss in retransmissions, which means every throughput figure this project
  took before it was measured through it.
- **Every last close of the library cost fifteen seconds**, so `curl --version`
  took 16.22 s where it went on to take 2.20 s. `S2_OFFLINE` returns queued
  reads without needing `AbortIO()`, and was being issued after the readers had
  already timed out waiting for an abort this driver never performs. Underneath
  the delay, the old path freed the reply port, the pinned packets and the stack
  a reader thread was still running on.
- **A resumed TLS handshake ignored the trust store**, described above.

The lesson generalises past the client that produced it: a suite that tries to
BREAK the stack finds things a suite that checks conformance cannot, because
the second one only ever asks questions the implementer already thought of.

Its host end survives and is still load-bearing. `tests/peer/httppeer.py` and
`tests/peer/mkpki.sh` -- HTTP with keep-alive, ranges, chunking and drip-feed,
seven HTTPS servers on seven certificate chains, and a whole test PKI -- now
serve `tests/compare`, `tests/tools/run-sntp.sh` and `tests/trace`. The guest
end survives too, as `tests/compare/checkrunner.c`: nothing about running a
list of commands and writing down what happened was ever curl-specific.

## Ported clients

[`clients/`](../clients/) is a harness for porting a Unix network client to
AmigaOS 3.x: toolchain resolution, the NDK flags every such port needs, and the
libc and libgcc gaps in `clients/compat/`. curl was the first client through it
and is the reason several of those shims exist -- `__ctzdi2` for a bitset scan,
`__floatdidf` for a progress meter -- which is why `clients/compat/` still
names it. It is no longer built or shipped here: no TLS through our own
`tls.library`, and its remaining faults were the toolchain's rather than the
stack's.

Dropbear builds through the same harness, so the Amiga can `ssh`: unpatched
against stock OpenSSH 10.2 with no compatibility settings, negotiating
curve25519, an ed25519 host key, chacha20-poly1305 and public-key auth.

A connection was 96 s at first contact and is **12.18 s** now. Profiling put
**97% of the handshake in public-key arithmetic and 1.7% in the network**, with
the largest single row being host-key *verification* (46%) rather than the key
exchange. The cause was representation, not algorithm: Dropbear's TweetNaCl
holds a field element as 16 x 16-bit limbs in an `i64[16]`, so one field
multiply became 256 software 64x64 multiplies on a part with a one-instruction
32x32 to 64. Redone over `uint32_t[8]` with a dedicated squaring, an
addition-chain inversion and a dedicated Edwards doubling, it is 6.93x faster,
with no assembly at all.

Switching to the algorithms `crypto68k` already accelerates was tried and is
**1.8x worse** -- 149.62 s -- because Dropbear's P-256 goes through libtommath,
which is 5x slower per scalar multiply than TweetNaCl's curve25519.

Two things a person running a ported client needs to know, both of which cost
several failed runs to find. It wants **`mathieeedoubbas.library` in `LIBS:`**
— newlib implements double arithmetic by calling it, and it is *not* in the
Kickstart 3.1 ROM, though every Workbench install has it; if the client is
linked against AmiSSL it wants `mathieeedoubtrans.library` too, as a **matched
pair from the same source**, since a mixed pair does not work. And a Shell
gives a command 4 KB of stack, so **`stack 200000`** first.

## Two things left as they are, on purpose

Both were found by the DHCP and RFC 3927 work, both were measured, and both
were decided against changing. They are recorded here so that finding them
again does not read as finding them for the first time.

**AutoIP gives up its address on the first late conflict.** RFC 3927 §2.5 asks
for one defence, keeping the address, before yielding. The vendored
`nx_auto_ip.c` says in its own comment that there is no defence, and a trace
confirms the shape of it: the ARP layer *does* send the defensive announcement,
and AutoIP then zeroes the interface anyway. So a single stray ARP costs a
link-locally addressed machine its address. Correcting it means overriding
vendored code, and it only bites a machine that has no DHCP server at all and
a conflicting host on the same wire. Left alone.

**`DHCPDECLINE` is never sent, so a duplicate address is not detected before
use.** The probe state, the conflict handler and the decline are all behind
`NX_DHCP_CLIENT_SEND_ARP_PROBE`, which this port does not define. A measurement
build confirmed it works when enabled and priced it: bring-up goes from 1.12 s
to 5.00 s, **+3.9 s on every boot**, to guard against a server handing out an
address somebody else already holds. Not worth it here. Enabling it is one
`#define` in `port/netxduo-amiga/inc/nx_user.h`, and the RFC 3927 test activates
its declined-address phase automatically if you do.

## The AMITCP ARexx host, and what ADD/RESET would cost

`src/netstack/netstack_rexx.c` services the port; `netstack_rexx_vars.c` is
QUERY and SET. Everything in both comes from AmiTCP/IP 3.0b2's own source, which
is published as `comm/net/AmiTCP-src-30b2.lha`: `kern/variables.src` for the
name space, `kern/config_var.awk` to turn it into the exact `FindArg()`
templates (`awk -v TARGETTI=C -f config_var.awk variables.src`),
`kern/amiga_config.c` for the parser and error strings, `kern/amiga_cstat.c` for
CONNECTIONS, ICMPHIST and ROUTES, `kern/amiga_netdb.c` for ADD and RESET.

### What the corpus actually sends

Re-scanned `comm/tcp` + `comm/net` -- 1,024 archives, each downloaded,
extracted, grepped and deleted; 1,002 were actually read (21 over a 2 MB cap, one
404). Fifteen send `ADDRESS AMITCP`, and between them they send six distinct
commands:

One caveat on the method, because it bit once. The first pass filtered with
`grep --binary-files=without-match`, and AmigaDOS scripts often carry high-bit
bytes, so grep called them binary and skipped them -- which is how SLIPShuttle's
`startnet` was missed. Every archive §75.7 names was then re-fetched and read
with `grep -a`, including the compiled ones: SLIPCall and SLIPShuttle carry the
command in a binary string table, and it is `KILL` in both.

| Command | Archives | Sent by |
|---|---|---|
| `KILL` | 13 | `stopnet` in AmiTCP 2.3 / 3.0b2 / 4.0 / 4.3, Genesis's copy, CobbWeb's patched copy, `AmiTCP_session` 1.0 and 1.1, `AmiTCP_dialup/hangup.rexx`, Netdial 4.0, Patch2AmiTCP43, TCP_Start_Stop |
| the variable space, wholesale | 1 | AmiTCP's own `bin/netstat` |
| `QUERY CONNECTIONS` alone | 1 | `rx.fingerd` |
| `ADD HOST <address> <name> [alias...]` | 4 | CobbWeb's patched `startnet` and SLIPShuttle's `startnet`; AmiTCP 4.0's `HowToInstall` and AmiTCP 2.0's `usertext.txt`, both as documentation examples |
| `SET HOSTNAME` | 1 | TCP_Start_Stop's `startnet`, twice |
| `RESET` | 2 | TCP_Start_Stop's `startnet`; AmiTCP 2.0's `usertext.txt` as an example |

Nothing in the corpus sends `READ` or `ROUTE`, which is consistent with AmiTCP
never having implemented either.

`netstat` is why the variable space is implemented. It sends
`QUERY CONNECTIONS`, then 8 ICMP + 20 IP + 46 TCP + 9 UDP counters by
abbreviated name, then `Q ICMPHIST`, then `QUERY ROUTES ALL`. `rx.fingerd`
confirms the format matters independently: it walks `QUERY CONNECTIONS`'s answer
eight words at a time looking for a local port of 79, so the fields have to be
fixed-width and in AmiTCP's order, not merely present.

The three commands with a consumer that is **not** implemented are `SET
HOSTNAME`, `ADD HOST` and `RESET`, and all three have the same shape of
consumer: TCP_Start_Stop, CobbWeb and SLIPShuttle are dial-up front ends
installing the address a SLIP or PPP link was just assigned, under a name. All
discard the return code.
That scenario is ruled out here, and the remaining sightings are documentation.

Two of AmiTCP's own limits had to go, because both break `netstat` on AmiTCP
itself: `CONFIGLINELEN`/our command buffer at 256 bytes cannot hold netstat's
~600-byte TCP query, and `REPLYBUFLEN` at 255 cannot hold 46 counters once any
of them has reached ten digits. They are 1024 here.

### ADD and RESET

`RESET` is cheap and `ADD` is not, and the reason is the same for both.

AmiTCP kept a `struct NetDataBase` of `MinList`s -- hosts, networks, services,
protocols, nameservers, domains, access rules -- built from `AmiTCP:db/netdb`
and mutable at runtime under a read/write lock. `ADD` appended a node to one
list; `RESET` built a whole second database from the file and swapped the lists
into the live one.

`src/config/netdb.c` keeps the same data in a different shape: four
`NetdbTable`s, each one flat `AmiNetdbEntry` array whose `name`, `aliases` and
`proto` pointers all point into a single tokenised copy of the file text, plus a
shared alias pool. It is built once by `ami_netdb_load()` and **immutable
afterwards, which is why no lock exists** -- eight lookup entry points and three
iterators read it from any task with no synchronisation at all.

So:

- **`RESET`** is `ami_netdb_free()` then `ami_netdb_load()`, which already
  exists. The work is not the reload, it is that making the table mutable means
  every one of those eleven readers now needs the lock, and `AmiNetdbEntry`
  pointers handed out to `gethostbyname()` callers currently outlive the call.
- **`ADD`** needs a second, separately allocated store for runtime entries,
  consulted by all eleven readers under that same lock, with its own string
  storage (it cannot point into the file buffer). Plus the `ReadArgs()` template
  `"$NAME$/A,$ENTRY$/A,$ALIAS$/M"` and seven entry-type parsers, of which only
  HOST has a corpus consumer.

Neither is built, for the reason in the table above: the running code that sends
them is dial-up front ends naming a freshly assigned PPP address, and it ignores
what it gets back.
