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
| Every diagnostic is compiled out of every shipped binary | `AMI_ERROR`/`WARN`/`INFO` need `AMINETXDUO_LOG`, which defaults OFF | `include/aminetxduo/compat.h:105` |
| TARGET: X-Surf-100 reads 412 KB/s where AmiTCP_NG reads 906 | same machine, card and driver, never measured here; the A1200 half is answered | `src/netstack/netstack.c:325` |
| Real X-Surf hardware trails other stacks where emulation says we lead | 699 against 1103 KB/s, worst arm -37%; ACK-clock starvation is the hypothesis | `src/netdev/netdev_cards.c` |
| The `FASTMEM=0` zero-window count is 0 to 44 on the 8 MB arm from one boot to the next | same pool, same binary, same 15 s; nothing says which of the two is the machine | `tests/perf/run-poolshare.sh:141` |
| `anxnet.device` acknowledges 12 ms later than `cnet.device` | p50 35.2 against 23.0 ms; loss, window and cadence ruled out, register cost left | `src/netdev/dp8390.c` |
| IPv6 fusing refuses any extension header and all of ICMPv6 | next-header not TCP/UDP declines outright; the chain is never walked, so ND and fragments still cost the stack a payload pass | `src/net68k/n68k_rx_verify.c:72` |
| Three receive-path changes are right ideas whose implementations crash | RX on the SANA-II reader, re-arm before delivering, stop poking the scheduler | `src/sana2/sana2_rx.c` |
| The 3c589 RX FIFO-hold fix has no wire number | no emulator models a 3c589; it is proven only in the C mock | `src/netdev/test/test_netdev_el3.c:35` |
| The console pacing does not turn a cheaper pass into a sooner one | skipping cut duty to 24% of a 75% cap and latency got worse; see `FB_GRAB_FLOOR` | `src/tools/httpfb.c` |
| `__udivmoddi4`'s 64-bit-divisor branch is 6.6x libgcc's | 852 us against 129 on a 68020; a 64-iteration bit loop where libgcc uses Knuth D. Nothing shipped divides by more than 32 bits | `src/common/ami_udivdi3.c:162` |
| Server-side TLS is 2.8% of the library and unreachable | cutting it changes `NX_SECURE_TLS_SESSION`'s layout, so it needs a second build | `src/tls/CMakeLists.txt` |
| The TCP window ceiling has never been observed binding | it needs a saturated pool; the A3000 32 MB arm of `run-bigmem` was queued and did not report | `tests/tools/run-bigmem.sh:58` |
| Nothing enforces that a wired harness's runner ever FIRES | `run-socket.sh` names a workflow that is dispatch, cron and tags only | `tools/check-harnesses.sh`, `tests/ipv6/run-socket.sh` |
| Two socket files are still outside the host tier | `tcp_handler.c` needs the whole `ACTION_*` vocabulary; `transfer.c` pins 4.4BSD `iovec` | `tests/bsdsocket/CMakeLists.txt` |
| 9 harnesses are still unwired | one needs a segment with no DHCP server, one KS 2.04, three a bridged repair | `tools/check-harnesses.sh` |
| The expunge refusal is proven at the library and the port, never at the joint | nothing tests that a failed stop leaves the started flag set | `src/netstack/netstack.c:1404`, `:1628` |
| The bring-up figure has never been measured on the build that ships | it reads `netstack: mark` lines only an `AMINETXDUO_LOG=ON` build emits | `tests/ipv6/run-bringup.sh` |
| No harness boots Kickstart 2.x with a Zorro network board | the romtag under 2.x and `card.resource` V37 are both unproven | `tests/tools/run-addifleak.sh`, `tests/perf/run-fitzbench.sh` |
| No console arm exercises a screen arriving mid-session | every arm connects to a screen already in front and settled | `tests/tools/run-console.sh`, `tests/tools/run-bootconsole.sh` |
| No resolution the lab tests is unaligned | every arm is a multiple of 16 with nothing to pad, which is how 1600 columns shipped | `tests/tools/run-console.sh`, `src/tools/httprtg.c:560` |
| `nc -l` from outside has never been exercised under Amiberry | it forwards with `uae_slirp_redir`, an FS-UAE option; a bridged guest needs none | `tests/tools/run-nettools.sh` |
| DHCPv6 server selection by preference is untested | no lab server can send OPTION_PREFERENCE, so a two-server link picks by arrival | `tests/ipv6/run-dhcpv6.sh` |
| The on-target source-selection arm cannot separate Rule 6 | one `ADDRESS6` per interface, so no guest holds a ULA and a global | `src/config/config_parse.c:769`, `tests/tools/run-srcsel.sh` |
| The DHCP restart's re-arm path has no test | ALREADY_STARTED never arises in the SLIRP arm, so the stop-and-start-again is unexercised | `src/netstack/netstack.c:2686` |
| Nothing guards the usergroup.library hold ixemul clients depend on | no caller in our tree, so a tidy-up would silently close the stack for ixnet | `src/bsdsocket/library_runtime.c:46` |
| HAM and EHB are proven on one machine and one resolution | left: an A600 for HAM6 and EHB on OCS, and anything but 320x256 lores | `tests/tools/run-console.sh`, `src/tools/httpfb.c` |
| PCMCIA parity stops short of the CFTABLE walk | walking every entry would plausibly reach the seven multifunction cards | `src/netdev/netdev_pcmcia.c` |
| The PCMCIA socket is never contended in the lab | `CARDF_IFAVAILABLE`'s rejection path and the later-`OpenDevice()` claim are untested | `src/netdev/netdev_pcmcia.c` |
| Amiberry cannot eject a PCMCIA card, so removal is unexercised | `pc_on_removed`/`pc_on_inserted` have never fired, nor the reset order | `src/netdev/netdev_pcmcia.c:363`, `:386` |
| Two card rows have never run on their hardware | `xsurf500` is from the iComp wiki and `3c589` from the 3Com manual | `src/netdev/netdev_cards.c` |
| `AMINETXDUO_AMIBERRY_MAC` is ignored for `ne2000_pcmcia` | Amiberry uses the host NIC address, so the second consecutive run has a dead RX | `tools/emu-board.sh`, `tests/tools/cards.sh` |
| `aamprobe.c` hard-codes `a2065.device` twice | on any other board the re-add is refused with errno 6 and later asserts read poison | `tests/tools/aamprobe.c:669`, `:926` |
| binutils `amiga-2.46` cannot assemble gcc 16.2's own output | it forces a byte displacement on `jne`/`jeq`; we stay pinned at `amiga-2.39.0` | bebbo's `binutils-gdb` |
| The console records `.pfs` and nothing else can read it | an MP4 export needs a vendored muxer under the CSP and a lossless codec | `src/tools/web/client/console/pfs.ts` |
| No arm reaches the accelerated-Gayle timing ratio | an emulated bus read costs host time, not guest time; needs real hardware | `tests/tools/run-cpuspeed.sh:30` |
| No arm puts two network boards in one machine | Amiberry holds one board per family in a static; unit numbering and `CARD=` are untested | `tests/tools/cards.sh` |
| Payload content is unverified on a real 3c589 | the physical arm was never staged and Amiberry emulates no EtherLink III | `tests/tools/run-payverify.sh` |
| The xmit leg has never been read on a transfer | built and wired, but no capture run has quoted it beside reap/stuff/post | `tests/perf/run-poolshare.sh:97` |
| No arm varies the application read size | fetch is paid per recv(); a 32 KB read would pay it an eighth as often, never measured | `tests/perf/run-poolshare.sh:97` |
| The docs/*.md budget is 1150 where 800 was asked for | the enforced ceiling still exceeds the requested ceiling by 350 lines | `tools/check-doc-budget.sh:17` |
| No arm prices the 64-bit-divisor branch on a 68000 | rtdiv ran on an A1200 only; that branch now enters a 32-iteration fallback once per divide | `src/common/ami_udivdi3.c:87` |
| A poolshare KB/s row cannot say the rig was quiet | the interlock is per-board, so CI's bridged guests share ens18 and the cores | `tools/amiberry-run.sh:270` |
| Three tool headers stay over 20 comment lines | each header IS the `--help` text, printed by a hardcoded `sed -n` range, so trimming it needs a code edit | `tools/fetch-toolchain.sh:164` |
