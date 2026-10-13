# Emu68 and the TCP receive path

This note records a source and machine-code investigation of the PiStorm32
receive path. It distinguishes work done once per frame from work done once per
interrupt pass, and distinguishes a sampled PC from the code whose time that PC
represents.

## What runs

On GENET, `ge_rxintr()` walks a burst of up to 32 frames. `ge_deliver()` claims
each frame through `netdev_rx_claim()`, copies its payload into a NetX packet,
checks the chip's checksum result, then calls `netdev_rx_claimed()`. The batch
reply wakes the SANA-II reader, which settles the packets, optionally groups
them with GRO, and calls NetX IP input. NetX TCP packet processing reaches
`_nx_tcp_socket_state_data_check()` and its receive queue. `bsd_recv_tcp()`
eventually extracts queued data into the application's buffer.
For ordinary IPv4 TCP frames that GENET marks `ANXD_S2_RXF_VERIFIED`, the
SANA-II handoff publishes the checksum capability directly; the software
`n68k_rx_verify_sum()` fast path is not run for those frames.

`tcpprof` covers the NetX TCP and extraction part over the RAM driver.
`wireprof` and the recorded GENET `iperf` profile cover the real driver and
SANA-II path. The host `test_netdev_direct` exercises the direct claim and
batch header-copy cases; the TCP receive-pass host test covers NetX queue and
acknowledgement behavior. None of the host tests measures Emu68 execution.

## What the existing profile can say

The recorded PiStorm32/GENET receive profile in
`~/ai/evidence/genet-research/stack-profile/rx-report.txt` has 4,381 samples.
It assigns 1,152 (26.3%) to `netdev_soft_c`, 187 (4.3%) to the copy primitive,
196 (4.5%) to the SANA-II reader, 33 (0.8%) to NetX TCP data checking, and
20 (0.5%) to NetX socket packet processing. Those functions have few attributed
samples in this workload; the rows do not rank individual emitted sequences
or establish their costs in the standalone TCP tests.

`netdev_soft_c()` calls `netdev_interrupt()` between `Disable()` and `Enable()`.
The level-4 sampler cannot interrupt that masked interval. A sample taken
after the mask is released is charged to the instruction after `Enable()`;
the existing Exec-call audit found 290 of 291 `netdev_soft_c` samples there in
an earlier profile. Therefore the 26.3% row is evidence about the masked receive
pass, including ring service, packet copy, callbacks, and Exec work. It is not
a per-function cost for the small C wrapper or a JIT instruction ranking.
The profile's `unsampled 0.0%` estimate does not resolve this attribution.

## One instruction-shape change

`netdev_rx_claim()` copied the 14-byte Ethernet header with a C byte loop in
both the batch and ordinary direct paths. The pinned GCC 16.2 compiler, at
both `-Os` and `-O2 -m68020-60`, made the loop a `MOVE.B (An)+,(An)+` followed
by `DBF`, repeated 14 times. It also emitted an overflow guard after the loop.
The fixed-size `__builtin_memcpy` now emits three adjacent
`MOVE.L (An)+,(An)+` and one `MOVE.W (An)+,(An)+` in the fat `-O2` build. The
same four moves were found in the linked fat LTO driver, so the final link did
not turn the copy into an out-of-line `memcpy` call. The builtin preserves byte
copy semantics for the 68000 build and host tests, including odd pointers.

In the Emu68 translator matching the installed `d9059de` base, memory-to-memory
postincrement `MOVE` loads then stores with one AArch64 instruction each when
the condition flags are dead. `DBF` emits counter extraction, subtraction,
insertion, a conditional branch, and a PC update on the loop path. Thus the
old header-copy body is roughly 14 × (2 + 5) = 98 executed AArch64
instructions, while the new body is four × 2 = 8. This is a source-based
emission count for this local sequence, not an observed speedup of the whole
TCP receive path. Register allocation, translation-unit boundaries, memory
latency, and interrupt frequency still determine the real benefit.

## Validation and next measurement

`test_netdev_direct` passes with both header-copy paths exercised. The
`anxgenet.device` cross-build passes its PC-relative branch check in
`-O2 -m68020-60` builds with and without LTO. Disassembly of the linked LTO
image contains the four moves at both copy sites.

Before another receive restructure, measure a whole-transfer control and an
A/B run on the PiStorm32 with identical GENET configuration, sender, packet
size, and library. Record throughput, received bytes, interrupt passes, batch
versus plain-read counts, checksum errors, drops, and retransmissions. The
complete bare-metal Emu68 runtime does not run as a Linux process. Its block
translator and integer emitters can be hosted on Linux for code generation,
as the adapter described below demonstrates. A native Linux microbenchmark is
insufficient for that final comparison. If the
change is below the rig's repeatability, retain only the proven instruction
shape and do not assign it a throughput number.

