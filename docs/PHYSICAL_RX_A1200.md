# Physical A1200 TCP receive investigation

This note records the 2026-08-24 investigation of the low TCP receive rate on
real hardware.  It is an evidence record and a reproduction guide, not a claim
that the defect is fixed.

The important result is that the affected machine is busy, but it is not
spending twice the necessary time copying payloads.  An exact profile found
5.7% true CPU idle and two bulk copies totalling 19.4% of busy samples.  It did
not find a third bulk copy.  The strongest remaining lead is packet-pool
starvation on a machine with no Fast RAM: an existing `FASTMEM=0` experiment
lost 59% of its throughput while repeatedly closing the receive window.  That
mechanism still needs a controlled measurement on the physical machine.

## Scope

The affected test machine was:

| Item | Value |
|---|---|
| Computer | stock Amiga 1200 |
| CPU | 14 MHz 68EC020 |
| Memory | 2 MB Chip RAM, no Fast RAM |
| Network card | EtherLink III PCMCIA |
| SANA-II device | resident `anxnet.device` |
| Direction | Linux peer to Amiga, TCP receive |
| Application | AmiNetXDuo `iperf -s -4` |
| Diagnostic build | non-LTO, commit `4d0a5e23` |
| Profiler | interrupt sampler, 200 Hz |

This is not the same comparison as the user report that holds an X-Surf-100
and `x-surf-100.device` constant: AmiTCP_NG reaches 906 KB/s there and
AmiNetXDuo 412 KB/s.  The EtherLink III machine reached about 167 KB/s with an
unprofiled receive run.  It gives us an affected physical path that can be
profiled, but it does not by itself identify the cause of the X-Surf result.

Amiberry is also not a substitute for either result.  In the emulator
AmiNetXDuo leads AmiTCP_NG, so an emulator profile can reject an emulator-side
hypothesis but cannot establish the cause of a deficit that it does not
reproduce.

## Reproducing the profile

Follow the validation, staging and symbolization procedure in
[`tools/profiler/ReadMe`](../tools/profiler/ReadMe).  In particular, keep the
unprofiled throughput control, the raw profile, and the exact binaries from the
same build.  Do not compare a non-LTO diagnostic profile's throughput with an
LTO shipping build.

The successful physical run used this guest command:

```text
SYS:Profile-current QUIET RATE=200 SAMPLES=3000 \
    OUT=SYS:iperf-current.prof SYS:rx-current-prof-iperf -s -4
```

Wait until `iperf` says it is listening, then start the peer.  The independent
repository peer command used for this run was:

```sh
python3 tests/tools/iperfpeer.py send tcp AMIGA_ADDRESS \
    --seconds 9 --length 4096
```

The peer must be a third machine.  Record its exact command and address with
the result; do not rely on the machine name surviving in somebody's shell
history.  This run transferred 1,458,176 bytes at 1,295,437 bit/s according to
the peer and about 1.26 Mbit/s according to the guest.

The machine normally exposes the HTTP shell at `/shell` and `SYS:` through
WebDAV.  Keep both IPv4 and IPv6 access working while changing the IPv4 receive
path; otherwise the experiment removes its own recovery channel.  These are
convenient staging paths, not a substitute for a bootable recovery copy of the
library.

The sampler stored 2,002 samples over 9.95 seconds with zero dropped and zero
unresolved samples.  Its containment test covered 502 of 502 exact PAL frames,
and no sample found interrupts masked.  Those checks matter: a plausible list
of symbols from a wrongly decoded exception frame is not a profile.

## What the CPU did

Exec's idle `stop #$2000` state accounted for 115 of 2,002 samples, or 5.7%.
The remaining 1,887 samples were busy.  Approximately 68% of all samples were
in task context and 31% in supervisor or interrupt context.

The broad busy-time attribution was:

| Area | Busy samples |
|---|---:|
| `iperf` and its surrounding application work | 40% |
| SANA-II reader | 39% |
| other background work | 3% |
| AmiNetXDuo library task work | 3% |
| DHCPv6 | 2% |
| system tick | 2% |

The leading exact functions were more useful than those broad ownership
labels:

| Function | Samples | Share of busy time | Meaning |
|---|---:|---:|---|
| `_n68k_port_in_w_sum` | 242 | 12.8% | EtherLink FIFO to `NX_PACKET`, with checksum fused into the copy |
| `_n68k_copy_bytes_mv20` | 125 | 6.6% | `NX_PACKET` payload to the application buffer |
| `_n68k_rx_verify_sum` | 34 | 1.8% | validates/consumes the carried checksum; it is not another payload copy |
| `_bsd_event_post` | 14 | 0.7% | records socket events and wakes a waiter |
| Exec `Signal` | 4 | 0.2% | the signal operation itself |
| timer.device LVO -36 | 65 | 3.4% | timer work |

