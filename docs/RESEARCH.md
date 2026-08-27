# AmiNetXDuo research index

The engineering record was 22,212 lines across 89 sections. It is now the table
below: one row per section number cited from the tree, so every existing
`docs/RESEARCH.md §N` comment still resolves.

**The tree is the authority.** Where a row says *see `file:line`*, that comment is the
current statement of the finding and the row exists only so the citation lands
somewhere. Where a row says **superseded**, the section's conclusion is wrong today and
the named location is what replaced it.

The full prose is in git history: `git show 2b54025b:docs/RESEARCH.md`.

| § | Conclusion | Authority | Status |
|---|---|---|---|
| 3.1 | `SocketBase` is per-opener and never shared: every returned struct lives in the child base, never a file static. | `src/bsdsocket/netdb.c:15` | see code |
| 3.2 | The 121-LVO offset table comes from the Roadshow NDK `bsdsocket_pragmas.h`; four probes call vectors by raw LVO against it. `ipf_*` is out of scope. | `tools/gen_vectors.py:106` | current |
| 3.3 | Pin every Amiga ABI struct with `_Static_assert`: the toolchain's `ndk-include/pwd.h` substitutes newlib's 10-field `passwd` over usergroup's 7-field one. | `include/aminetxduo/mbuf.h:245`, `bpf.h:250` | current |
| 5.4 | `-noixemul` is unusable on this newlib toolchain: it breaks `sys/reent.h`. | `cmake/toolchain-m68k-amigaos.cmake:111` | see code |
| 5.4 | The 2026-07 build-spike byte counts. | git history | historical |
| 5.5 | No such section. Nothing cites it. | | dangling |
| 6.1 | 50 Hz tick. The wakeup source (`UNIT_VBLANK`) is not the time base (`ReadEClock`), and `NX_IP_PERIODIC_RATE` must equal `TX_TIMER_TICKS_PER_SECOND`. | `port/threadx-amiga/inc/tx_port.h:509` | see code |
| 6.2 | Baton model: one core lock, exactly one ThreadX thread runs at a time. `TX_DISABLE` is `Forbid()`/`Permit()`, never `Disable()`/`Enable()`, because nothing runs at Exec interrupt level. | `port/threadx-amiga/inc/tx_port.h:26` | see code |
| 6.4 | Build on `nx_tcp_socket_*`/`nxd_*` directly, not on `addons/BSD/nxd_bsd.c`: `WaitSelect` needs sockets plus Exec signals, errno is per-opener at caller-chosen width, and `ObtainSocket` crosses library bases. | `src/bsdsocket/bsdsocket_internal.h:6` | current |
| 6.6 | Roadshow's on-disk config layout rather than an invented one, `AMITCP` public port, self-start on first `OpenLibrary`. | `include/aminetxduo/config.h:4` | current; its file list is wrong, see `src/config/config_parse.c:6` |
| 8 | Milestone-1 exit criterion: four adopted tasks contending on a mutex and a timer, clean under Enforcer. | `tests/soak/soak_test.c:4` | historical, all nine milestones shipped |
| 9 | Decisions of 2026-07-24. Two of its shipping defaults are inverted in the tree today: TLS and IPv6 both default ON. | `CMakeLists.txt:57`, `CMakeLists.txt:107` | superseded |
| 9 | Target floor. Now any 68000+, OS 2.04+, 1 MB, with pools computed from `AvailMem()`. | `README.md:59`, `include/aminetxduo/netstack.h:272` | superseded |
| 9 | Roadshow config layout, `AMITCP` port, no `ENV:`-only mode. | `include/aminetxduo/config.h:4` | current |
| 9 | IPv6, usergroup, TLS and `mbuf_*`/`bpf_*` all in scope; `ipf_*` stubbed. | `tools/gen_vectors.py:296` | current |
| 9 | One SHA-256 hash-DRBG entropy pool, shared not duplicated. Credited ~26 bits (8 `AvailMem` + 4 + 2 + 12), enough for nonces and ISNs, not for adversarial TLS keys. The per-source credit table exists nowhere else. | `src/common/ami_random.c`, `include/aminetxduo/tlslib.h:591` | current |
| 9 | Karatsuba rejected at 32 limbs, ~5%, because Montgomery reduction is not Karatsuba-able so only half the work is eligible. | `tests/crypto68k/c68k_amissl_bench.c:692` | see code |
| 9 | TLS budget: ~7 s RSA, ~23 s ECDSA at 14 MHz, ~40 KB Fast RAM per open connection. | `include/aminetxduo/tlslib.h:43` | see code |
| 9 | The then-pinned 15.2.0 toolchain shipped a zero-byte `libgcc.a`; 16.2.0b has normal fallbacks, while `src/common/ami_udivdi3.c` remains AmiNetXDuo's CPU-dispatched implementation. | `src/common/ami_udivdi3.c` | historical origin; current implementation |
| 11.8 | A CDN front end abandons a handshake between ~11 and 20 s, so a 14 MHz Amiga fails on deep chains. Local test servers must raise their grace timeout. | `clients/dropbear/sshd-testserver.sh:131` | current |
| 13.2 | Never patch vendored source. Use a symbol override, or `-Wl,--wrap`. | `src/tlslib/CMakeLists.txt:51` | current |
| 13.4 | A server issuing an RFC 5077 ticket returns an empty session ID, so the client must generate 32 random bytes per attempt as the acceptance handle. `openssl s_client` hides this by fabricating one. | `src/tlslib/tls_resume.c:828` | current |
| 16.6 | The 50 Hz tick makes the delayed-ACK timer 200 ms. | `port/netxduo-amiga/inc/nx_user.h:34` | current; its two recommended fixes are both superseded at `nx_user.h:189` |
| 16.9 | A Shell command gets a 4 KB stack and a bsdsocket LVO runs NetX Duo on the caller's stack, so too little stack is an F-line trap and a reboot loop that reads as a timeout. Hence static control blocks, 64 KB `NP_StackSize` minimum, and flush every output line. | `src/tools/httpterm.h:51` | current |
| 17 | The raw filter copies a packet per interested descriptor and always declines, so ICMP still reaches `nx_icmp_ping()`. `MSG_OOB` rides a normal send with a one-shot `nx_ip_packet_filter`. | `src/bsdsocket/raw.c:56`, `oob.c:54` | see code |
| 18 | The hand-written 68020 SHA-256 lost to GCC 15.2 C, 67,656 against 66,687 us, and was deleted. Only the misaligned `MOVE.L` survived. | `src/crypto68k/c68k_sha256.h:22` | see code |
| 18.1 | Measured 68020 instruction costs: no data cache, byte read equals longword read, `ROR.L #n` 5.94 cycles against `ROR.L Dm,Dn` 7.91. The shifter is flat, so the SWAP idiom is worthless. This table exists nowhere else. | `clients/dropbear/localoptions.h:46` | current |
| 20 | `traceroute`, `tftp` and `whois` design notes. | `src/tools/tftp.c`, `whois.c` | historical |
| 21 | The size was ours, not newlib. `-ffunction-sections` on two archives plus `--gc-sections` on the commands only, worth -27% to -38%. | `src/tools/CMakeLists.txt:100` | see code |
| 21.6 | CMake's `Compiler/GNU` appends its own `-O3 -DNDEBUG` after `CMAKE_C_FLAGS_RELEASE_INIT`, so a toolchain file must name the level it wants. | `cmake/toolchain-m68k-amigaos.cmake:169` | current |
| 24 | The advertised TCP window is derived, never fixed: pool budget over live TCP socket count, clamped, decided at socket-create time. | `src/bsdsocket/bsdsocket_internal.h:290` | see code |
| 24.9 | An `AvailMem` drop of ~291 KB is `tls.library` loading, not a leak. Measurement arms must be built into private `build/` dirs because the instrument moves under the measurement. | `tests/endurance/endreport.py:9` | current |
| 25 | This toolchain mis-resolves a 32-bit PC-relative branch from a cross-section tail call twelve bytes short, into the middle of the preceding function. `-fno-optimize-sibling-calls` always travels with `-ffunction-sections`, and a post-link check enforces it. | `cmake/check-pcrel-branches.cmake:1` | see code |
| 25 | A guest that reboots looks exactly like a hang in a transcript, so every emulator test counts boots. | `tests/tools/run-livetools.sh:192` | see code |
| 27 | Ambiguous: two sections are numbered 27. DHCP option 12 must carry the configured hostname, not the literal `"amiga"`. | `src/netstack/netstack_mdns.c:14` | see code |
| 27 | Ambiguous: two sections are numbered 27. SYN retransmission was flat ~1 s because `NX_TCP_RETRY_SHIFT` defaulted to 0. | `port/netxduo-amiga/inc/nx_user.h:228` | superseded, backoff doubles now |
| 27.4 | NetX Duo only retransmits a packet the driver has released, and async SANA-II `CMD_WRITE` released only on the next send, so a lone unacked segment was never resent. | `src/sana2/sana2_tx.c:129` | superseded, fixed by deferred processing |
| 27.6 | `CloseSocket()` emitted a bare RESET where RFC 793 wants a FIN. | `src/bsdsocket/socket.c:555` | superseded, fixed |
| 33.1 | The keepalive arm is built with a 5 s idle timer so a probe is observable in an emulator run. | `CMakeLists.txt:307` | current, cited as 29 |
| 33.4 | Per-packet IP-ID randomisation costs 5.2% of loopback because `NX_RAND` is a SHA-256 DRBG, so the ID is seeded once at `nx_ip_create()`. | `src/netstack/netstack.c:815` | current, cited as 29.4 |
| 35 | An SSH handshake is 97% public-key arithmetic and the largest row is host-key verify at 46%, not the key exchange. Eight 32-bit limbs took 84 s to 12.28 s. | `src/crypto68k/c68k_25519.c` | current |
| 35.4 | The win is the representation, one `MULU.L` per partial product, not instruction selection. | `src/crypto68k/c68k_25519.S:3` | current; its "no assembly" half is dead, the assembly ships |
| 37.5 | Refused `connect()` to a port with a listen request leaked one `AmiSocket` and one packet each, 1009 B/s. | `src/bsdsocket/socket.c:684` | superseded, fixed by `bsd_tcp_abort` |
| 42.6 | The 745 ms tick stall is emulated-time non-dispatch, not overrun. The discriminator is the previous wakeup's service cost. | `include/aminetxduo/netstatus.h:488` | current, never reproduced |
| 44.9 | `DEVS:Networks` is not on the search path a bare device name reaches, so the open retries `Networks/<name>`. | `src/common/compat.c:388` | current |
| 45 | The toolchain has three multilibs keyed on canonical `-mcpu`, so `-m68040` and `-m68030` silently link the 68000 C library. Use `-m68020 -mtune=68040`. | `cmake/toolchain-m68k-amigaos.cmake:115` | see code |
| 54.3 | Little to big endian reversal is `ROL.W #8`/`SWAP`/`ROL.W #8`, ~15.8 cycles, about 8.5 cycles/byte against the cipher's ~120, so the endianness tax is not worth avoiding. | `src/crypto68k/c68k_poly1305.S:166` | current |
| 55 | `SocketBaseTagList()` returns the index of the first unserviced tag and stops, so one unknown code discards every tag after it in the same call. | `src/bsdsocket/errno.c:462` | current |
| 55 | `AddDomainNameServer()` as ENOSYS made Roadshow's `AddNetInterface` return rc 20 after doing everything right. A server must land in both the NetX Duo DNS client and `ns_Config.resolver`. | `src/bsdsocket/roadshow.c:253` | current |
| 55 | `IP_HDRINCL` is translated, not passed through: TOS, TTL, protocol and destination are mapped onto `nxd_ip_raw_packet_send()` and the caller's header stripped. | `src/bsdsocket/raw.c:584` | current |
| 55 | ARP entries must be emitted as `RTF_LLINFO` routes or Roadshow's `arp` prints nothing. | `src/bsdsocket/routing.c:411` | see code |
| 57 | `-Os` tree-wide, -24% size at no throughput cost, and it must be set after `project()` or `Compiler/GNU` appends `-O3`. | `CMakeLists.txt:31` | superseded in part, eight profiled NetX Duo files build at `-O2` via `AMINETXDUO_HOT_O2` |
| 58 | Sixteen ChaCha20 words into 8 data plus 7 address registers and one stack slot, moved with `EXG` at 4.01 cycles against 13.80 for a stack spill. | `src/crypto68k/c68k_chacha20.S` | current |
| 63 | A frame the emulator's host sends to the guest's MAC never returns to that NIC's pcap, so the peer in any bridged test must be a third machine. | `tests/perf/run-fitzbench.sh:50` | see code |
| 63.4 | A crashed WinUAE 6.0.3 on a bridged run is an oversized coalesced receive frame in `gotfunc2()`, not our stack. A dead emulator must report `reason=crash`, not rc 0. | `tools/winuae-run.sh:615` | current |
| 63.5 | Building WinUAE from master on winbuilder: build `winuae_msvc.vcxproj` alone, override `OutDir`, set `WINUAEPUBLICBETA 0` or a modal requester hangs the run under PsExec. | `tools/winuae/a2065-multicast-loopback.patch:28` | current |
| 75 | Of 2,149 Aminet `comm/` archives none would be fixed by a `miami.library` stub and 12 are made worse, so do not build one. `bsdsocket.library` is the entire probed interface. | `CMakeLists.txt:147` | current |
| 77 | No such section, it was deleted. `tools/sana2-stage.sh:40` means the driver licence audit: of eight third-party SANA-II drivers, licences permit fetching two. | `tools/sana2-stage.sh:44` | dangling |
| 77.6 | No such section. Cited comments mean the baton defect: `tx_thread_identify()` returns the global baton holder, not the caller, so a second Task skips adoption and enters NetX Duo unbracketed. | `port/netxduo-amiga/inc/nx_port.h:155`, `src/netstack/netstack.c:197` | dangling; also cited as 78 and 79 |
| 79.6 | Means 80.3. A size check is not a check: a shim bug returned the right length and the wrong bytes, so every byte is compared. | `tests/bebboget/check.sh:8` | mis-cited |
| 81 | The memory floor is 1 MB on both builds: fixed cost 432 to 439 KB, and a 1 MB A2000 runs the shipped tools with a 17-packet pool. | `include/aminetxduo/netstack.h:272`, `README.md:59` | current; the two source comments that said 4 MB now say the measured floor |
| 81.5 | The last `CloseLibrary()` on `bsdsocket.library` does not return, blamed on our close wrapper. | `tests/concurrent/concurrent_test.c:46` | superseded in cause, it is `a2065.device` 2.16 ignoring `AbortIO()` on a pending `CMD_READ` |
| 87 | Chain `addx.l` off `movem.l` so the end-around carry rides the X flag: 201 to 138 ns/B. Register saves cost 24 to 32% on a 20-byte header, so short runs take a computed jump into unrolled pairs. | `src/net68k/n68k_checksum.S:1` | see code, the file header is the same text |
