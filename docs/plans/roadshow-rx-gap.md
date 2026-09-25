# Roadshow receive gap on `x-surf-100.device`: test protocol

User-selected performance priority as of 2026-09-24. The protocol exists to
answer one question before any mechanism is named or any change is made:

> On one machine, with one `x-surf-100.device` binary and a **current pinned
> AmiNetXDuo build**, does AmiNetXDuo receive TCP data more slowly than
> Roadshow 1.15, and by how much, with an interval?

## Evidence so far

| Item | Value | Missing controls |
|---|---|---|
| Roadshow 1.15 + `x-surf-100.device` | 949 KB/s | driver version and hash, tool, direction as measured, sample count, run order, CPU cache and Z2/Z3 mode |
| AmiNetXDuo 0.25.5 + `x-surf-100.device` | 918 KB/s (Roadshow +3.4%) | the same. **Historical**: 0.26.x-0.28.x changed the receive path, ACKs and queues (CHANGELOG), so this is not a current figure |
| AmiNetXDuo + `anxnet.device` | 938 KB/s | the same |
| Source | a user's A3000 with a 68060, EAB thread 123359, row in [BACKLOG.md](../BACKLOG.md) | the exact method is being extracted from the thread |
| Noise floor | null controls on identical binaries read +0.22%, +2.75% and -0.29% (CHANGELOG, the Receive section of the cycle that added them); one A/B does not settle 1-3% | 3.4% is at that floor |

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
| Arm selection | one stack per **boot**, chosen by a selector file read by `S:Startup-Sequence` before any network start. The controller writes the selector over the door, then `C:Reboot`. A disk-loaded driver does not survive the reset, so Init runs fresh every boot. Stacks never share a boot, and no ENV toggle or teardown is involved. No power cycle: the shop switch also cuts the A1200 |
| Rollback | selector back to AmiNetXDuo, then one boot, then verify the image hashes |
| Unit | the **boot**. 4 receive transfers per boot, averaged into one value. Position within a boot is worth about 1% (`tests/perf/run-rate-ab.sh`) |
| Order | alternate R/A/A/R across boots, so neither arm always runs first after power-on |
| Direction | peer to Amiga only (receive). A fixed-length TCP stream from a peer server that sends from memory, 12 s or 16 MB |
| Client | **one** binary for both arms: plain `bsdsocket.library` socket/connect/recv, fixed read size, timed by `timer.device`. No stack-specific tags. Peer-side byte count and timing kept as a cross-check |
| Recorded per boot | md5 of the stack library, the driver, the client and the Startup files; `AttnFlags`; `CacheControl` read-back; Z2/Z3 mode; FAST RAM map; link speed and duplex; the peer's kernel and NIC offloads; wall clock |
| Null control first | 3 A/A boots and 3 R/R boots measure this machine's between-boot spread before any R/A claim |
| Sample size | from that spread: enough boots per arm that the 95% interval of the median difference is narrower than 3.4%. The emulator's table (3% needs 2-4 rounds at 12 s) is not a hardware figure |
| Verdict | a gap is claimed only if the bootstrap 95% interval of the R-A median difference excludes zero and exceeds the null spread. Otherwise the 31 KB/s is recorded as not reproduced on this machine |

**Machine.** The lab A3000 is a 68030/25, and the report came from a 68060.
A result there answers whether a same-driver gap exists on this A3000, not
what the 68060 number is. A 68060 reproduction waits for a 68060 card or for
the reporter's own runs. The emulator cannot price this: Amiberry services
a longword read as two `ne2000_wget` calls, which reverses the order. It is
for dry-running the selector, logging and client only.

## Measurements that separate mechanisms

Run only after a gap is reproduced. Each is passive or uses the same binary in
both arms.

| Hypothesis | Discriminating measurement | Reads as |
|---|---|---|
| Receive window or ACK policy | peer pcap of one transfer per arm: advertised window, ACKs per data segment, data-to-ACK delay | fewer ACKs or a larger window in R with the same guest CPU points at TCP policy |
| Per-packet stack cost | guest idle share during the transfer, from a lowest-priority counter task (same binary in both arms) | both arms at 0% idle: per-packet cost decides. Idle left: latency or window decides |
| SANA-II read depth and copy path | driver-boundary counters (the evidence and driver lane): outstanding `CMD_READ`s, buffer-management hooks passed at `OpenDevice` | a shallower read queue or a slower copy hook in A shows at the driver boundary |
| Application read size | fixed by the shared client, and varied only as a second pass | a gap that moves with read size is on the socket side, not the driver side |

## Ownership

| Lane | Owner |
|---|---|
| This protocol and the priority matrix | claudecode |
| EAB 123359 method extraction, `x-surf-100.device` 1.16 receive behaviour, driver-boundary counters | zz9k-fpga |
| AmiNetXDuo receive path: copy, wakeup, ACK and window | deepseek-v4 |
| Any hardware run or stack switch | only on codex-amiga's GO |
