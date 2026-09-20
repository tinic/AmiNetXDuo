# ZZ9000 Ethernet: anxzz9000.device and the firmware fork

State of 2026-09-20 07:10 UTC; stopped here, codex takes a second look. Everything below was measured on the real
A3000 (68030/25, OS 3.2, 12 MB motherboard RAM, ZZ9000 at $48000000 Zorro III,
X-Surf 100 at $40000000 for the door) against a gigabit Linux peer on the
same switch. Numbers are one run each unless said otherwise.

## Where the work is

| what | where |
|---|---|
| driver `anxzz9000.device` | AmiNetXDuo branch `zz9000`: `src/netdev/zz9000.c`, `NETDEV_ROSTER_ZZ9000`, card rows appended after `genet`, `n68k_copy_longs_sum`, shared `netdev_rx_continues()` in `netdev_verify.[ch]` |
| firmware | github.com/tinic/zz9000-firmware branch `aminetxduo` (= codex's `console-encode-offload` + the commits below); built with Arm GNU 13.2.rel1 + bootgen (no docker, no Vivado on the rig) |
| flashed on the A3000 | `Work:Attic/ZZ9000-console/BOOT-bd64d.bin` (also `BOOT-l2fix`, `-asynctx`, `-loopgap`, `-l2lean`, `-bd64`, `-bd64b`, `-bd64c`); codex's original `BOOT.bin` kept beside them |
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
| 8 | **open**: with 15-frame runs NetX TCP drops ~9 merged heads per 12 s ("dropped on receipt", `nx_ip_tcp_receive_packets_dropped`), 25-30 retransmits; with runs of 2, zero | `netstat -s` tcp line; capture shows 1-5 segment holes right at the ack point, later data SACKed | not found. Suspects: `nx_tcp_socket_packet_process.c` window acceptance with `rx_window_current`, the merged head's length vs the window the ACK advertised, the fork's ramp/settle code |
| 9 | **open, blocker**: the A3000 freezes (both interfaces, httpd, ssh) under receive with our driver: after 1.3 MB with `ZZ_GRO_MAX 6` (twice), after 36.5 MB with 16 (once), after 42.5 MB with **CONTINUES off** (once) -- so not the mark; MNT's driver on the same firmware ran 60 s / 30.5 MB clean at 4.19 Mbit/s | `ss -ti` bytes_acked frozen, unacked 24; no display attached, so a Guru is indistinguishable from a bus hang | next arms, one 60 s soak each: sync TX forced (`fork = FALSE` in `zz_attach`; a build of it is in the rig scratchpad `srv/`), then firmware `l2fix` (32 BDs, no async) with this driver, then the stale-wait and the VB `zz_tick` reclaim. What MNT's driver does not do that ours does: INT6 top half + software interrupt drain under Disable(), async TX from a task under Forbid() with a blank-time reclaim, the direct claim into the reader's buffer, the beam-timed stale wait |

## Numbers

| configuration | TCP RX | retrans / 9 s | ACKs per segment |
|---|---|---|---|
| ZZ9000Net.device 2.2, MNT-derived fw | 3.1 Mbit/s | - | - |
| ZZ9000Net.device 2.2, fw bd64d | 4.19 (60 s) | - | - |
| anxzz9000, fw l2fix, sync TX | 4.6 | - | 0.85 |
| + async TX | 4.9 | 0 | 0.85 |
| + CONTINUES (runs of 2) | 4.8 | 0 | 0.85 |
| + `ANXDPOOLPACKETS 1024` | 8.0 | 0 | 0.06 |
| pool 2048, window 396 KB, 32 RX BDs | 3.7 | 275 | - |
| 64 RX BDs, fit 56 frames, runs of 2 | 7.7 | 2 | 0.05 |
| runs of 15 | 8.0 | 25-30 | 0.05 |
| httpd 3.8 MB download (TX) | 381 KB/s (disk-bound; X-Surf iComp 327) | | |

Profile at 4.7 Mbit/s (Profile, audio-channel sampler): idle ~30 % of the
transfer, bsdsocket.library 34 % of busy, the device 16 % (the window copy
under Disable: 490 us a frame at 1.3 us a longword), one 32-frame drain =
19.7 ms with interrupts off.

## What codex proposed and where it stands

| proposal | status |
|---|---|
| per-window ARCACHE=0000 in mntzorro.v (RX window non-cacheable, framebuffer stays 0xF) | agreed, the clean fix; needs a bitstream (Vivado, not on the rig). The firmware invalidation becomes the old-bitstream path; the firmware should read a REG3 capability bit and skip it |
| direct MMIO -> final copy | done |
| GEM checksum verdict in the slot header | not done; the GEM's RX_CHKSUM is on by default, BD status bits 23:22; needs an opt-in (a write to 0x8a) so `ZZ9000Net.device`'s unmasked length read keeps working |
| driver-side CONTINUES | done; see #7-#9 |
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
| 0 state | both doors answer (`curl -m 4 http://192.168.1.147/` and `.175`); `ShowNetStatus` shows `zz9000` on `ZZ9000Net.device` (boot config); `RAM:zzreg c0` = firmware 0x0208, `RAM:zzreg 8a` bit 15 = the fork's async TX is flashed |
| 1 build driver | worktree on branch `zz9000`: `cmake --build build/release --target anxzz9000_device`; copy `build/release/src/netdev/anxzz9000.device` into a directory a plain `python3 -m http.server 8766` serves (the rig, 192.168.1.184) |
| 1 build firmware | `~/zz9000-firmware` branch `aminetxduo`: `PATH=~/.cache/zz9000/arm-gnu-toolchain/bin:$PATH BOOTGEN=~/.cache/zz9000/bootgen/bootgen ./build_firmware.sh && ./build_bootimage.sh --output bootimage_work/BOOT-<tag>.bin` (Arm GNU 13.2.rel1 + bootgen from github.com/Xilinx/bootgen; 3 min; the linker's 1 MB low-section ASSERT is the size gate) |
| 2 stage | over the .175 web shell (`AMISH_TAKE=1 python3 ~/tools/anxd-webshell.py 192.168.1.175 80`, one command per line on stdin): `fetch http://192.168.1.184:8766/srv/<f> TO RAM:<f>`; a driver goes on with `Copy RAM:anxzz9000.device AmiNetXDuo:Devs/Networks/anxzz9000.device` (only our file; MNT's is never touched); `Echo >RAM:zz9k "DEVICE=AmiNetXDuo:Devs/Networks/anxzz9000.device*NUNIT=0*NCONFIGURE=DHCP*NMDNS=NO*NPRIORITY=0"` |
| 3 switch | over the .147 ssh door, one script, no `fetch` in it (the X-Surf goes deaf after downloads and the script then blocks with both addresses dark): `FailAt 21` / `RemoveNetInterface zz9000 FORCE` / `AddNetInterface RAM:zz9k TIMEOUT=40` / `Stack 65536` / `iperf -s -t 30 -q` / `NetDevStats DEVICE anxzz9000.device`. A new driver binary on a running unit: `RemoveNetInterface zz9k FORCE` / `Avail FLUSH >NIL:` / `Copy RAM:anxzz9000.device AmiNetXDuo:Devs/Networks/anxzz9000.device` / `AddNetInterface RAM:zz9k TIMEOUT=40` |
| 4 measure | from the rig: `iperf -c 192.168.1.175 -t 12`; three `ss -tin \| grep -A1 192.168.1.175:5001` snapshots for `rwnd_limited`, `snd_wnd`, `cwnd`, `rtt`, `retrans`; a capture needs root (`sudo tcpdump -ni eth0 -w x.pcap host 192.168.1.175 and port 5001`) and the retransmit analysis is a 20-line python over `tcpdump -nr` (loss run lengths, burst positions of lost originals). On the Amiga: `NetDevStats DEVICE anxzz9000.device`, `netstat -s` (tcp "dropped on receipt", interface "receive errors: checksum"), `RAM:zzreg 8c` (rx status), `8e` (dropped<<8 \| pause frames), `ac`/`ae` (GEM RX FIFO overruns, error interrupts), `a8` then `aa` (longest service-loop pass, its tag) |
| 5 read counters | on the fixed firmware "passes after a top half, empty" and "header appeared on spin / never appeared" must stay 0 -- a rise says stale headers again; "top halves: Ethernet pending" is bursts, not frames; "serial gaps" = the ARM's ring overflowed; "frames marked CONTINUES" / "runs" gives the run length; the shell's "Direct receive fills" should equal packets received |
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
6. Registers added by the fork: 0x8a TX status, 0xa8/0xaa longest service-loop pass and its tag, 0xac/0xae GEM RX FIFO overruns and error interrupts.
