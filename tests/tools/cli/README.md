# Shipped CLI crash sweep

`matrix.py` derives the native executable inventory from `dist/make-dist.sh`.
`policy.json` supplies valid representative inputs and lifecycle expectations.
A missing tool or newly added option fails the host gate instead of silently
reducing coverage. Vendored Installer and WirelessManager contracts are tied to
the exact binary hashes; their option sets require review when those files change.

The current inventory contains 47 Amiga executables, including installer helpers,
SSH/SCP, WirelessManager and the optional Developer/Profile commands. The suite
requires them all. Tools not shipped in the archive are outside this inventory.
The shipped Python `profreport.py` is exercised separately by the host CLI test,
including every declared flag, missing/truncated files and invalid arguments.
The Installer installation/uninstallation scripts remain covered by
`install/test/run-all.sh` and the existing Workbench installation harnesses;
they are scripts, not LoadSeg executables.

For each tool the sweep covers every option and alias, every pair of options,
no arguments, help, unknown options, duplicate options, missing required inputs,
missing values, empty values, malformed quoting and long values. Numeric fields
also cover negative/zero values, signed 32-bit boundaries and overflows. This is
not an exhaustive enumeration of every possible string or larger combination.
Compiled-out SSH flags are tested for safe refusal as well.

Every case runs at **4096 and 8192 bytes**, under both present and absent socket
libraries. The supervisor uses 64 KB; children receive `NP_StackSize` explicitly
and report their actual task stack bounds from the exit callback. A tool's own
intentional StackSwap is permitted; it must restore the process stack at exit.
No tool is silently given the supervisor's larger stack.

Resident and GUI programs are stopped through Ctrl-C, close-window, Escape or
a Cancel gadget event (both vanilla and raw Escape are supported).
Only `InstallWiFi TO` uses that program's GUI lifecycle; `PRIORITY` and syntax
errors still have to exit without help.
For the pinned Commodore Installer, the suite reuses the button IDs documented
in `install/test/installdrive.c`; no private button layout is assumed for other
programs. Startup/error/abort-confirmation requesters are answered within the
ten-second resident shutdown period (ordinary late-exit cleanup allows three
seconds). Windows that appear after startup are polled as well. The Installer has no batch mode. The minimal Installer script exits with `(quiet)` so a
completion dialog does not obscure its CLI parsing. ReadArgs `?` can continue
using defaults; resident programs keep their ordinary shutdown contract.
The finite profiler workload has a 120-second guest deadline; other cases use
one second before resident shutdown or three seconds for an ordinary exit.
Ordinary exit cases must finish unaided. A late exit, unexpected return code,
missing or malformed journal, load/launch failure, unreplied close message,
exception or timeout fails the run. A valid completed prefix remains covered
when a later child kills the guest. Only the unstarted suffix is retried in a
fresh guest; unrun cases never count as coverage. The guest retains a hung child's allocations and
parks until the host reaps the entire emulator, avoiding an unsafe UnLoadSeg.

Before every full sweep, real guest controls prove normal completion, Ctrl-C,
GUI close (including delayed windows), modal Cancel/Escape, Installer-button
routing, unexpected return codes, late exit, a hang and an illegal instruction
at both stack sizes. A boot failure cannot satisfy a negative control: its child
must actually reach START and produce the expected failure evidence.

The emulator is boardless and uses private RAM/filesystem fixtures. Networking
calls exercise startup/error/loopback paths; there is no live Wi-Fi or TCP peer.
Successful wire protocols, hardware devices, allocation-failure injection and
all possible stack corruptions are not established by this sweep. The existing
network, TLS, installer and hardware harnesses still provide those other tests.

## Run

Build shipping commands/libraries with the existing cross stage and clients with
`AMINETXDUO_CLIENT_ANY=1 AMIGA_CLIENT_ARCH=-m68000 clients/dropbear/build.sh`.
On a Linux host with Amiberry and the licensed ROM for the selected machine:

```sh
tests/tools/run-cli-crashes.sh -b build/ci/default --clients build/dropbear \
  --model A600 --output build/cli-crashes
```

`--tool NAME` and `--state present|missing` narrow diagnostic runs; their metadata
marks them partial. `--controls-only --control-kind NAME` can isolate a failing
supervisor control. Full runs always execute all controls. `--helper-bin DIR`
accepts helpers built on a separate compiler host; their hashes are retained.
`--shard-index N --shard-count 4` partitions inputs deterministically while
keeping their 4096/8192-byte runs together. All four shards, each with both
socket-library states, make up one full profile; each artifact records its shard.
`AMINETXDUO_CLI_ROOT` selects the repository containing the emulator harness for
such split-host validation. These options do not turn a partial run into full
coverage.

`inputs.json` records binary, library, helper and case-generator hashes.
`results.json`, JUnit XML, per-batch journals and logs retain failures and exact
arguments. CI uploads diagnostics, not licensed ROMs or guest disk images.

## CI

`tools/ci.sh host` runs inventory and transcript negative tests on each host CI
invocation. Cross CI compiles the supervisor and fault-control program with
warnings treated as errors. Dispatch the separate Emulator workflow with
`run_cli_crashes` to run the complete native matrix for default/68000 A600 and
fat/68020 A1200 shipping configurations. It is not part of the nightly schedule.
Native execution requires the licensed-ROM runner;
host checks alone do not establish that the commands run without crashing.
