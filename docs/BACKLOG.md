# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
**ONE LINE PER ROW.** What is wrong and where. Not how it was found.
| Item | Why it is open | Cite |
|---|---|---|
| The guru after `NetShutdown` is unreproduced | a2065/A1200, 3 cycles: the task lists match name for name and `run-cycledrill.sh` is green; the report is a 3c589 | `tests/tools/run-cycledrill.sh` |
| CNet and CNet16 still do not attach on real hardware | the vendor `cnet16.device` does, so a layer fix or the 16-bit probe is wrong | `src/netdev/netdev_pcmcia.c` |
| Keystroke latency on truecolour is unproven against the 47 ms requirement | 3.7x better on mean and 4.1x on p90 after the banded readback, but measured on A1200+uaegfx where the 178 ms baseline never was; the A3000 ClassicWB P96 rig is what would settle it | `tests/tools/run-console.sh`, `src/tools/httpfb.c` |
| TARGET: X-Surf-100 reads 412 KB/s where AmiTCP_NG reads 906 | same machine, card and driver, never measured here; the A1200 half is answered | `src/netstack/netstack.c:329` |
| Real X-Surf hardware trails other stacks where emulation says we lead | 699 against 1103 KB/s, worst arm -37%; ACK-clock starvation is the hypothesis | `src/netdev/netdev_cards.c` |
| The zero-window count is 0 to 44 on the default 8 MB Fast RAM arm from one boot to the next | 0, 44, 0, 38 across four boots of two binaries with identical pool arithmetic; `FASTMEM=0` is the arm that reads CLEANLY | `tests/perf/run-poolshare.sh:139` |
| `anxnet.device` acknowledged 12 ms later than `cnet.device` before the single-copy landing | p50 35.2 against 23.0, n=2, 2026-08-20; `a7576b83` changed the DP8390 receive path and `e14ed3b8` fixed the sub-MSS window that run was measured under, and ackscope has not been re-run | `src/netdev/dp8390.c`, `tests/perf/run-ackscope.sh` |
| Three receive-path changes are right ideas whose implementations crash | RX on the SANA-II reader, re-arm before delivering, stop poking the scheduler | `src/sana2/sana2_rx.c` |
| The 3c589 RX FIFO-hold fix has no wire number | no emulator models a 3c589; it is proven only in the C mock | `src/netdev/test/test_netdev_el3.c:35` |
| The server-side TLS handshake has never been run | the door is open now -- `TLSA_Server` plus a DER certificate and key load an identity, and the transport presents the type `_nx_secure_tls_session_start()` branches on -- but nx_secure's server state machine is vendored, unchanged and unexercised; it needs a tls.library server and client over loopback in one emulator | `src/tlslib/tls_server.c:89` |
| The TCP window ceiling has never been observed binding | it needs a saturated pool; the A3000 32 MB arm of `run-bigmem` was queued and did not report | `tests/tools/run-bigmem.sh:58` |
| No harness boots Kickstart 2.x with a Zorro network board | the romtag under 2.x and `card.resource` V37 are both unproven | `tests/tools/run-addifleak.sh`, `tests/perf/run-fitzbench.sh` |
| Nothing asserts that a running `httpd -C` picks up a screen that arrives after it started | `run-bootconsole.sh` opens one mid-session, then starts a SECOND httpd and asserts on that, so the per-session re-read is unexercised | `tests/tools/run-bootconsole.sh:72`, `src/tools/httpfb.c:722` |
| No resolution the lab tests is unaligned | every arm is a multiple of 16 with nothing to pad, which is how 1600 columns shipped | `tests/tools/run-console.sh`, `src/tools/httprtg.c:560` |
| `nc -l` from outside has never been exercised under Amiberry | it forwards with `uae_slirp_redir`, an FS-UAE option; a bridged guest needs none | `tests/tools/run-nettools.sh` |
| DHCPv6 server selection by preference is untested | no lab server can send OPTION_PREFERENCE, so a two-server link picks by arrival | `tests/ipv6/run-dhcpv6.sh` |
| The on-target source-selection arm cannot separate Rule 6 | one `ADDRESS6` per interface, so no guest holds a ULA and a global; Rule 6 does run off-target in `test_ipv6_srcsel_host.c` | `include/aminetxduo/config.h:191`, `tests/tools/run-srcsel.sh` |
| HAM and EHB are proven on one machine and one resolution | left: an A600 for HAM6 and EHB on OCS, and anything but 320x256 lores | `tests/tools/run-console.sh`, `src/tools/httpfb.c` |
| PCMCIA parity stops short of the CFTABLE walk | walking every entry would plausibly reach the seven multifunction cards | `src/netdev/netdev_pcmcia.c` |
| The PCMCIA socket is never contended in the lab | `CARDF_IFAVAILABLE`'s rejection path and the later-`OpenDevice()` claim are untested | `src/netdev/netdev_pcmcia.c` |
| Amiberry cannot eject a PCMCIA card, so removal is unexercised | `pc_on_removed`/`pc_on_inserted` have never fired, nor the reset order | `src/netdev/netdev_pcmcia.c:363`, `:386` |
| One card row has never run on its hardware | `xsurf500` is from the iComp wiki; a real 3c589 has run since 2026-08-22, by hand, with no lab arm and no wire payload check | `src/netdev/netdev_cards.c` |
| `AMINETXDUO_AMIBERRY_MAC` is ignored for `ne2000_pcmcia` | the board is not autoconfig, so `gayle.cpp:1590` passes a NULL `autoconfig_info` and `ethernet_getmac` never sees the address; an 11-line Amiberry patch is written and verified, and applying it needs root on each lab host | `tools/emu-board.sh:86`, `tools/amiberry-run.sh:284` |
| binutils `amiga-2.46` strips every relocation out of a hunk executable | exit 0, no diagnostic, `LoadSeg()` then relocates nothing: the 2.46 rebase dropped bebbo's `TARGET_AMIGA` carve-out in `copy_relocations_in_section()`. It assembles gcc 16.2 fine, which is what the row used to say. Only matters if the pin moves off `amiga-2.39.0` | `tools/build-toolchain.sh:35` |
| No arm reaches the accelerated-Gayle timing ratio | an emulated bus read costs host time, not guest time; needs real hardware | `tests/tools/run-cpuspeed.sh:30` |
| No arm puts two network boards in one machine | one `-N` per run and `emu_board_lines` is called once, so non-zero `UNIT` is untested; `CARD=` itself is exercised by the card sweeps | `tools/amiberry-run.sh:40` |
| Payload content is unverified on a real 3c589 | the physical arm was never staged and Amiberry emulates no EtherLink III | `tests/tools/run-payverify.sh` |
| The xmit leg has never been read on a transfer | built and wired, but no capture run has quoted it beside reap/stuff/post | `tests/perf/run-poolshare.sh:97` |
| No arm varies the application read size | fetch is paid per recv(); a 32 KB read would pay it an eighth as often, never measured | `tests/perf/run-poolshare.sh:97` |
| The docs/*.md budget is 1150 where 800 was asked for | the enforced ceiling still exceeds the requested ceiling by 350 lines | `tools/check-doc-budget.sh:17` |
| Three harnesses still reach the network through SLIRP | `run-livetools.sh` and `run-dnssearch.sh` hardcode 10.0.2.x, and `run-hangup.sh` needs the emulator host's loopback through SLIRP's alias, which a bridged guest cannot reach | `tests/tools/run-livetools.sh`, `tests/tools/run-dnssearch.sh`, `tests/tls/run-hangup.sh` |
| `run-tcpdrill.sh` still defaults to the fs-uae runner | `RUNNER="${AMINETXDUO_RUNNER:-fsuae}"`, and fs-uae is not what any lab host runs | `tests/tcpdrill/run-tcpdrill.sh:19` |
| Two harnesses are red on their own premise | `run-dnscache.sh` reads `$HD/host.pcap`, which nothing has written since fs-uae left; `run-resolvebreak.sh` arm 1 returns errno 22 in 1.02 s so arm 3 has nothing to interrupt, and it prints 85899340.92 s from -250 ticks | `tests/tools/run-dnscache.sh`, `tests/tools/run-resolvebreak.sh` |
