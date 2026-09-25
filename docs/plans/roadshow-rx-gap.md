# Roadshow receive gap on `x-surf-100.device`: test protocol

User-selected performance priority as of 2026-09-24. The protocol exists to
answer one question before any mechanism is named or any change is made:

> On one machine, with one `x-surf-100.device` binary and a **current pinned
> AmiNetXDuo build**, does AmiNetXDuo receive TCP data more slowly than
> Roadshow 1.15, and by how much, with an interval?

## Evidence so far

One post: EAB thread 123359, [post 1810965](https://eab.abime.net/showpost.php?p=1810965)
(26 Aug 2026). Method, from [post 1806771](https://eab.abime.net/showpost.php?p=1806771),
an earlier build cited for the method only: Fitz 1.21 (Aminet), a file the
post gives as "10e7 bytes" (read literally, 100,000,000 bytes), a Linux file
server, kb = 1000 bytes. Machine, inferred from the poster's standing rig
(posts 1807384 and 1807759), since post 1810965 names none: an A3000 with a
CyberStorm MkII 68060 and an X-Surf 100. The row in [BACKLOG.md](../BACKLOG.md)
records the read figures.

| Stack and driver | Read, server to `RAM:` (kb/s) | Write, `RAM:` to server (kb/s) |
|---|---|---|
| Roadshow 1.15 + `x-surf-100.device` | 949 | 855 |
| AmiNetXDuo 0.25.5 + `x-surf-100.device` | 918 | 1047 |
| AmiNetXDuo 0.25.5 + `anxnet.device` | 938 | 1089 |
| AmiTCP_NG 4.1.5, 68000 and 68040 builds, `x-surf-100.device` | 918 and 890 | 1033 and 1014 |

| Limit | Detail |
|---|---|
| Sample count | unstated; one reported figure per row |
| Not recorded | run order, reboots between stacks, driver version (1.16 assumed), `ENV:` SANA-II settings, Roadshow's request counts, CPU clock, OS, the server's NIC and duplex, which 0.25.5 build |
| Spread in the same table | two builds of one stack on one driver differ by 3.1%, the size of the gap |
| Historical | 0.25.5. Later x-surf reads were 252 (0.26.1, post 1811396) and about a third of expected (0.26.3, post 1812902), then the receive path, ACKs and queues changed through 0.28.x (CHANGELOG) |
| Noise floor | null controls on identical binaries read +0.22%, +2.75% and -0.29% (CHANGELOG, Receive); one A/B does not settle 1-3% |
| Write | Roadshow writes 18% slower in the same post, so "Roadshow is faster" does not hold in general |

**The AmiTCP bypass is ruled out as the gate, not as the origin.** The driver
reads the `AMITCP` port once, at Init. Roadshow 1.15 never creates that port.
An emulator proof on 2026-09-24 printed `AmiTCP optimizations enabled` only
when a forged port was present and `NOAMITCPOPT` was absent: absent, present,
absent. AmiNetXDuo hides its port around the X-Surf open
(`src/common/compat.c:471-474`). So both stacks drive the driver through
standard SANA-II. That places the difference in the stacks, but it does not
say where.

## Why the earlier A/B was invalid

| Design | Problem |
|---|---|
| Toggle `NOAMITCPOPT` in ENV between arms | the driver samples it at Init only, so without an expunge both arms run the first arm's state |
| Switch stacks in one boot (`RemoveNetInterface FORCE`, `Avail FLUSH`, re-add) | the device may never reach open count 0; this measures Roadshow's teardown, not receive speed, and every arm destroys the state of the one before |

## Smallest controlled A/B

| Control | Rule |
|---|---|
| Arms | **R**: Roadshow 1.15. **A**: AmiNetXDuo at a pinned release. Both use one `x-surf-100.device` file (hash recorded). No third arm until R vs A is settled |
| Arm selection | one stack per **boot**, chosen by a **one-shot** selector file that `S:Startup-Sequence` reads and deletes before any network start; with no selector, the machine boots AmiNetXDuo. The controller writes the selector over the door, then `C:Reboot`. A disk-loaded driver does not survive the reset, so Init runs fresh every boot. Stacks never share a boot, and no ENV toggle or teardown is involved. No power cycle: the shop switch also cuts the A1200 |
| Rollback | needs no network: the selector is gone after one boot, and a **local watchdog**, started before the stack, runs `C:Reboot` after a fixed bound unless the boot's script completes. A Roadshow boot that loses its door therefore returns to AmiNetXDuo on its own. Then verify the image hashes |
| Before any hardware GO | the emulator proves recovery: one deliberately failed Roadshow boot, the watchdog firing within its bound, the next boot coming up as AmiNetXDuo with its door working |
| Unit | the **boot**. 4 receive transfers per boot, averaged into one value. Position within a boot is worth about 1% (`tests/perf/run-rate-ab.sh`) |
| Order | alternate R/A/A/R across boots, so neither arm always runs first after power-on |
| Direction | server to Amiga only (receive), written to `RAM:` so no disk is involved. A fixed-length TCP stream from a peer server that sends from memory, 12 s or 16 MB |
| Client | **one** binary for both arms (hash recorded): plain `bsdsocket.library` socket/connect/recv into a `RAM:` file, fixed read size, timed by `timer.device`. No stack-specific tags. Peer-side byte count and timing kept as a cross-check |
| Workload cross-check | Fitz 1.21 copying the "10e7 bytes" file (100,000,000 bytes) from the server to `RAM:`, the original method, one transfer per boot beside the client's four. Without it, a "no material gap" verdict could reflect only a different workload |
| Recorded per boot | md5 of the stack library, the driver, the client and the Startup files; `AttnFlags`; `CacheControl` read-back; Z2/Z3 mode; FAST RAM map; link speed and duplex; the peer's kernel and NIC offloads; wall clock |
| Null control first | 3 A/A boots and 3 R/R boots measure this machine's between-boot spread before any R/A claim |
| Sample size | 20 boots per arm, 4 transfers per boot, paired R/A boot by boot: the design that resolved +1.34% on the emulator (CHANGELOG). The null boots check that 20 is enough on this machine and raise it if not |
| Report | per-boot means for each arm, their spread, and the paired R-A difference with a bootstrap 95% interval |
| Margin | declared before any run: **±1.5%** of AmiNetXDuo's median is "no material gap". It is under half the historical 3.4% and above the null controls' +0.22% and -0.29%, but not their +2.75% |
| Verdict | **gap**: the interval excludes zero and lies entirely beyond the margin. **No material gap**: the interval lies entirely within ±1.5%. **Inconclusive**: anything else, including an interval that contains zero but reaches past the margin. Neither the null spread nor an interval merely containing zero establishes equivalence |

**Machine.** The lab A3000 is a 68030/25, and the report came from a 68060.
A result there answers whether a same-driver gap exists on this A3000, not
what the 68060 number is. A 68060 reproduction waits for a 68060 card or for
the reporter's own runs. The emulator cannot price this: Amiberry services
a longword read as two `ne2000_wget` calls, which reverses the order. It is
for dry-running the selector, logging, watchdog and client only. Hypothesis,
unmeasured on a real 68060: a 68030 pays more than a 68060 for the misaligned
copy (H3), so a gap on the lab A3000 could be larger than the reported one.

## Measurements that separate mechanisms

Run only after a gap is reproduced. Each is passive or uses the same binary in
both arms. The driver (1.16, disassembly in the evidence lane) looks up only
`S2_CopyToBuff`, `S2_CopyFromBuff` and `S2_PacketFilter`, so our CopyToBuff16
and extension tags go unused and every stack gets plain CopyToBuff. It reads each whole
frame into a static buffer and, on the cooked path, copies from frame + 14,
always 2 mod 4. That is two copies a packet, the same path for any stack. The
raw path's source phase is not established.

| Hypothesis | Discriminating measurement | Reads as |
|---|---|---|
| H1: no current gap | the A/B above | a "no material gap" verdict; an inconclusive one leaves H1 open |
| H2: receive policy (ACKs, window) | server-side pcap per arm: ACKs per data segment, advertised window, gaps, retransmits | a difference in ACK spacing or window between the arms on the **same** driver. The cross-driver comparison (Roadshow over the vendor driver beating our own direct driver) motivates H2 but cannot isolate it, since the two driver paths differ in more than the copy |
| H3: copy phase | a `(from & 3, to & 3)` histogram and sampled EClock time in our CopyToBuff | our destination is 0 mod 4 against a 2 mod 4 source (`sana2_rx.c:719`; +30-37% of the copy on a 68020, `sana2_copy.c:60-110`). Roadshow's destination phase and the 68060 cost are unknown |
| H4: read starvation | outstanding-read low-water mark and zero-read count, plus the driver's own drop count (unit + 302) through its statistics | frames lost with no read posted show up only as TCP retransmits |
| H5: task priority | stack and driver task priorities per arm, from the status report and `Status` | a difference in who preempts whom during a burst |
| Guest CPU | idle share during the transfer, from a lowest-priority counter task | 0% idle in both arms: per-packet cost decides. Idle left: H2 or H4 |

## Ownership

| Lane | Owner |
|---|---|
| This protocol and the priority matrix | claudecode |
| EAB 123359 method extraction, `x-surf-100.device` 1.16 receive behaviour, driver-boundary counters | zz9k-fpga |
| AmiNetXDuo receive path: copy, wakeup, ACK and window | deepseek-v4 |
| Any hardware run or stack switch | only on codex-amiga's GO |