The two visible payload copies are necessary at the current interfaces: data
must leave the card's I/O FIFO for an `NX_PACKET`, and `recv()` must place it in
the caller's buffer.  Together they are 19.4% of busy time.  The profile has no
third bulk-copy function large enough to explain a factor-of-two gap.  It also
shows that alignment or cache behaviour cannot be the whole explanation on
this 68EC020: even eliminating both required copies would recover less than a
fifth of busy time.

The whole resident driver accounted for 446 busy samples, or 23.6%:

| Driver function | Samples | Busy share | Context |
|---|---:|---:|---|
| `_n68k_port_in_w_sum` | 242 | 12.82% | interrupt |
| `_netdev_begin_io` | 67 | 3.55% | task |
| `_el3_intr` | 31 | 1.64% | interrupt |
| `_netdev_rx_claim` | 23 | 1.22% | interrupt |
| `_bus_rdata` | 16 | 0.85% | interrupt |
| `_netdev_rx_claimed` | 13 | 0.69% | interrupt |
| `_el3_get` | 12 | 0.64% | interrupt |
| `_netdev_tick` | 8 | 0.42% | task/interrupt |
| `_netdev_take` | 8 | 0.42% | task |
| `_el3_put.constprop.0` | 5 | 0.26% | task |

A temporary counter independently reported `direct_rx=9330`, proving that the
direct PIO receive path was active.  The counter patch was reverted after the
measurement.

The hottest named NetX Duo functions were small by comparison:

| Function | Samples | Busy share |
|---|---:|---:|
| TCP socket packet processing | 46 | 2.4% |
| TCP packet processing | 32 | 1.7% |
| packet trim from front | 29 | 1.5% |
| TCP timestamp processing | 28 | 1.5% |
| TCP ACK checking | 28 | 1.5% |

## Resolving the LTO driver

The loaded resident device, not the file currently present in `DEVS:Networks`,
must be symbolized.  Its three loaded hunks were 30,712, 140 and 244 bytes.
Those sizes matched the exact `anxnet.device` built for the run.

The original LTO link had discarded its temporary `*.ltrans.o` symbol tables.
To recover names without guessing, the same driver was rebuilt with GCC's
`-save-temps=obj`.  The rebuilt device had both the same hunk sizes and this
SHA-256 as the run's device:

```text
788626037396740970d22623dbd8dff1450a73a283af53534405af651f1ce6c9
```

The saved `*.ltrans.s` was then reassembled so its internal labels could be
mapped to the sampled PCs.  This is an exceptional recovery technique, not a
replacement for preserving the exact build objects.  A matching source commit
or a plausible nearby label is insufficient; require an identical final
binary before using regenerated LTO assembly to name samples.

## Hypotheses rejected by measurement

### A hidden second stack copy

Rejected for this run.  The complete exact profile found the device-to-packet
and packet-to-application copies, totalling 19.4% of busy samples, and no third
bulk pass over the payload.  The fused receive checksum avoids a separate
checksum walk.

### One baton handoff for every frame

Rejected.  A bounded continuous-drain experiment compared a maximum burst of
four with a maximum of one:

| Arm | Frames | Baton transitions | Frames/transition | Drops/overruns | Rate |
|---|---:|---:|---:|---:|---:|
| maximum 4 | 1,292 | 676 | 1.91 | 0 | about 1.488 Mbit/s |
| maximum 1 | 1,288 | 671 | 1.92 | 0 | about 1.496 Mbit/s |

The change produced neither extra batching nor a throughput improvement.  It
was not merged.  Earlier descriptions of the receive path as paying one baton
handoff per frame should not be repeated.

### Unconditional `WaitSelect()` event signals

This is real avoidable work, but not the factor-of-two defect.  An experiment
made the private event signal edge-triggered while preserving ThreadX's own
wake for blocking receives.  All 97 host tests passed and both LTO and non-LTO
cross-builds succeeded.  In the physical profile, however,
`_bsd_event_post` plus Exec `Signal` occupied less than 1% of busy time.  The
experiment remains unmerged because its benefit is below the resolution of
the throughput problem and it changes delicate wakeup semantics.

### Receive burst size

Rejected by the maximum-four versus maximum-one result above.  Neither arm
dropped frames or overran the device, and both averaged about 1.9 frames per
baton transition.

## Experiment disposition

This was the state when the note was written.  The names are an audit trail,
not a request to merge the branches:

