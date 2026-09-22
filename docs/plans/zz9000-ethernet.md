# ZZ9000 Ethernet: anxzz9000.device and the firmware fork

State of 2026-09-20 09:10 UTC. Everything below was measured on the real
A3000 (68030/25, OS 3.2, 12 MB motherboard RAM, ZZ9000 at $48000000 Zorro III,
X-Surf 100 at $40000000 for the door) against a gigabit Linux peer on the
same switch. Numbers are one run each unless said otherwise.

## Where the work is

| what | where |
|---|---|
| driver `anxzz9000.device` | independent MIT implementation (not derived from GPL `ZZ9000Net.device`): `src/netdev/zz9000.c`, `NETDEV_ROSTER_ZZ9000`, card rows appended after `genet`, `n68k_copy_longs_sum`; GRO classification now lives in the stack's ordinary SANA-II receive path |
| firmware | github.com/tinic/zz9000-firmware branch `aminetxduo` (= codex's `console-encode-offload` + the commits below); built with Arm GNU 13.2.rel1 + bootgen (no docker, no Vivado on the rig) |
| flashed on the A3000 | `BOOT-txcsum-mcast.bin` (multicast receive plus register 0xa6 = 0xc000: RX metadata and TX checksum insertion present); earlier candidates remain under `Work:Attic/ZZ9000-console/` |
| boot config | unchanged: `DEVS:NetInterfaces/zz9000` still names MNT's `ZZ9000Net.device`; ours is installed beside at `AmiNetXDuo:Devs/Networks/anxzz9000.device` and brought up from `RAM:zz9k` for a test |

## The card as the 68k sees it (measured)

| item | value |
|---|---|
| register block | +0x0000..0x1fff, every access served by the ARM's main loop: read 3.5-3.8 us, write 1.6 us; the 68k stalls in the bus cycle until the loop comes round (longest idle pass 235 us, tag: SDK mailbox task; 22 ms once at boot) |
| RX window | +0x2000, 16 KB = slot REG4 (`frames_backlog_read`) and the 7 after it; FPGA reads DDR through **S_AXI_ACP with ARCACHE=0xF** (mntzorro.v, MNT commit 0c24783, v1.6RC3, 2020); UWORD read 1.3 us, longword copy 4.6-4.9 MB/s |
| TX window | +0x8000, 8 KB = four 2 KB slots; UWORD write 1.0-1.4 us |
| interrupt | INT6 (INTB_EXTER); +0x04 write with bit 3 clear = enable word (bit 0 Ethernet), bit 3 set = ack (bit 4); read = pending mask; the ARM re-raises on every idle loop pass while enabled && backlog > 0 |
| slot | [len:2][serial:2][frame], serials skip 0 and 1; ack = write the serial to +0x82 |
| A3000 caches | CACR 0x2101, TT0 0x40008507 (X-Surf block, CI), TT1 0x403f8107 (all of Z3, cacheable); the ZZ9000 register block still reads fresh from supervisor mode -- no TT guard needed |

## Findings, in the order they were hit

| # | finding | evidence | fix |
|---|---|---|---|
| 1 | a fresh frame's header stays hidden 0.1-4 ms, often longer: the 68k's poll of the empty slot allocated the line in the ARM's **L2**; the ARM's strongly-ordered header write and the GEM's DMA go past L2 to DDR | driver counters: 20 interrupts per frame, "ARM had frames ready" while the window read serial 0; 3.8 MB download at 2 KB/s | fw `841987a`: invalidate the slot's L2 lines after each header write / clear |
| 2 | the synchronous send stalls the 68k bus until the GEM has sent (usleep(100) poll): 16 % of the CPU at 4.6 Mbit/s, all ACKs | profile: anxzz9000.device 8-9 % sampled with interrupts on = bus stalls | fw: `REG_ZZ_ETH_TX` bit 15 = async, slot in bits 12..11; `REG_ZZ_ETH_TX_STATUS` 0x8a (low half of the 0x88 longword) bit 15 present, 14..0 frames done; `TXBD_CNT` 4; SendHandler retires every completed BD |
| 3 | the sender was rwnd-limited 99.8 %: the A3000 advertises a 20 KB window (SYN-ACK win 19992, wscale 0) | `ss -ti`; pool = avail/16/1.7 KB ~ 300 packets, TCP budget /8, split among every live socket incl. listeners | `SetEnv ANXDPOOLPACKETS 1024` -> 31 KB window, 8.0 Mbit/s. Stack policy, not driver |
| 4 | with a bigger window every frame past the **32nd of a burst** is lost in the GEM, nothing counts it (no BUFFNA, no RXOVR, no serial gap) | capture: lost originals at burst positions 32,33,34 of 33-35; 25-275 retransmits / 10 s | fw: `RXBD_CNT` 64, HIGH watermark 120 pending, LOW 96; driver advertises 56 frames (`ZZ_ARM_RING_FRAMES_FORK`) so the window fit stays under the armed descriptors; card rows say 100 Mbit/s so `ami_bsd_tcp_window_burst_bound()` applies the fit |
| 5 | `Xil_L2CacheInvalidateRange` masks IRQs, disables L2 line fills, syncs per line; a lean INV_PA loop without polling **drops lines** (PL310) -> stale payload, half the frames failed the checksum | 152 of 300 verified | fw: poll INV_PA bit 0 per line, one sync at the end |
| 6 | invalidating the header line first and the payload after leaves a window in which a draining 68k reads stale payload lines (L2 prefetch past the polled header) | ~1 % unverified, retransmits | fw `bd64d`: payload lines first, then the header write, then line 0 |
| 7 | `ge_continues`' flag-byte key breaks every run at Linux's PSH (every ~5th segment): runs of 2 | 8,435 segments cost 431 ACKs; runs 2.1 | shared `netdev_rx_continues()` drops PSH from the key: runs of 15, 1 ACK per 20 segments |
| 8 | **fixed**: with 15-frame runs NetX TCP dropped merged heads that covered the whole receive window: their first byte was below the left edge and their last byte beyond the right edge, so the old endpoint-only test rejected them despite useful overlap | before: ~9 "dropped on receipt" and 25-30 retransmits per 12 s; the host regression presents a three-packet chain spanning both edges and verifies that exactly the window is queued, while chains wholly beside either edge remain rejected | NetX Duo `4ae7e902` tests the two half-open ranges for any non-empty intersection and leaves trimming to `_nx_tcp_socket_state_data_check()`; `test_tcp_rxflood_host.c` gates the covering and adjacent cases |
| 9 | **fixed**: an exact RX serial acknowledgement can be rejected when the ACP supplies a stale serial. The same presented frame then re-enters every 32-frame drain, which re-enables the level-six source and creates an unbounded INT6/software-interrupt storm; both interfaces and the whole machine appear frozen | forcing legacy ack `1` ran 120 s / 88.9 MB and crossed the 16-bit serial wrap, while the exact-ack variants froze at 1.12-42.5 MB; synchronous TX still froze at ~65 MB, excluding async TX. With the recovery below, the full async/CONTINUES build ran 150 s / 143 MB at 7.99 Mbit/s and both interfaces remained reachable. A test build then deliberately sent one wrong exact ack: the recovery counter became 1 and a 15 s follow-up transferred 15.3 MB at 8.21 Mbit/s, with zero device errors, overruns, serial gaps, retransmits, or checksum errors | when a nonzero serial repeats, the previous synchronous register write was necessarily rejected; do not deliver the duplicate, reset the GRO run, and use the firmware's reserved legacy ack `1` once to advance. Counter: `rejected serial acknowledgements recovered` |
| 10 | **fixed**: the GEM already validates IPv4/TCP/UDP checksums, but the 68k repeated the whole payload sum while copying each frame | a 60 s receive soak transferred 55.3 MB at 7.66 Mbit/s: 53,620 frames used the GEM verdict, 81 were rechecked, with zero checksum, ring, serial, or GEM errors. Twenty deliberately checksum-less IPv4 UDP datagrams increased only the fallback counter by 20 | firmware exposes bit 15 (present) and descriptor bits 23..22 for the currently presented slot in read-only register 0xa6. The driver trusts only TCP/UDP verdicts after its published structural checks; options, fragments, padding, malformed lengths, checksum-less UDP, IPv6, and old firmware retain the exact software verifier |
| 11 | **fixed**: the GEM ran with Xilinx' defaults, unicast + broadcast only -- no multicast frame ever reached the 68k, on any driver: no IPv6 neighbour discovery (solicited-node groups), no mDNS, no router advertisements | before: `ping6 <A3000 global>` from the peer: neighbour FAILED, `http 000`; the card had been answering IPv6 only for the peers whose ND cache still held it from a unicast exchange | fw `init_ethernet_buffers()`: `XEmacPs_SetOptions(MULTICAST)` + `HASHL`/`HASHH` = 0xFFFFFFFF (accept every group; the 68k filters). After the flash: ND REACHABLE, rtt 254-528 ms, `http 200` over the global address with MNT's driver as well as ours. Flashed as `BOOT-mcast.bin` |
| 12 | **fixed**: the GEM's full TX checksum insertion was enabled, but the stack still walked every TCP payload on the 68030 and the driver did not tell the GEM which writes could use it | matched three-run A/B on the same boot and firmware: software checksum 4.48/4.32/4.55 Mbit/s (mean 4.45), GEM insertion 4.65/4.64/4.63 Mbit/s (mean 4.64, +4.3%). The offload runs inserted 24,558 checksums with no new retransmits, bad packets, checksum errors, or device errors; a separate 60 s run sustained 4.60 Mbit/s | firmware register 0xa6 bit 14 positively reports that GEM full TX checksum insertion is enabled. The driver negotiates TCP/UDP only when that bit is present, validates the Ethernet/IPv4/transport headers, and zeroes the transport checksum only in the copied card-window frame. The retained NetX packet is never changed, so retry and fallback remain safe; old firmware retains software checksums |
| 13 | **fixed**: the vendor 2.8 RC driver follows `int2 = on` in ZZ9000.CFG; ours had assumed INT6 unconditionally | source audit against v2.8.0-rc3; the current card reports the key absent and continues on INT6 | query firmware config key 5 at attach and register the Exec server on INT2 only when both its value and presence word are nonzero; pre-2.3 firmware reads zero and retains INT6 |
| 14 | the official 2.8 RC3 driver is not a safe code base for this firmware unchanged: after its first 20.8 s / 10.5 MB receive control both A3000 interfaces stopped answering and required a cold cycle | 4.23 Mbit/s before the hard hang; no post-failure counters were reachable. The updated `anxzz9000.device` immediately followed with 30.4 s / 28.0 MB at 7.72 Mbit/s, both doors still live, zero retransmits, checksum, ring, serial, or device errors | keep the independent bounded serial-ack recovery; upstream the protocol and fixes in reviewable pieces rather than replacing this core with the GPL driver |
| 15 | **fixed** (fd7af15e): the payload sits 2 mod 4 in the receive slot, and the hardware-checksum fast path and the staging copy handed that source to the plain longword copy, so every longword came off Zorro III in two word cycles; the summed path had always peeled one word first | probe on the A3000, 2026-09-21: window longword reads 1934 ns aligned against 2554 ns at 2 mod 4 (+32 %). Same-boot ABABABAB, 12 s RX, 4 legs per arm: median 5.675 -> 6.055 Mbit/s (+6.7 %), 4 of 4 pairs, ranges overlap -- suggestive at n=4 | `zz_copy_payload()` in `src/netdev/zz9000.c`, the summed copy without the sum, at both sites; `test_netdev_zz9000` proves every bulk source 0 mod 4 |

## Numbers

| configuration | TCP RX | retrans / 9 s | ACKs per segment |
|---|---|---|---|
| ZZ9000Net.device 2.2, MNT-derived fw | 3.1 Mbit/s | - | - |
| ZZ9000Net.device 2.8 RC3, fork fw | 4.23 (20 s; then hard hang) | 0 before hang | - |
| anxzz9000, fw l2fix, sync TX | 4.6 | - | 0.85 |
| + async TX | 4.9 | 0 | 0.85 |
| + CONTINUES (runs of 2) | 4.8 | 0 | 0.85 |
| + `ANXDPOOLPACKETS 1024` | 8.0 | 0 | 0.06 |
| pool 2048, window 396 KB, 32 RX BDs | 3.7 | 275 | - |
| 64 RX BDs, fit 56 frames, runs of 2 | 7.7 | 2 | 0.05 |
| runs of 15 | 8.0 | 25-30 | 0.05 |
| + GEM RX checksum verdict | 7.66 (60 s) | 0 | 0.05 |
| 2026-09-21, baseline bitstream (ARCACHE 0xF), no L2 invalidate in fw, MNT ZZ9000Net.device, 12 s | 4.35 / 4.41 / 4.50 / 4.51, median 4.455 | 111/55/55/71 | - |
| same, RX window ARCACHE=0011 | 4.11 / 4.12 / 4.12 / 3.88, median 4.115 (-8.3 %, ranges disjoint) | 60/67/98/48 | - |
| same boot and firmware, anxzz9000 fefd05ca (payload copy 2 mod 4 off the window), legs 1/3/5/7 of ABABABAB | 5.73 / 6.02 / 5.62 / 4.82, median 5.675 | - | - |
| same, anxzz9000 fd7af15e (window read longword aligned), legs 2/4/6/8 | 5.86 / 5.97 / 6.14 / 6.63, median 6.055 (+6.7 %, won 4 of 4 pairs, ranges overlap: suggestive, n=4) | - | - |
| httpd 3.8 MB download (TX) | 381 KB/s (disk-bound; X-Surf iComp 327) | | |

For TCP transmit, a matched three-run test improved from 4.45 Mbit/s mean
with software checksums to 4.64 Mbit/s with GEM insertion (+4.3%). This is a
CPU saving rather than a new data-movement path; the Zorro writes remain the
dominant cost.

Profile at 4.7 Mbit/s (Profile, audio-channel sampler): idle ~30 % of the
transfer, bsdsocket.library 34 % of busy, the device 16 % (the window copy
under Disable: 490 us a frame at 1.3 us a longword), one 32-frame drain =
19.7 ms with interrupts off.

The CONTINUES rows above are measurements of the retired driver-side GRO
prototype. Current builds classify and coalesce runs in the stack.

## What codex proposed and where it stands

| proposal | status |
|---|---|
| per-window ARCACHE=0011 in mntzorro.v (RX window normal/non-allocating, framebuffer stays 0xF) | **rejected in the tested configuration** (2026-09-21, zz9k-fpga's bitstream, codex-amiga's verdict): on MNT's driver, with neither firmware performing an L2 invalidate, 0x3 read 8.3 % slower than 0xF with disjoint ranges (table above). The stale-serial problem it was meant to fix is handled by the driver's bounded serial-ack recovery; REG3 bit 12 stays reserved for it should a firmware that skips invalidation ever need it |
| direct MMIO -> final copy | done |
| GEM checksum offload | RX and TX done without changing the slot ABI: read-only register 0xa6 returns bit 15 RX metadata present, bit 14 TX full-checksum insertion enabled, and BD status bits 23..22 for the current RX slot. MNT's driver is unchanged and was exercised after flashing; an AmiNetXDuo driver on old firmware sees zero and keeps both software paths |
| stack-side GRO | done; the SANA-II receive layer classifies ordinary verified frames, and NetX window-edge acceptance is fixed and regression-tested; see #7-#9 |
| READ_BATCH / RX_POLL / batched replies | `rx_holds` possible with the 128-slot ring; batched replies were measured a loss on a real 68k |
| drain several slots per wakeup | done (up to 32; bursts of 32 seen) |
| receive buffers in ZZ9000 fast RAM, GEM DMA into them | not primary: saves the fast-RAM copy (143 ns/B) but keeps the Zorro read (213 ns/B), and the same L2/ACP exposure |

## Procedure: driving a driver or firmware candidate on the A3000

One variable per run. 12 s runs for a number, 60 s soaks for stability;
every run records throughput, `retrans`, and the driver counters. The
machine has no display: a hang costs a bench power cycle, and the A1200
shares that mains.

| step | how |
|---|---|
| 0 state | both doors answer (`curl -m 4 http://<X-Surf door>/` and `http://<ZZ9000 door>/`; the addresses are DHCP leases and move -- 2026-09-21 they were .101 and .161); `ShowNetStatus` shows which driver holds `zz9000`; `RAM:zzreg c0` = firmware 0x0208, `RAM:zzreg 8a` bit 15 = the fork's async TX is flashed. **The 256 MB Zorro RAM must be mapped**: `Avail` shows about 273 MB free with a 256 MB largest block. A boot that shows ~12 MB fast and a few hundred KB free (the RTG framebuffer then sits in system RAM) invalidates every number taken on it; `C:Reboot` remapped it on 2026-09-21 |
| 1 build driver | main checkout: `cmake --build build/release --target anxzz9000_device`; copy `build/release/src/netdev/anxzz9000.device` into a directory a plain `python3 -m http.server 8766` serves (the rig, 192.168.1.184) |
| 1 build firmware | `~/zz9000-firmware` branch `aminetxduo`: `PATH=~/.cache/zz9000/arm-gnu-toolchain/bin:$PATH BOOTGEN=~/.cache/zz9000/bootgen/bootgen ./build_firmware.sh && ./build_bootimage.sh --output bootimage_work/BOOT-<tag>.bin` (Arm GNU 13.2.rel1 + bootgen from github.com/Xilinx/bootgen; 3 min; the linker's 1 MB low-section ASSERT is the size gate) |
| 2 stage | over the .175 web shell (`AMISH_TAKE=1 python3 ~/tools/anxd-webshell.py 192.168.1.175 80`, one command per line on stdin): `fetch http://192.168.1.184:8766/srv/<f> TO RAM:<f>`; a driver goes on with `Copy RAM:anxzz9000.device AmiNetXDuo:Devs/Networks/anxzz9000.device` (only our file; MNT's is never touched); `Echo >RAM:zz9k "DEVICE=AmiNetXDuo:Devs/Networks/anxzz9000.device*NUNIT=0*NCONFIGURE=DHCP*NMDNS=NO*NPRIORITY=0"` |
| 3 switch | drive the Amiga through the httpd web shell (`/shell`, row 2) on either door -- the control path does not have to be the interface under test, the data target does (2026-09-21: shell on the X-Surf lease, iperf to the ZZ9000 lease); the X-Surf ssh door is recovery only, for when httpd stops answering. To put our driver on the card: `RemoveNetInterface zz9000 FORCE` / `AddNetInterface RAM:zz9k TIMEOUT=40` / `NetDevStats DEVICE anxzz9000.device`. A new driver binary on a running unit: `RemoveNetInterface zz9k FORCE` / `Avail FLUSH >NIL:` / `Copy RAM:anxzz9000.device AmiNetXDuo:Devs/Networks/anxzz9000.device` / `AddNetInterface RAM:zz9k TIMEOUT=40`. Traffic is pinned to the ZZ9000 by addressing the ZZ9000 interface's lease; the X-Surf stays up |
| 4 measure | one fresh server per run, and never probe the port first: a connect to 5001 (a `/dev/tcp` check, `nc -z`) IS the client, the server takes it as a 1-byte run and exits, and the real client is refused. The control that works: from the web shell on the ZZ9000 door a plain `iperf -s -t 30 -q` (no `-w`, no `-l`, no `Stack`), then the rig's plain `iperf -c <ZZ9000 door> -t 12`. httpd also serves `/iperf/tcp-rx/<seconds>` (`src/tools/httpd.c`, `httpd_iperf_hook`), the same measurement without a Shell; builds before the fix that recognises `/iperf` as an application address answered 403 in volume mode, and it does not replace the recipe above until a build with that fix has answered on this bench. Confirmed 2026-09-21 on the ARCACHE firmware: 4.11 / 4.12 / 4.12 / 3.88 Mbit/s over four runs, peer retrans 60/67/98/48. Three `ss -tin \| grep -A1 <ZZ9000 door>:5001` snapshots on the rig for `rwnd_limited`, `snd_wnd`, `cwnd`, `rtt`, `retrans`; a capture needs root (`sudo tcpdump -ni eth0 -w x.pcap host <door> and port 5001`) and the retransmit analysis is a 20-line python over `tcpdump -nr`. On the Amiga: `NetDevStats DEVICE anxzz9000.device`, `netstat -s` (tcp "dropped on receipt", interface "receive errors: checksum"), `RAM:zzreg 8c` (rx status), `8e` (dropped<<8 \| pause frames), `ac`/`ae` (GEM RX FIFO overruns, error interrupts), `a8` then `aa` (longest service-loop pass, its tag). A `fetch` from the Amiga proves nothing about the ZZ9000 unless httpd's access log on the rig shows the request came from the ZZ9000 lease: on 2026-09-21 it came from the X-Surf |
| 5 read counters | on the fixed firmware "passes after a top half, empty" and "header appeared on spin / never appeared" must stay 0 -- a rise says stale headers again; "top halves: Ethernet pending" is bursts, not frames; "serial gaps" = the ARM's ring overflowed; the shell's "Direct receive fills" should equal packets received |
| 6 flash | `Copy RAM:BOOT-<tag>.bin RAM:BOOT.bin` then `ZZFwUpdate RAM:BOOT.bin` (the name must be BOOT.bin; keep a copy in `Work:Attic/ZZ9000-console/`), then a POWER CYCLE: HA `switch.wemo_switch_shop` off 30 s, on (a shorter hold is ignored; verify the A1200 goes dark); both machines are back in ~45 s; a warm `SyncReboot` does not reload the card and DOES bring its 256 MB Z3 RAM online (`Avail`), which changes where the packet pool lands |
| 7 profile | `RAM:Profile RATE=1000 SAMPLES=30000 OUT=RAM:x.prof AmiNetXDuo:C/iperf SERVER PORT 5002 TIME 25` from the web shell with `AMISH_QUIET=45` (the ssh door hangs it); the module table prints at the end; `tools/profiler/profreport.py --allow-unresolved --allow-stale x.prof` for tasks and gaps. A sample after a Disable() section lands on the instruction after Enable(): device time in `netdev_soft` is the drain |
| 8 hang | both doors dark and `ss -tin` frozen = the machine, not the link: power cycle, note MB-at-hang and the arm, one arm per soak; MNT's driver on the same firmware is the control (60 s clean) |
| 9 leave it | boot config untouched (MNT's driver at boot), our device file beside it, `RAM:` is gone at the next boot; `ENVARC:ANXDPOOLPACKETS` is currently 2048 from the window experiments -- delete it or leave it deliberately |

## Test recipe

1. Stage files over the .175 web shell (`python3 ~/tools/anxd-webshell.py 192.168.1.175 80`, `AMISH_TAKE=1`): `fetch http://192.168.1.184:8766/srv/<f> TO RAM:<f>`.
2. Switch over the .147 ssh door, never with a `fetch` in the same script
   (the X-Surf goes deaf after downloads; `Offline eth0`/`Online eth0` revives it):
   `FailAt 21` / `RemoveNetInterface zz9000 FORCE` / `AddNetInterface RAM:zz9k TIMEOUT=40`.
3. `RAM:zz9k`: `DEVICE=AmiNetXDuo:Devs/Networks/anxzz9000.device`, `UNIT=0`, `CONFIGURE=DHCP`, `MDNS=NO`, `PRIORITY=0`.
4. Measure: `iperf -s -t 30 -q` on the Amiga, `iperf -c 192.168.1.175 -t 12` here, `ss -tin` for rwnd_limited/retrans, `NetDevStats DEVICE anxzz9000.device`, `netstat -s`, card registers with `RAM:zzreg 8a|8c|8e|ac|a8`.
5. Firmware: `ZZFwUpdate RAM:BOOT.bin` (the file must be named BOOT.bin), then a power cycle -- a warm reboot does not reload the card, and a warm reboot brings the card's 256 MB Z3 RAM online while a cold one does not (`Avail`), which moves where the pool lands.
6. Registers added by the fork: 0x8a TX status; 0xa6 checksum capabilities
   (bit 15 RX metadata, bit 14 TX insertion) and current RX verdict in bits
   1..0; 0xa8/0xaa longest service-loop pass and its tag; 0xac/0xae GEM RX
   FIFO overruns and error interrupts.
