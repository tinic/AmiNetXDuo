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
| 2 | No m68k ThreadX or NetX Duo port existed. lwip-amiga is PiStorm-only, AmiTCP_NG and AROSTCP are GPL, so an MIT SANA-II stack was the open niche. | `docs/DEVELOPMENT.md:188` | historical |
| 2.6 | `bsdsocktest`, 142 tests, is the acceptance gate. | `tests/conformance/build.sh:8` | current |
| 3.1 | `SocketBase` is per-opener and never shared: every returned struct lives in the child base, never a file static. | `src/bsdsocket/netdb.c:15` | see code |
| 3.2 | The 121-LVO offset table comes from the Roadshow NDK `bsdsocket_pragmas.h`; four probes call vectors by raw LVO against it. `ipf_*` is out of scope. | `tools/gen_vectors.py:106` | current |
| 3.3 | Pin every Amiga ABI struct with `_Static_assert`: the toolchain's `ndk-include/pwd.h` substitutes newlib's 10-field `passwd` over usergroup's 7-field one. | `include/aminetxduo/mbuf.h:245`, `bpf.h:250` | current |
| 3.4 | SANA-II is cooked: the device owns the link header, so the shim builds no Ethernet header on TX and synthesises one on RX. Raw is a `CMD_READ`/`CMD_WRITE` flag, cannot be probed, ships off. | `src/sana2/sana2_tx.c:500`, `sana2_rx.c:651` | current |
| 5.2 | NetX Duo reaches into `TX_THREAD` internals in ~40 files to build its own suspension lists, so a `tx_mutex_*` to `SignalSemaphore` shim cannot work. Real ThreadX is required. | `port/threadx-amiga/src/tx_amiga_adopt.c:11` | see code |
| 5.3 | Leave `NX_LITTLE_ENDIAN` undefined: m68k is native network order, so every header byte swap compiles away. | `port/netxduo-amiga/inc/nx_port.h:8` | see code |
| 5.4 | `-noixemul` is unusable on this newlib toolchain: it breaks `sys/reent.h`. | `cmake/toolchain-m68k-amigaos.cmake:111` | see code |
| 5.4 | The 2026-07 build-spike byte counts. | git history | historical |
| 5.5 | No such section. Nothing cites it. | | dangling |
| 6.1 | 50 Hz tick. The wakeup source (`UNIT_VBLANK`) is not the time base (`ReadEClock`), and `NX_IP_PERIODIC_RATE` must equal `TX_TIMER_TICKS_PER_SECOND`. | `port/threadx-amiga/inc/tx_port.h:509` | see code |
| 6.2 | Baton model: one core lock, exactly one ThreadX thread runs at a time. `TX_DISABLE` is `Forbid()`/`Permit()`, never `Disable()`/`Enable()`, because nothing runs at Exec interrupt level. | `port/threadx-amiga/inc/tx_port.h:26` | see code |
| 6.3 | Thread adoption shipped, the worker pool never was. Adopt on entry, orphan on exit, and never hold the baton across application code or a non-ThreadX `Wait()`. | `port/threadx-amiga/src/tx_amiga_adopt.c:14` | see code |
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
| 11 | Loopback is the fastest path and the source of every throughput figure, so it needs its own tap via `NX_ENABLE_IP_PACKET_FILTER`. | `port/netxduo-amiga/inc/nx_user.h:505` | see code |
| 11.2 | Kickstart 3.1 ROM has `mathieeesingbas` only, so a doubles-using port must stage the IEEE math libraries or hang silently. | `tools/amissl-run.sh:101` | see code |
| 11.3 | The pinned toolchain's `crt0.o` pushes `&__argv`, so `main()` gets a garbage `argv`. Patch a copy in the build dir, never the toolchain. | `clients/compat/fix-crt0.py`, `tools/fix-toolchain-crt0.py:267` | current |
| 11.5 | A ported client needs `-D__USE_NEW_TIMEVAL__`, `-D_SYS_MBUF_H` and `-include sys/types.h`, and cannot run on a Shell's 4 KB stack. | `clients/amiga-client.sh:97` | current |
| 11.6 | Write a TLS backend over `tls.library` rather than importing mbedTLS or wolfSSL. | | superseded, the 8x gap was really 2.4x |
| 11.7 | A Unix client with no AmigaOS awareness needs one compat TU teaching it that a socket is not an fd. | `clients/dropbear/amiga_dropbear.c:9` | see code; its hand-written-config half is refuted at `clients/dropbear/build.sh:40` |
| 11.8 | A CDN front end abandons a handshake between ~11 and 20 s, so a 14 MHz Amiga fails on deep chains. Local test servers must raise their grace timeout. | `clients/dropbear/sshd-testserver.sh:131` | current |
| 12.3 | `SOCK_RAW`, `MSG_OOB`/`SIOCATMARK` and orderly close were the three remaining gaps. All three are closed. | `src/bsdsocket/raw.c`, `oob.c`, `socket.c:557` | superseded |
| 12.5 | No such section. `src/bsdsocket/socket.c:580` cites it; it means 12.3 item 3. | `src/bsdsocket/socket.c:576` | dangling |
| 13 | Session resumption removes all public-key work: 23.4 s to 0.6 s, constant across chains, and it survives a reboot through the on-disk mirror. | `src/tlslib/tls_resume.c:301` | current |
| 13.2 | Never patch vendored source. Use a symbol override, or `-Wl,--wrap`. | `src/tlslib/CMakeLists.txt:51` | current |
| 13.4 | A server issuing an RFC 5077 ticket returns an empty session ID, so the client must generate 32 random bytes per attempt as the acceptance handle. `openssl s_client` hides this by fabricating one. | `src/tlslib/tls_resume.c:828` | current |
| 15 | Against AmiSSL 5.27 we win the handshake 2.4x and draw on bulk, because neither tree had m68k assembly for it. | `src/crypto68k/c68k_aes.S`, `c68k_chacha20.S`, `c68k_poly1305.S` | superseded, the assembly was then written |
| 15.7 | The largest lever on `https://` is an unwritten 68020 SHA-256. | `src/crypto68k/c68k_sha256.h:5` | superseded, hand-written SHA-256 lost to the C |
| 16 | The wire ceiling is 8.0 ms per 1440-byte segment at 14 MHz: the per-segment CPU cost of the receive pipeline, not the window and not the tick. | `src/sana2/sana2_tx.c:116` | current |
| 16.3 | The emulator's A2065 hex log is an independent host-side capture but carries no timestamps. Take timing from the guest pcap, loss and ordering from this one. | `tests/trace/a2065pcap.py:1` | see code |
| 16.6 | The 50 Hz tick makes the delayed-ACK timer 200 ms. | `port/netxduo-amiga/inc/nx_user.h:34` | current; its two recommended fixes are both superseded at `nx_user.h:189` |
| 16.9 | A Shell command gets a 4 KB stack and a bsdsocket LVO runs NetX Duo on the caller's stack, so too little stack is an F-line trap and a reboot loop that reads as a timeout. Hence static control blocks, 64 KB `NP_StackSize` minimum, and flush every output line. | `src/tools/httpterm.h:51` | current |
| 17 | The raw filter copies a packet per interested descriptor and always declines, so ICMP still reaches `nx_icmp_ping()`. `MSG_OOB` rides a normal send with a one-shot `nx_ip_packet_filter`. | `src/bsdsocket/raw.c:56`, `oob.c:54` | see code |
| 18 | The hand-written 68020 SHA-256 lost to GCC 15.2 C, 67,656 against 66,687 us, and was deleted. Only the misaligned `MOVE.L` survived. | `src/crypto68k/c68k_sha256.h:22` | see code |
| 18.1 | Measured 68020 instruction costs: no data cache, byte read equals longword read, `ROR.L #n` 5.94 cycles against `ROR.L Dm,Dn` 7.91. The shifter is flat, so the SWAP idiom is worthless. This table exists nowhere else. | `clients/dropbear/localoptions.h:46` | current |
| 18.2 | AES-128-CBC costs 233 cycles/byte and HMAC-SHA256 236. Four 1 KB T-tables in assembly beat the one-table layout by ~14%. | `src/crypto68k/c68k_aes.h:129` | current |
| 18.6 | A wire figure through an emulator carries host-contention variance, so an A/B must be two binaries from one commit measured back to back. | `clients/dropbear/run-dbclient.sh:29` | current |
| 19 | An unset clock silently accepts expired certificates. `sntp` sets `timer.device` and `battclock`, and takes the timezone from `locale.library`'s `loc_GMTOffset`. | `src/tools/sntp.c:24` | see code |
| 19.6 | A Shell command links its own ThreadX and NetX Duo whose kernel was never entered, so it can never read the running stack by linking. The fix is published LVOs. | `include/aminetxduo/netstatus.h:1` | see code |
| 20 | `traceroute`, `tftp` and `whois` design notes. | `src/tools/tftp.c`, `whois.c` | historical |
| 20.1 | `IP_TTL` reached the wire on a raw socket but not a UDP one. | `src/bsdsocket/transfer.c:682` | superseded, UDP honours it now |
| 20.2 | FS-UAE's SLIRP zeroes the ICMP sequence on a proxied reply while preserving the identifier, and never generates TIME_EXCEEDED or unreachables. | `src/tools/ping.c:180` | current |
| 21 | The size was ours, not newlib. `-ffunction-sections` on two archives plus `--gc-sections` on the commands only, worth -27% to -38%. | `src/tools/CMakeLists.txt:100` | see code |
| 21.3 | `--gc-sections` must never reach a `.library`: the romtag has no relocation pointing at it, worth -60 KB off `tls.library`. | `src/tools/CMakeLists.txt:110` | see code |
| 21.6 | CMake's `Compiler/GNU` appends its own `-O3 -DNDEBUG` after `CMAKE_C_FLAGS_RELEASE_INIT`, so a toolchain file must name the level it wants. | `cmake/toolchain-m68k-amigaos.cmake:169` | current |
| 22 | `AddNetInterface` deliberately leaks an `OpenLibrary()` reference to keep the network up, so no expunge is reachable after it has run. | `tests/tools/cycledrill.c:14` | current |
| 22.5 | `NX_ENABLE_IP_STATIC_ROUTING` is off, so `NETCTRL_ROUTE_ADD` answers ENOSYS. | `port/netxduo-amiga/inc/nx_user.h:592` | superseded, the enable is on and `AddNetRoute` ships |
| 22.8 | A reverse lookup costs `BSD_RESOLVE_TIMEOUT`, 30 s, per name server, paid before the first packet, so commands read `DEVS:Internet/hosts` first. | `src/bsdsocket/resolver.c:18` | current |
| 24 | The advertised TCP window is derived, never fixed: pool budget over live TCP socket count, clamped, decided at socket-create time. | `src/bsdsocket/bsdsocket_internal.h:290` | see code |
| 24.7 | `NX_ENABLE_LOW_WATERMARK` is undefined, so a socket's receive queue has no packet-count cap and the advertised window is its only bound. | `port/netxduo-amiga/inc/nx_user.h:1095` | see code |
| 24.8 | One packet pool for the whole stack is deliberate: a second pool takes memory from the path at risk to protect the path that is not. | `port/netxduo-amiga/inc/nx_user.h:1086` | see code |
| 24.9 | An `AvailMem` drop of ~291 KB is `tls.library` loading, not a leak. Measurement arms must be built into private `build/` dirs because the instrument moves under the measurement. | `tests/endurance/endreport.py:9` | current |
| 25 | This toolchain mis-resolves a 32-bit PC-relative branch from a cross-section tail call twelve bytes short, into the middle of the preceding function. `-fno-optimize-sibling-calls` always travels with `-ffunction-sections`, and a post-link check enforces it. | `cmake/check-pcrel-branches.cmake:1` | see code |
| 25 | A guest that reboots looks exactly like a hang in a transcript, so every emulator test counts boots. | `tests/tools/run-livetools.sh:192` | see code |
| 27 | Ambiguous: two sections are numbered 27. DHCP option 12 must carry the configured hostname, not the literal `"amiga"`. | `src/netstack/netstack_mdns.c:14` | see code |
| 27 | Ambiguous: two sections are numbered 27. SYN retransmission was flat ~1 s because `NX_TCP_RETRY_SHIFT` defaulted to 0. | `port/netxduo-amiga/inc/nx_user.h:228` | superseded, backoff doubles now |
| 27.4 | NetX Duo only retransmits a packet the driver has released, and async SANA-II `CMD_WRITE` released only on the next send, so a lone unacked segment was never resent. | `src/sana2/sana2_tx.c:129` | superseded, fixed by deferred processing |
| 27.6 | `CloseSocket()` emitted a bare RESET where RFC 793 wants a FIN. | `src/bsdsocket/socket.c:555` | superseded, fixed |
| 29 | Measured against Roadshow 1.15 and AmiTCP_NG 4.1.1. `CMakeLists.txt:311` cites this number but means 33.1. | `tests/tcpdrill/scripts/keepalive.drill` | superseded |
| 29.3 | Two instruments disagreed on the same wire, and the per-call bracket was blamed. | `tests/perf/bracket_test.c:4` | superseded twice, see 39 and 65 |
| 29.4 | Time to a DHCP lease. Three sites cite this number but mean 33.4. | `src/netstack/netstack.c:815` | superseded, bring-up is 2483 ms |
| 29.5 | Roadshow answered ICMP 2.7 ms faster. | `tests/compare/tickprobe.c:6` | superseded by 36.7, we are faster through a common device |
| 30.5 | SLIRP relays outbound multicast but rewrites the source to the host's real address, so RFC 6762 correctly drops the unicast reply. The querier half is unprovable under SLIRP, and that is conformance, not a defect. | `tests/tools/run-mdns.sh:49` | see code |
| 31.5 | The cipher is invisible at login payload sizes, and Dropbear's optimistic kex guess costs a wasted 12 s against modern OpenSSH. Trust the guest's own clock: a host-side timestamp is not a measurement of the guest. | `clients/dropbear/localoptions.h:110` | current |
| 31.6 | Asked whether P-256 beats curve25519 on this part. | `clients/dropbear/localoptions-p256.h:5` | superseded, answered no at 149.62 s |
| 32.10 | Blocking `accept()` could hang forever, and slicing the accept itself is unsafe: a timeout runs `_nx_tcp_connect_cleanup`. | `src/bsdsocket/select.c:368` | superseded in part, measured 2026-08-18/19. The SYN cache makes the old duplicate-SYN+ACK failure unreachable, but the harmless state wait remains. Its one-tick polling cost about two ticks per poll on an adopted task; `bsd_wait_sliced()` now charges the actual ThreadX clock delta, including when the caller disabled its break mask. A1200: three requested 400 ms waits take 1401 ms, and a 2500 ms connect takes 2673-2754 ms rather than 4790-4880 ms (`acceptwait.drill`, `connectcancel.drill`) |
| 33.1 | The keepalive arm is built with a 5 s idle timer so a probe is observable in an emulator run. | `CMakeLists.txt:307` | current, cited as 29 |
| 33.4 | Per-packet IP-ID randomisation costs 5.2% of loopback because `NX_RAND` is a SHA-256 DRBG, so the ID is seeded once at `nx_ip_create()`. | `src/netstack/netstack.c:815` | current, cited as 29.4 |
| 34 | An inetd-style handoff leaves `as_Owner` pointing at a freed SocketBase, so the next NetX Duo callback signals a task read out of freed memory. | `src/bsdsocket/socket.c:1059` | see code |
| 35 | An SSH handshake is 97% public-key arithmetic and the largest row is host-key verify at 46%, not the key exchange. Eight 32-bit limbs took 84 s to 12.28 s. | `src/crypto68k/c68k_25519.c` | current |
| 35.4 | The win is the representation, one `MULU.L` per partial product, not instruction selection. | `src/crypto68k/c68k_25519.S:3` | current; its "no assembly" half is dead, the assembly ships |
| 37 | An exhausted packet pool cannot produce `EAGAIN` on a blocking socket here; it drops frames silently. | `src/bsdsocket/errno.c:190` | see code |
| 37.1 | The six unconditional `NX_NO_PACKET` to `EWOULDBLOCK` mappings are safe only because no vector ever runs on the NetX Duo IP thread. | `src/bsdsocket/select.c:325` | see code |
| 37.2 | Three send/recv sites discarded the NetX Duo status before choosing an errno. | `src/bsdsocket/transfer.c:297` | see code, fixed |
| 37.4 | A failed relisten cleared `as_Incoming` with no way back, so every later `accept()` returned `EINVAL`. | `src/bsdsocket/socket.c:1795` | superseded, spares are parked up front |
| 37.5 | Refused `connect()` to a port with a listen request leaked one `AmiSocket` and one packet each, 1009 B/s. | `src/bsdsocket/socket.c:684` | superseded, fixed by `bsd_tcp_abort` |
| 37.10 | An 11 minute single-connection soak: 224 MB, no corruption, no `EAGAIN`. | `tests/soak/fitz_soak.c:14` | historical |
| 40 | Dropbear's `spawn_command()` is the only place it starts a program, `--wrap`ped onto `SystemTagList()`, so no `fork` exists. | `clients/dropbear/amiga_dropbear.c:1515` | see code; `src/bsdsocket/socket.c:696` cites 40 but means 41.2 |
| 41.4 | Three back-to-back non-blocking `connect()`s wedged the calling task. | `tests/leak/refused_leak_test.c:557` | superseded, the defect was the test's own LVO stubs |
| 42 | An AmigaOS library call clobbers d0/d1/a0/a1, so a hand-written `jsr a6@` stub passing an argument in d1/a0/a1 without declaring it written gets a stale value. Nothing lints this. | `src/tools/toolsock.c:11` | current |
| 42.6 | The 745 ms tick stall is emulated-time non-dispatch, not overrun. The discriminator is the previous wakeup's service cost. | `include/aminetxduo/netstatus.h:488` | current, never reproduced |
| 44.9 | `DEVS:Networks` is not on the search path a bare device name reaches, so the open retries `Networks/<name>`. | `src/common/compat.c:388` | current |
| 45 | The toolchain has three multilibs keyed on canonical `-mcpu`, so `-m68040` and `-m68030` silently link the 68000 C library. Use `-m68020 -mtune=68040`. | `cmake/toolchain-m68k-amigaos.cmake:115` | see code |
| 46 | There is no `.rodata` on m68k-amigaos: literals pool into `.text`, so `--gc-sections` can never collect a dead function's strings. `-flto` is permanently out, there is no hunk backend in libiberty `simple-object`. | `cmake/check-pcrel-branches.cmake:95` | current |
| 47 | NDK 3.2 ships Barthel's `bsdsocket.library` autodoc at `SANA+RoadshowTCP-IP/doc/bsdsocket.doc`, the contract the Roadshow vectors were written from. | `tools/gen_vectors.py:156`, `src/bsdsocket/roadshow.c:8` | see code |
| 52 | SANA-II readers must have a numerically lower ThreadX priority than the IP thread. The shipped inversion cost 6.6%. | `src/thread_priorities.h:34` | see code, an `#error` enforces it |
| 54 | ChaCha20-Poly1305 is both the compatible choice and 1.72x faster than AES-CBC plus HMAC. GCM is compiled in but never offered, 20x slower. | `src/tls/ami_tls_crypto.c:949` | current |
| 54.3 | Little to big endian reversal is `ROL.W #8`/`SWAP`/`ROL.W #8`, ~15.8 cycles, about 8.5 cycles/byte against the cipher's ~120, so the endianness tax is not worth avoiding. | `src/crypto68k/c68k_poly1305.S:166` | current |
| 55 | `SocketBaseTagList()` returns the index of the first unserviced tag and stops, so one unknown code discards every tag after it in the same call. | `src/bsdsocket/errno.c:462` | current |
| 55 | `AddDomainNameServer()` as ENOSYS made Roadshow's `AddNetInterface` return rc 20 after doing everything right. A server must land in both the NetX Duo DNS client and `ns_Config.resolver`. | `src/bsdsocket/roadshow.c:253` | current |
| 55 | `IP_HDRINCL` is translated, not passed through: TOS, TTL, protocol and destination are mapped onto `nxd_ip_raw_packet_send()` and the caller's header stripped. | `src/bsdsocket/raw.c:584` | current |
| 55 | ARP entries must be emitted as `RTF_LLINFO` routes or Roadshow's `arp` prints nothing. | `src/bsdsocket/routing.c:411` | see code |
| 56 | 1,000 ms of a 1,201 ms DHCP lease was NetX Duo's RFC 2131 desync delay; retiming the client's own timer to one tick gives -53%. | `src/netstack/netstack.c:1394` | current |
| 57 | `-Os` tree-wide, -24% size at no throughput cost, and it must be set after `project()` or `Compiler/GNU` appends `-O3`. | `CMakeLists.txt:31` | superseded in part, eight profiled NetX Duo files build at `-O2` via `AMINETXDUO_HOT_O2` |
| 58 | Sixteen ChaCha20 words into 8 data plus 7 address registers and one stack slot, moved with `EXG` at 4.01 cycles against 13.80 for a stack spill. | `src/crypto68k/c68k_chacha20.S` | current |
| 60 | AmigaDOS 3.x has no `2>`, so a foreign binary's fatal error is rc 20 and an empty file. Capturing stderr is impossible. | `tests/conformance/run-winuae.sh:15` | current |
| 62 | `-fanalyzer` silently gave up on 48 of 213 TUs. A clean report from a tool that quit is indistinguishable from coverage. | `tools/analyze.sh:140` | current, uncited |
| 63 | A frame the emulator's host sends to the guest's MAC never returns to that NIC's pcap, so the peer in any bridged test must be a third machine. | `tests/perf/run-fitzbench.sh:50` | see code |
| 63.4 | A crashed WinUAE 6.0.3 on a bridged run is an oversized coalesced receive frame in `gotfunc2()`, not our stack. A dead emulator must report `reason=crash`, not rc 0. | `tools/winuae-run.sh:615` | current |
| 63.5 | Building WinUAE from master on winbuilder: build `winuae_msvc.vcxproj` alone, override `OutDir`, set `WINUAEPUBLICBETA 0` or a modal requester hangs the run under PsExec. | `tools/winuae/a2065-multicast-loopback.patch:28` | current |
| 63.6 | A connection whose peer closed immediately is still acceptable: readiness and `accept()` must admit CLOSE_WAIT, CLOSING, TIMED_WAIT and LAST_ACK. | `src/bsdsocket/socket.c:2099` | current |
| 64 | The 68020 is CPU-bound, not window-bound: 1.78x throughput for a 1.76x clock at a fixed window, flat above an 8 KB knee. Reconfirmed on a bridged rig with a real peer. | `src/bsdsocket/bsdsocket_internal.h:314` | current |
| 64.6 | A window sweep on a zero-latency link can never show a bandwidth-delay-product benefit. The bridged answer is flat: window scaling removes a cap, not a rate. | `tests/perf/run-fitzbench.sh:26` | current, the open question is closed |
| 64.8 | There is no meaningfully faster checksum inner loop on a 68020. | `src/net68k/n68k_checksum.S:6` | superseded by 87, up to 1.40x |
| 66 | The IPv6 gap was structural, not 28 omissions: `tool_sock_resolve()` returned a `ULONG` and rejected `h_length != 4`. Nothing may sit between a local register variable declaration and the `jsr`. | `src/tools/toolsock.c:532` | current |
| 67 | `_nxd_ip_raw_packet_source_send()` dropped `ttl` and hardcoded `address_index` 0 on the IPv6 branch. The ICMPv6 checksum covers the source address, so a wrong one is a silently dropped packet. Set `nx_packet_ip_version` before `_nx_ip_checksum_compute()`. | `src/bsdsocket/raw.c:426` | current |
| 68 | The same function dropped `tos` on the IPv6 branch. RFC 2474 makes the IPv4 TOS octet and the IPv6 traffic class one DS field, so `as_Tos` serves both. Carried as a parameter so an unupdated caller fails to compile. | `src/bsdsocket/in6.c:253` | current |
| 71 | `x-surf-100.device` 1.16 sets an AmiTCP-mbuf buffer contract on `FindPort("AMITCP")`, sampled once per `OpenDevice`, so the port must be down across every SANA-II open. | `src/netstack/netstack_rexx.c:620` | current, shipped in 0.12.2 |
| 75 | Of 2,149 Aminet `comm/` archives none would be fixed by a `miami.library` stub and 12 are made worse, so do not build one. `bsdsocket.library` is the entire probed interface. | `CMakeLists.txt:147` | current |
| 75.7 | `AMITCP` is AmiTCP's ARexx host port, not a flag. 31 archives `ADDRESS AMITCP`, and a `PA_IGNORE` port turns a clean failure into a hang. | `src/netstack/netstack_rexx.c:124` | current, a real ARexx host services it |
| 76.7 | `ethernet_getselectionname()` returns `slirp` for any unmatched name, so a mistyped bridged run passes on NAT. Assert the backend from the emulator log. | `tools/amiberry-run.sh:41` | see code |
| 77 | No such section, it was deleted. `tools/sana2-stage.sh:40` means the driver licence audit: of eight third-party SANA-II drivers, licences permit fetching two. | `tools/sana2-stage.sh:44` | dangling |
| 77.6 | No such section. Cited comments mean the baton defect: `tx_thread_identify()` returns the global baton holder, not the caller, so a second Task skips adoption and enters NetX Duo unbracketed. | `port/netxduo-amiga/inc/nx_port.h:155`, `src/netstack/netstack.c:197` | dangling; also cited as 78 and 79 |
| 78 | A bridged Amiberry guest is a real LAN machine, and a bridged run is strictly stronger than SLIRP. | `tools/amiberry-run.sh` | current |
| 78.9 | No such subsection, means 80.9. Terminal size, resize and termios all come from `CON:`, not from the socket. | `tests/bebbossh/run-bebbossh.sh` | dangling, off by two |
| 78.11 | No such subsection, means 80.11. A piped script stops after the second command, in `bebbosshd`'s shell channel, not our send path. | `tests/bebbossh/run-bebbossh.sh:374` | dangling, off by two |
| 79 | Amiberry drops frames silently at saturation, and our stack was starved rather than backlogged. | `include/aminetxduo/sana2.h:131` | superseded, the read collapse was unfilled TX-offload checksums from VM peers |
| 79.11 | Every pre-bridging SLIRP throughput figure is uncomparable, and any run offering more than ~3,000 f/s was losing frames silently. | `tests/perf/run-fitzbench.sh` | current |
| 79.6 | Means 80.3. A size check is not a check: a shim bug returned the right length and the wrong bytes, so every byte is compared. | `tests/bebboget/check.sh:8` | mis-cited |
| 80 | On identical crypto our `fetch` is ~30% faster than bebboget and the handshake 3.5x. Our `fetch` is 2.6x faster on ChaCha20 than on CBC, which is `src/crypto68k`'s 68020 assembly. | `tests/bebbossh/run-bebbossh.sh:462` | current |
| 81 | The memory floor is 1 MB on both builds: fixed cost 432 to 439 KB, and a 1 MB A2000 runs the shipped tools with a 17-packet pool. | `include/aminetxduo/netstack.h:272`, `README.md:59` | current; the two source comments that said 4 MB now say the measured floor |
| 81.3 | 512 KB refuses cleanly, but `AddNetInterface` blamed the cable. It now has an out-of-memory branch reached before the device probe. | `src/tools/addnetinterface.c:143` | current, covered by `tests/tools/run-oommsg.sh` |
| 81.5 | The last `CloseLibrary()` on `bsdsocket.library` does not return, blamed on our close wrapper. | `tests/concurrent/concurrent_test.c:46` | superseded in cause, it is `a2065.device` 2.16 ignoring `AbortIO()` on a pending `CMD_READ` |
| 87 | Chain `addx.l` off `movem.l` so the end-around carry rides the X flag: 201 to 138 ns/B. Register saves cost 24 to 32% on a 20-byte header, so short runs take a computed jump into unrolled pairs. | `src/net68k/n68k_checksum.S:1` | see code, the file header is the same text |
| 89 | Scheduling was 23% of a 1 MB transfer and the cause was frequency, not slowness. Direct baton handoff plus inlined `TX_DISABLE`/`TX_RESTORE` is -17.5% wall on 68020. | `port/threadx-amiga/inc/tx_port.h:231` | see code |
| 89.6 | The scheduling win does not grow on a 68000: 6.5% share against the 68020's 8.7%. | `port/threadx-amiga/src/tx_amiga_internal.h:156` | current |