| Experiment | State | Disposition |
|---|---|---|
| `codex/rx-continuous-burst`, commit `4d0a5e23` | physical maximum-four/maximum-one measurement complete | neutral result; do not merge as a throughput fix |
| `codex/rx-event-signal-gate` | 97 host tests pass; LTO and non-LTO builds pass | profile caps the direct benefit below 1%; leave unmerged |
| `codex/lowmem-pool-share` | measurement build for a configurable pool divisor | unfinished; use only for the controlled physical A/B |
| clean LTO `origin/main`, commit `08fe2a21` | built and staged as the control | boot locked; investigate separately from throughput |

The known-good physical diagnostic image was the non-LTO
`rx-current-prof.library` built from `4d0a5e23`.  A staged event-signal image and
the pool-share build are experiments, not fallback libraries.

## Strongest remaining lead: the low-memory receive window

The stock A1200 has no Fast RAM and only 2 MB of Chip RAM shared by the system,
applications, driver and stack.  AmiNetXDuo currently allocates the packet pool
from one sixteenth of `AvailMem(MEMF_PUBLIC)`:

```text
packets = (AvailMem(MEMF_PUBLIC) / 16) / packet_stride
```

The result is clamped to 16 through 512 packets.  Each packet carries a
1,568-byte payload plus its metadata and alignment.  A new TCP socket then gets
one eighth of the pool payload budget, divided among live TCP consumers, with
an 8,192-byte floor.  The relevant implementation is
`ami_ns_pool_packets()` in `src/netstack/netstack.c` and
`ami_bsd_tcp_window()` in `src/bsdsocket/socket.c`.

An existing controlled `FASTMEM=0` emulator experiment is the closest matched
memory result:

| Observation | Result |
|---|---:|
| maximum advertised receive window | fell from 39,200 to 8,192 bytes |
| zero-window advertisements | 108 in 10 seconds |
| time with receive window shut | 13.5% |
| Linux sender receive-window limited | 31.9% |
| retransmission | 0.233% |
| throughput loss | 59% |
| longest individual zero window | 38 ms |

The sender filled the advertised window, and the retransmission rate is two
orders of magnitude too small to account for the loss.  Short zero-window
periods and prompt updates also rule out the TCP persist timer.  This is packet
buffer starvation, not missing frames.

That 59% loss is the strongest explanation currently consistent with the
physical machine's memory class and the factor-of-two symptom.  It is still an
inference: the physical profile did not record packet-pool low water, window
advertisements or sender `rwnd_limited` time.  The earlier emulated 68020 result
that throughput was flat above an 8 KB window used a configuration whose sender
was application-limited; it does not contradict a run in which the pool
empties and repeatedly advertises zero.

Do not test this by increasing only the advertised TCP window.  A larger number
does not create the `NX_PACKET`s needed to hold it.  The controlled test is to
increase the pool's memory share, initially from one sixteenth to one eighth,
while leaving the TCP window derivation intact.

For each arm, record all of the following from the same boot and workload:

- free public memory before stack start, after stack start and after the run;
- packet-pool total, low-water mark, empty requests and allocation failures;
- maximum advertised receive window, zero-window count and time closed;
- sender `rwnd_limited` time, retransmissions and drops; and
- unprofiled throughput, followed by a profile only if the arm changes it.

A useful result must show the whole causal chain: more pool storage, fewer or
no zero windows, less sender receive-window limitation, and higher throughput
without an unacceptable memory cost.  A faster one-off run alone is not enough.

## LTO boot warning and recovery

During this investigation the known-good non-LTO diagnostic library was
replaced with a clean LTO `origin/main` build at `08fe2a21`.  The next reboot
locked the machine.  This is a serious separate observation, but it is not yet
proof of an LTO miscompile: both optimization mode and binary changed, and no
minimal reproducer or failing instruction has been isolated.  Do not attribute
the receive-rate defect to LTO from this event.

The machine was staged with a known-good recovery copy named:

```text
SYS:rx-current-prof.library
```

If it locks again, boot without Startup-Sequence and restore it with:

```text
Copy SYS:rx-current-prof.library LIBS:bsdsocket.library
```

Then perform a cold or physical reboot.  Do not use another software reboot
until the restored library has been verified.  Other experimental staged
libraries are measurements in progress, not recovery images.

## Current conclusion

The physical 68EC020 is not sitting idle for half the transfer: it was idle
5.7%.  The driver and both required copies are visible and finite, and there is
no evidence for a duplicated full-payload copy.  Scheduler signals and receive
burst sizing are too small or measured neutral.  The next high-value test is a
packet-pool memory-share A/B on the recovered physical A1200, with TCP window
and sender limitation captured at the same time.

Until that A/B is complete, the only defensible status is: the physical
throughput defect is reproduced and profiled, its copy and wakeup explanations
have been narrowed substantially, and low-memory receive-buffer starvation is
the leading unproven mechanism.
