# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
**ONE LINE PER ROW.** What is wrong and where. Not how it was found.
| Item | Why it is open | Cite |
|---|---|---|
| `-ffunction-sections` reaches LTRANS without `-fno-optimize-sibling-calls` | a pairing the tree calls indivisible is split | `src/common/CMakeLists.txt:144` |
| The guru after `NetShutdown` is unreproduced | a2065/A1200, 3 cycles: the task lists match name for name and `run-cycledrill.sh` is green; the report is a 3c589 | `tests/tools/run-cycledrill.sh` |
| CNet and CNet16 still do not attach on real hardware | the vendor `cnet16.device` does, so a layer fix or the 16-bit probe is wrong | `src/netdev/netdev_pcmcia.c` |
| A live IPv6-only interface cannot be reconfigured or release DHCPv6 | no ADDRESS6/CONFIGURE6 writer, no `NETSTATUS_DHCP6` | `src/bsdsocket/netstatus.c:1723` |
| A certificate whose public key is `id-RSASSA-PSS` is refused | `rsa_pss_pss_*`: the SPKI OID is neither RSA nor EC, so the parse stops | `third_party/netxduo/nx_secure/src/nx_secure_x509.c:239` |
| `tls.library` runs only on our own `bsdsocket.library` | a hand-coded private LVO, so Roadshow, AmiTCP and Miami get `TLS_ERR_NOSTACK` | `src/tlslib/tls_netx.c:33` |
| `tls.library` is callable from exactly one compiler | GCC extended-asm stubs, no `.fd` and no pragmas, so SAS/C and vbcc cannot | `include/aminetxduo/tlslib.h`, `developer/sfd/` |
| No ALPN, so HTTP/2 cannot be negotiated | the extension exists nowhere in nx_secure, `src/tlslib` or `include/` | `include/aminetxduo/tlslib.h` |
| Keystroke latency is 3.8x the requirement on a truecolour screen | 178 ms against 47 at 8-bit; every band re-resolves and re-locks the screen | `src/tools/httpfb.c:170` |
| `cpu68020` and `cpu68060` are red on three `tests/tls` images, and no longer on `tls.library` | twelve targets failed the pcrel check, nine of them on an interface `-ffunction-sections` since removed | `tests/tls/CMakeLists.txt` |
| Every diagnostic is compiled out of every shipped binary | `AMI_ERROR`/`WARN`/`INFO` need `AMINETXDUO_LOG`, which defaults OFF | `include/aminetxduo/compat.h:105` |
| A console session that works logs nothing | only the startup banner, so the log cannot say what was served or at what depth | `src/tools/httpd.c:7090` |
| `nu_TickPolls` and `nu_RxKicks` reach neither `netstat` nor `ShowNetStatus` | kept for a field report that can never quote them | `src/netdev/netdev_device.c:1123`, `:1291` |
| The tick task's catch-up count crosses no wire | `tx_amiga_tick_catchups` reaches only a serial dump shipped builds compile out | `port/threadx-amiga/inc/tx_amiga.h:173` |
| TARGET: X-Surf-100 reads 412 KB/s where AmiTCP_NG reads 906 | same machine, card and driver, never measured here; the A1200 half is answered | `src/netstack/netstack.c:325` |
| Real X-Surf hardware trails other stacks where emulation says we lead | 699 against 1103 KB/s, worst arm -37%; ACK-clock starvation is the hypothesis | `src/netdev/netdev_cards.c` |
| Five tcpdrill retransmission cases fail and no gate carries the number | it grades by emulator status rather than through `test-verdict.sh` | `tests/tcpdrill/run-tcpdrill.sh` |
| The `FASTMEM=0` zero-window count is 0 to 44 on the 8 MB arm from one boot to the next | same pool, same binary, same 15 s; nothing says which of the two is the machine | `tests/perf/run-poolshare.sh:141` |
| `anxnet.device` acknowledges 12 ms later than `cnet.device` | p50 35.2 against 23.0 ms; loss, window and cadence ruled out, register cost left | `src/netdev/dp8390.c` |
| The fused receive checksum stops at IPv4, so IPv6 frames are walked twice | both verify entries bail at the version gate; needs content-level RX tests | `src/net68k/n68k_rx_verify.c:81` |
| `-m68000` ships on compatibility alone | wire 2026-08-25: `any` 417.8, 68020 426.4 (+2.1%), 68060 441.7 KB/s; 68020 also smaller. Spread tracks zero-windows, not CPU | `cmake/toolchain-m68k-amigaos.cmake:261` |
| Three receive-path changes are right ideas whose implementations crash | RX on the SANA-II reader, re-arm before delivering, stop poking the scheduler | `src/sana2/sana2_rx.c` |
| The 3c589 RX FIFO-hold fix has no wire number | no emulator models a 3c589 and the A1200 is off-limits; proven only in the C mock | `src/netdev/test/test_netdev_el3.c:35` |
| The console pacing does not turn a cheaper pass into a sooner one | skipping cut duty to 24% of a 75% cap and latency got worse; see `FB_GRAB_FLOOR` | `src/tools/httpfb.c` |
| `__udivmoddi4`'s 64-bit-divisor branch is 6.6x libgcc's | 852 us against 129 on a 68020; a 64-iteration bit loop where libgcc uses Knuth D. Nothing shipped divides by more than 32 bits | `src/common/ami_udivdi3.c:162` |
| Server-side TLS is 2.8% of the library and unreachable | cutting it changes `NX_SECURE_TLS_SESSION`'s layout, so it needs a second build | `src/tls/CMakeLists.txt` |
| The TCP window ceiling has never been observed binding | it needs a saturated pool; the A3000 32 MB arm of `run-bigmem` was queued and did not report | `tests/tools/run-bigmem.sh:58` |
| The DHCP/RA absorb runs on the caller's stack with 680 bytes to spare | a 1280-byte frame on any task that resolves or reads live config | `src/netstack/netstack_dns.c:959` |
| `ami_rx_service` has a 2508-byte frame and nothing states its stack | it runs on the ARexx host's process and nothing records that process's size | `src/netstack/netstack_rexx.c:215` |
| 13 of 20 serial logs come back empty, so their assertions cannot fire | a harness with an empty input passes vacuously | `tools/amiberry-run.sh`, `tests/ipv6/run-bringup.sh` |
| `run-ifdhcp`'s bridged arm always fails because it asserts SLIRP's literals | derive the expected addresses from the arm instead | `tests/tools/run-ifdhcp.sh:92`, `:318` |
| `run-lossgate.sh` measures a clean link under an impaired name | warm-up and arm take two leases; netem impairs the warm-up's | `tests/perf/run-lossgate.sh`, `tools/emu-mac.sh` |
| Nothing enforces that a wired harness's runner ever FIRES | `run-socket.sh` names a workflow that is dispatch, cron and tags only | `tools/check-harnesses.sh`, `tests/ipv6/run-socket.sh` |
| Two socket files are still outside the host tier | `tcp_handler.c` needs the whole `ACTION_*` vocabulary; `transfer.c` pins 4.4BSD `iovec` | `tests/bsdsocket/CMakeLists.txt` |
| 9 harnesses are still unwired | one needs a segment with no DHCP server, one KS 2.04, three a bridged repair | `tools/check-harnesses.sh` |
| The fork's own regression suite has never run, on either side | `ci.sh host` does not configure it and the fork's workflow run count is zero | `tools/ci.sh` |
| The expunge refusal is proven at the library and the port, never at the joint | nothing tests that a failed stop leaves the started flag set | `src/netstack/netstack.c:1395`, `:1619` |
| The bring-up figure has never been measured on the build that ships | it reads `netstack: mark` lines only an `AMINETXDUO_LOG=ON` build emits | `tests/ipv6/run-bringup.sh` |
| No lab read-throughput measurement can predate v0.21.0 | `iperf.c` first shipped there, so the reported 699-to-420 bracket is unreachable | `tests/perf/run-fitzbench.sh` |
| No harness boots Kickstart 2.x with a Zorro network board | the romtag under 2.x and `card.resource` V37 are both unproven | `tests/tools/run-addifleak.sh`, `tests/perf/run-fitzbench.sh` |
| No console arm exercises a screen arriving mid-session | every arm connects to a screen already in front and settled | `tests/tools/run-console.sh`, `tests/tools/run-bootconsole.sh` |
| No resolution the lab tests is unaligned | every arm is a multiple of 16 with nothing to pad, which is how 1600 columns shipped | `tests/tools/run-console.sh`, `src/tools/httprtg.c:560` |
| `nc -l` from outside has never been exercised under Amiberry | it forwards with `uae_slirp_redir`, an FS-UAE option; a bridged guest needs none | `tests/tools/run-nettools.sh` |
| DHCPv6 server selection by preference is untested | no lab server can send OPTION_PREFERENCE, so a two-server link picks by arrival | `tests/ipv6/run-dhcpv6.sh` |
| The on-target source-selection arm cannot separate Rule 6 | one `ADDRESS6` per interface, so no guest holds a ULA and a global | `src/config/config_parse.c:769`, `tests/tools/run-srcsel.sh` |
| A release followed by a DHCP restart was observed silent on the wire | no DISCOVER where one was due, and the harness grades verdicts not the wire | `tests/tools/run-ifdhcp.sh:90` |
| A legacy one-shot mDNS query drew no observable answer | proper 5353-sourced queries are answered; ephemeral-port one-shots are not | `third_party/netxduo/addons/mdns/nxd_mdns.c:8871` |
| Nothing guards the usergroup.library hold ixemul clients depend on | no caller in our tree, so a tidy-up would silently close the stack for ixnet | `src/bsdsocket/library_runtime.c:46` |
| The crypto yield hook is installed by `tls.library` alone | anything linking `aminetxduo_tls` directly gets a NULL hook and holds the baton | `src/tlslib/tls_netx.c:95` |
| `rfbbench` cannot price the banded path | its timed loop always encodes a whole frame; `--bands` only affects the compare pass | `src/rfb/host/rfbbench.c` |
| HAM and EHB are proven on one machine and one resolution | left: an A600 for HAM6 and EHB on OCS, and anything but 320x256 lores | `tests/tools/run-console.sh`, `src/tools/httpfb.c` |
| PCMCIA parity stops short of the CFTABLE walk | walking every entry would plausibly reach the seven multifunction cards | `src/netdev/netdev_pcmcia.c` |
| The PCMCIA socket is never contended in the lab | `CARDF_IFAVAILABLE`'s rejection path and the later-`OpenDevice()` claim are untested | `src/netdev/netdev_pcmcia.c` |
| Amiberry cannot eject a PCMCIA card, so removal is unexercised | `pc_on_removed`/`pc_on_inserted` have never fired, nor the reset order | `src/netdev/netdev_pcmcia.c:363`, `:386` |
| Two card rows have never run on their hardware | `xsurf500` is from the iComp wiki and `3c589` from the 3Com manual | `src/netdev/netdev_cards.c` |
| `AMINETXDUO_AMIBERRY_MAC` is ignored for `ne2000_pcmcia` | Amiberry uses the host NIC address, so the second consecutive run has a dead RX | `tools/emu-board.sh`, `tests/tools/cards.sh` |
| Nothing checks the archive's contents against what is installed | `anxnet.device` shipped uninstalled for eleven releases unnoticed | `dist/make-dist.sh`, `install/Install-AmiNetXDuo` |
| `peercap_tcpdump_state` turns a transient ssh failure into a missing binary | it discards stderr and exits 2; two of its three callers have no retry | `tests/perf/peercap.sh` |
| Three more netdev poll bounds are still sized in iterations | `dp8390_halt` 900 vs 1214 us, `ed_attach` 5000 and `ne_probe` 100 wait on ISR.RST | `src/netdev/dp8390.c:86`, `src/netdev/ed.c:324`, `src/netdev/ne2000.c:459` |
| A submodule bump should pin the `master` merge, not a topic tip | check `cat-file -e` and `merge-base --is-ancestor` first; it fabricated an id once | `1d8b8a15`, `b8bb2bc8` |
| `C:ssh` is the one artefact that does not reproduce from its own tag | 44 `__FILE__` paths reach it and `-ffile-prefix-map` appears nowhere | `clients/dropbear/build.sh:89` |
| The ClassicWB hostname collides the way the MAC used to | `NAME` defaults to model plus variant, so two agents claim one mDNS name | `tools/classicwb.sh:114` |
| Our `telnet` answered no option negotiation the peer recorded | `telnet.c` implements WONT/DONT; may be netpeer returning mid-buffer | `src/tools/telnet.c`, `tests/tools/netpeer.py` |
| `aamprobe.c` hard-codes `a2065.device` twice | on any other board the re-add is refused with errno 6 and later asserts read poison | `tests/tools/aamprobe.c:669`, `:926` |
| binutils `amiga-2.46` cannot assemble gcc 16.2's own output | it forces a byte displacement on `jne`/`jeq`; we stay pinned at `amiga-2.39.0` | bebbo's `binutils-gdb` |
| The console records `.pfs` and nothing else can read it | an MP4 export needs a vendored muxer under the CSP and a lossless codec | `src/tools/web/client/console/pfs.ts` |
| No arm reaches the accelerated-Gayle timing ratio | an emulated bus read costs host time, not guest time; needs real hardware | `tests/tools/run-cpuspeed.sh:30` |
| No arm puts two network boards in one machine | Amiberry holds one board per family in a static; unit numbering and `CARD=` are untested | `tests/tools/cards.sh` |
| Payload content is unverified on a real 3c589 | the physical arm was never staged and Amiberry emulates no EtherLink III | `tests/tools/run-payverify.sh` |
| The budget has no ACK/TX leg | the transmit half of every received segment is uninstrumented, so a TX cut cannot be priced | `src/common/budget.c:22` |
| No arm varies the application read size | fetch is paid per recv(); a 32 KB read would pay it an eighth as often, never measured | `tests/perf/run-poolshare.sh:97` |
| A pending recv is never completed on the IP thread | priced +2-5% in src/bsdsocket alone; the realm was built instead and is rate-neutral | `src/bsdsocket/transfer.c:990` |
| The request gate's owner-death reap is proven by inspection only | nothing kills an opener mid-recv under the emulator | `port/threadx-amiga/src/tx_amiga_green.c:789` |
| The stray-Wait net covers Wait() only | a green thread blocking in WaitIO or WaitPort sleeps the whole realm and nothing counts it | `port/threadx-amiga/inc/tx_amiga.h:231` |
| The docs/*.md budget is 1150 where 800 was asked for | the non-campaign reference docs alone are 1129 lines | `tools/check-doc-budget.sh:17` |
| `ShowNetStatus` can print the CONFIGURED default route, not the live one | run-ifsurvive reads it back after a detach; it is non-zero either way once the survivor has a GATEWAY line | `tests/tools/run-ifsurvive.sh:315` |
| `AddNetInterface` cannot name what stood down for an add that SUCCEEDS | `report_what_yielded()` sees only `NETSTATUS_IF_NAMED`; read the event ring | `src/tools/addnetinterface.c:299` |
| No arm prices the 64-bit-divisor branch on a 68000 | rtdiv ran on an A1200 only; that branch now enters a 32-iteration fallback once per divide | `src/common/ami_udivdi3.c:87` |
| Three more harnesses name a serial log their runner never writes | the `-e` lane spelling on amiberry/winuae lanes; the greps match nothing | `tests/compare/run-legacy-client.sh:300` |
| A poolshare KB/s row cannot say the rig was quiet | the interlock is per-board, so CI's bridged guests share ens18 and the cores | `tools/amiberry-run.sh:270` |
| `ci.sh host` cannot pass without root on this box | the rig-lock selftest hard-fails when `ping` cannot probe; no unprivileged ICMP, no `cap_net_raw` | `tools/emu-rig-lock-selftest.sh:234` |
| Three tool headers stay over 20 comment lines | each header IS the `--help` text, printed by a hardcoded `sed -n` range, so trimming it needs a code edit | `tools/fetch-toolchain.sh:164` |
