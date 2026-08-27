# Backlog
What is outstanding. Nothing else — finished work leaves a commit message and a
comment beside the code, not an entry here.
**HARD CAP: 250 LINES.** Over it, delete rows. Never relocate them; git has it.
**ONE LINE PER ROW.** What is wrong and where. Not how it was found.
| Item | Why it is open | Cite |
|---|---|---|
| The guru after `NetShutdown` is unreproduced | a2065/A1200, 3 cycles: the task lists match name for name and `run-cycledrill.sh` is green; the report is a 3c589 | `tests/tools/run-cycledrill.sh` |
| CNet and CNet16 still do not attach on real hardware | the vendor `cnet16.device` does, so a layer fix or the 16-bit probe is wrong | `src/netdev/netdev_pcmcia.c` |
| `tls.library` runs only on our own `bsdsocket.library` | a hand-coded private LVO, so Roadshow, AmiTCP and Miami get `TLS_ERR_NOSTACK` | `src/tlslib/tls_netx.c:33` |
| `tls.library` is callable from exactly one compiler | GCC extended-asm stubs, no `.fd` and no pragmas, so SAS/C and vbcc cannot | `include/aminetxduo/tlslib.h`, `developer/sfd/` |
| No ALPN, so HTTP/2 cannot be negotiated | the extension exists nowhere in nx_secure, `src/tlslib` or `include/` | `include/aminetxduo/tlslib.h` |
| Keystroke latency is 3.8x the requirement on truecolour | 178 vs 47 ms at 8-bit. NOT the re-resolve: a chunky band past the first skips it. Host prices encode at 1.9x, so the card readback carries the rest | `src/tools/httpfb.c:1316` |
| The whole-frame card readback is charged to every screen pass | 600 KB of Zorro per pass at 640x480 truecolour whether one tile changed or none; nothing reads back a band | `src/tools/httpfb.c:1386` |
| Every diagnostic SENTENCE is compiled out of every shipped binary | `AMI_ERROR`/`WARN`/`INFO` need `AMINETXDUO_LOG`, which defaults OFF; only the numeric event ring reaches a user, and one `C:` command reads it | `include/aminetxduo/compat.h:105`, `include/aminetxduo/events.h:20` |
| TARGET: X-Surf-100 reads 412 KB/s where AmiTCP_NG reads 906 | same machine, card and driver, never measured here; the A1200 half is answered | `src/netstack/netstack.c:329` |
| Real X-Surf hardware trails other stacks where emulation says we lead | 699 against 1103 KB/s, worst arm -37%; ACK-clock starvation is the hypothesis | `src/netdev/netdev_cards.c` |
| The zero-window count is 0 to 44 on the default 8 MB Fast RAM arm from one boot to the next | 0, 44, 0, 38 across four boots of two binaries with identical pool arithmetic; `FASTMEM=0` is the arm that reads CLEANLY | `tests/perf/run-poolshare.sh:139` |
| `anxnet.device` acknowledged 12 ms later than `cnet.device` before the single-copy landing | p50 35.2 against 23.0, n=2, 2026-08-20; `a7576b83` changed the DP8390 receive path and `e14ed3b8` fixed the sub-MSS window that run was measured under, and ackscope has not been re-run | `src/netdev/dp8390.c`, `tests/perf/run-ackscope.sh` |
| IPv6 fusing refuses any extension header and all of ICMPv6 | next-header not TCP/UDP declines outright; the chain is never walked, so ND and fragments still cost the stack a payload pass | `src/net68k/n68k_rx_verify.c:72` |
| Three receive-path changes are right ideas whose implementations crash | RX on the SANA-II reader, re-arm before delivering, stop poking the scheduler | `src/sana2/sana2_rx.c` |
| The 3c589 RX FIFO-hold fix has no wire number | no emulator models a 3c589; it is proven only in the C mock | `src/netdev/test/test_netdev_el3.c:35` |
| The console pacing does not turn a cheaper pass into a sooner one | skipping cut duty to 24% of a 75% cap and latency got worse; see `FB_GRAB_FLOOR` | `src/tools/httpfb.c` |
| Server-side TLS is 2.8% of the library and unreachable | cutting it changes `NX_SECURE_TLS_SESSION`'s layout, so it needs a second build | `src/tls/CMakeLists.txt` |
| The TCP window ceiling has never been observed binding | it needs a saturated pool; the A3000 32 MB arm of `run-bigmem` was queued and did not report | `tests/tools/run-bigmem.sh:58` |
| Nothing enforces that a wired harness's runner ever FIRES | `run-socket.sh` names a workflow that is dispatch, cron and tags only | `tools/check-harnesses.sh`, `tests/ipv6/run-socket.sh` |
| Two socket files are still outside the host tier | `tcp_handler.c` needs the whole `ACTION_*` vocabulary; `transfer.c` pins 4.4BSD `iovec` | `tests/bsdsocket/CMakeLists.txt` |
| 9 harnesses are still unwired | one needs a segment with no DHCP server, one KS 2.04, three a bridged repair | `tools/check-harnesses.sh` |
| The expunge refusal is proven at the library and the port, never at the joint | nothing tests that a failed stop leaves the started flag set: `ami_ns_kernel_stop_locked()` returns without clearing it, and `netstack_can_unload()` reads it | `src/netstack/netstack.c:1742`, `:2020` |
| The bring-up figure has never been measured on the build that ships | it reads `netstack: mark` lines only an `AMINETXDUO_LOG=ON` build emits | `tests/ipv6/run-bringup.sh` |
| No harness boots Kickstart 2.x with a Zorro network board | the romtag under 2.x and `card.resource` V37 are both unproven | `tests/tools/run-addifleak.sh`, `tests/perf/run-fitzbench.sh` |
| Nothing asserts that a running `httpd -C` picks up a screen that arrives after it started | `run-bootconsole.sh` opens one mid-session, then starts a SECOND httpd and asserts on that, so the per-session re-read is unexercised | `tests/tools/run-bootconsole.sh:72`, `src/tools/httpfb.c:2036` |
| No resolution the lab tests is unaligned | every arm is a multiple of 16 with nothing to pad, which is how 1600 columns shipped | `tests/tools/run-console.sh`, `src/tools/httprtg.c:560` |
| `nc -l` from outside has never been exercised under Amiberry | it forwards with `uae_slirp_redir`, an FS-UAE option; a bridged guest needs none | `tests/tools/run-nettools.sh` |
| DHCPv6 server selection by preference is untested | no lab server can send OPTION_PREFERENCE, so a two-server link picks by arrival | `tests/ipv6/run-dhcpv6.sh` |
| The on-target source-selection arm cannot separate Rule 6 | one `ADDRESS6` per interface, so no guest holds a ULA and a global; Rule 6 does run off-target in `test_ipv6_srcsel_host.c` | `include/aminetxduo/config.h:191`, `tests/tools/run-srcsel.sh` |
| The DHCP restart's re-arm path has no test | ALREADY_STARTED never arises in the SLIRP arm, so the stop-and-start-again is unexercised | `src/netstack/netstack.c:2695` |
| Nothing guards the usergroup.library hold ixemul clients depend on | no caller in our tree, so a tidy-up would silently close the stack for ixnet | `src/bsdsocket/library_runtime.c:46` |
| HAM and EHB are proven on one machine and one resolution | left: an A600 for HAM6 and EHB on OCS, and anything but 320x256 lores | `tests/tools/run-console.sh`, `src/tools/httpfb.c` |
| PCMCIA parity stops short of the CFTABLE walk | walking every entry would plausibly reach the seven multifunction cards | `src/netdev/netdev_pcmcia.c` |
| The PCMCIA socket is never contended in the lab | `CARDF_IFAVAILABLE`'s rejection path and the later-`OpenDevice()` claim are untested | `src/netdev/netdev_pcmcia.c` |
| Amiberry cannot eject a PCMCIA card, so removal is unexercised | `pc_on_removed`/`pc_on_inserted` have never fired, nor the reset order | `src/netdev/netdev_pcmcia.c:363`, `:386` |
| One card row has never run on its hardware | `xsurf500` is from the iComp wiki; a real 3c589 has run since 2026-08-22, by hand, with no lab arm and no wire payload check | `src/netdev/netdev_cards.c` |
| `AMINETXDUO_AMIBERRY_MAC` is ignored for `ne2000_pcmcia` | Amiberry hands that board the host NIC address, so two bridged runs share one; detected and refused rather than fixed, one at a time per host | `tools/emu-board.sh:57`, `tools/amiberry-run.sh:284` |
| `aamprobe.c` hard-codes `a2065.device` twice | on any other board the re-add is refused with errno 6 and later asserts read poison | `tests/tools/aamprobe.c:669`, `:926` |
| binutils `amiga-2.46` cannot assemble gcc 16.2's own output | it forces a byte displacement on `jne`/`jeq`; we stay pinned at `amiga-2.39.0` | bebbo's `binutils-gdb` |
| No arm reaches the accelerated-Gayle timing ratio | an emulated bus read costs host time, not guest time; needs real hardware | `tests/tools/run-cpuspeed.sh:30` |
| No arm puts two network boards in one machine | one `-N` per run and `emu_board_lines` is called once, so non-zero `UNIT` is untested; `CARD=` itself is exercised by the card sweeps | `tools/amiberry-run.sh:40` |
| Payload content is unverified on a real 3c589 | the physical arm was never staged and Amiberry emulates no EtherLink III | `tests/tools/run-payverify.sh` |
| The xmit leg has never been read on a transfer | built and wired, but no capture run has quoted it beside reap/stuff/post | `tests/perf/run-poolshare.sh:97` |
| No arm varies the application read size | fetch is paid per recv(); a 32 KB read would pay it an eighth as often, never measured | `tests/perf/run-poolshare.sh:97` |
| The docs/*.md budget is 1150 where 800 was asked for | the enforced ceiling still exceeds the requested ceiling by 350 lines | `tools/check-doc-budget.sh:17` |
| No arm prices the 64-bit-divisor branch on a 68000 | rtdiv ran on an A1200 only; that branch now enters a 32-iteration fallback once per divide | `src/common/ami_udivdi3.c:87` |