For larger gains, split the masked receive pass with existing GENET step
counters before changing the TCP state machine. Those counters can distinguish
cache maintenance, descriptor service, delivery, packet copy, verification,
and callback/Exec cost; the sampled `netdev_soft_c` row cannot.

## Actual ARM generation on Linux

The Emu68 fork now contains an emission-only Linux adapter in
`tests/linux_jit/`, on branch `codex/linux-jit-harness`, based on `d9059de`.
It generated actual ARM bytes on the Pi 4 for the standalone RX fixtures and
the linked fat LTO `tcpprof`. Its `ASSESSMENT.md` records the broader findings:
call/return and dispatcher overhead, repeated window-cap work, established
state-switch dispatch, CCR reconstruction, guest stack traffic, inlined
critical-section tails, and loop duplication. The annotated dumps retain each
guest instruction beside its emitted ARM instructions.

The initial candidates used temporary source copies and matched LTO controls.
The changes below are now applied to the stack and checked in the actual
linked library as well as `tcpprof`. The generated ARM was not executed;
instruction reductions are evidence about code shape, with no claimed
throughput improvement.

## Integrated changes and verification

NetX Duo's `nx_tcp_socket_packet_process.c` now tests established state at the
existing switch site, after all preceding validation and ACK work. The entire
established body moves together, including the TCP/IP-offload guard; the other
states retain their switch and the shared urgent-callback/packet-release tail.
`nx_tcp_socket_state_data_check.c` snapshots the cap only at the window-update
decision, after earlier callbacks. The port's `_tx_amiga_permit_finish` is
`noinline, cold`, preserving its nesting-counter and Exec rescheduling logic.

Actual linked output, `-O2 -m68020-60 -mregparm=3 -flto`:

| Check | Before | After |
|---|---:|---:|
| Library established-state discrimination, including old indirect-JMP unit exit | 29 ARM instructions and one dispatcher transition | 9 ARM instructions, no transition at this decision |
| Duplicate interface-cap reload/normalization block | 15 ARM instructions in the displayed unit | Removed; the snapshot supplies the threshold calculation |
| Embedded Exec Permit calls in the socket-processing region | 2 sites | 0; rare work calls the shared tail |
| Linked library text | 404,932 bytes | 398,180 bytes (-6,752, 1.67%) |
| Linked tcpprof text | 117,524 bytes | 113,788 bytes (-3,736, 3.18%) |

The direct call to data checking still follows the established-state test.
These counts do not imply that the entire packet path is twenty instructions
shorter: other LTO layout and inlining decisions also changed. Likewise, the
15-instruction removal describes one duplicate block, not the net cost of the
whole window decision. The library's first entry unit remains 781 ARM
instructions in both images because it stops before the changed decision.

Compact actual mappings are retained here:

- [Original dispatch](emu68-rx-arm/dispatch-before.txt)
- [Integrated dispatch](emu68-rx-arm/dispatch-after.txt)
- [Removed cap reload/check block](emu68-rx-arm/cap-duplicate-before.txt)
- [Integrated cap snapshot](emu68-rx-arm/cap-after.txt)
- [Source and binary hashes](emu68-rx-arm/manifest.json)

The Pi emitted all 2,837 selected before/after translation units successfully.
The data-check helper lost its public HUNK symbol during final LTO; its after
dump uses an analysis alias at the direct call target from the established
path. The alias changes no instruction bytes. Full images, mappings, and ARM
disassemblies are in `build/emu68-rx-codegen/`.

Both final links pass `check-pcrel-exact.py` with zero mismatches and zero
unscanned relocations. The host tests `tcp_rxpass`, `tcp_rxflood`,
`tcp_rxflood_seq31`, `tcp_rxflood_seq32`, `tcp_persist`, and `tcp_sws` pass.
The added rxflood regression drives the real packet processor with an ACK
handler that changes the socket state; it checks established, CLOSE-WAIT,
and FIN-WAIT-2 dispatch, packet ownership, and the shared urgent tail.

To reproduce generation, build `tcpprof` and `bsdsocket_library` using the
`jit-rx-fat-lto` configuration, relocate each HUNK with the Emu68 fork's
`tests/linux_jit/build_rx.py --hunk ... --out ...`, and save guest disassembly
with `m68k-amigaos-objdump -d -m m68k:68020`. On the Pi, run
`tests/linux_jit/dump_rx.py` for the socket packet processor and the data-check
call target, using the default depth 256, loops 8, and CCR scan 20. The adapter
and its limitations are documented in that fork's `tests/linux_jit/README.md`.

Guest/native flag fusion and the loop-count-1 bug require Emu68 translator
changes. The stack retains the normal JIT settings.
