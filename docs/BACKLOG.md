# Backlog

What is outstanding, what was decided against, and why. Findings come mostly
from the NDK 3.2 autodoc audit (four passes over all 122 documented entries)
and from the memory-floor and stress work.

The autodoc is at `NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc`. **`grep`
silently fails on it**, `file` misidentifies it as "GTA in-game text". Read it
with python.

**The NDK headers have the same trap.** `m68k-amigaos/ndk-include` is Latin-1
and carries a `©`, so a plain `grep -r` reads those files as binary and finds
nothing, an empty result there means "not read", not "not present". Use
`LC_ALL=C grep -a`. A whole RFC 3542 assessment was once written on one of those
empty results.

---

## Open, no decision taken

Everything below survived the 2026-08-04 sweep. What that sweep fixed is in the
git log; what it declined is under *Decided against*; what it disproved is under
*Withdrawn*. Conformance status and citations are in `CONFORMANCE.md`.

### Recommended, not scheduled

| Item | Why it is here rather than under *Decided against* | Cite |
|---|---|---|
| **A TLS 1.3 handshake dies inside certificate verification** | The client sends its ClientHello, takes the whole server flight, and never sends its Finished. **Traced on the wire** with a logging proxy in the path (`tcpdump` needs sudo; the peer is on 127.0.0.1 for a slirp run, so a relay sees the same bytes): `client->server: handshake(206) msg1` and nothing else, `server->client: msg2 CCS appdata(23) appdata(939) appdata(96) appdata(53)`, then our own FIN 6 s later, which is `fetch` cleaning up after the failure rather than the cause. Sampling the socket state per handshake message gives `msg.types 08 0b 0f 14` against `msg.states 5 1 1 1`: **ESTABLISHED while EncryptedExtensions is processed, CLOSED from Certificate onward**, with no server segment in between -- so it is closed locally, not by the peer. `NOVERIFY` completes the handshake and sends the request, which puts it in verification. **Ruled out by measurement**: slirp (fails bridged), the packet pool (220 free, 0 empty requests), thread priorities (SANA-II 1 > IP 2 > callers 16), CPU speed, connection order, crypto68k's limb substitutions (A/B with `AMINETXDUO_C68K_LIMBS=OFF` fails identically), the trust-store disk read (already bracketed by `nxc_BatonRelease`/`Acquire`, `tls_store.c:407`), and the record buffer (10240, above nx_secure's 4000 minimum). Intermittent: a connection sometimes completes. Next: find what writes `nx_tcp_socket_state` during `_nx_secure_tls_process_remote_certificate` | `nx_secure_tls_1_3_client_handshake.c`, `tests/tls/run-tls13.sh` |
| **TLS 1.3 client** | On branch `tls13`, and it is a compile switch: nx_secure's seven `nx_secure_tls_1_3_*.c`, `_nx_crypto_rsa_pss_verify`, `crypto_method_hkdf` and the `NX_SECURE_TLS_TLS_1_3_ENABLED` rows in `ami_tls_crypto.c` were all present and only compiled out. Fixed on the way: `tls_library` links nx_secure by `$<TARGET_FILE:...>`, which carries no usage requirements, so `tls_conn.c` compiled a different `NX_SECURE_TLS_SESSION` than nx_secure operated on. Still failing: `tests/tls/run-tls13.sh` is red at `nx_secure_tls_process_record.c:558`, a content type that is not 20/21/22/23, so the key schedule derives wrong keys. The hash method is implicated -- our rows carry `AMI_BULK_SHA256` where stock carries `crypto_method_sha256`, and swapping them moves the failure to `NX_CRYPTO_PTR_ERROR`, so they are not interchangeable. Server-side 1.3 is impossible regardless: nx_crypto has PSS verify and no PSS sign | RFC 8446 |
| **Encrypt-then-MAC, RFC 7366** | The answer to the CBC padding-timing row, which is declined on its own terms below. EtM fails the MAC before padding is examined, at no per-record cost; constant-time padding costs ~10 ms per record at 7 MHz |, |
| **Extended master secret, RFC 7627** | Without it, resumption is exposed to the triple-handshake attack. A change to nx_secure's key schedule, not a table entry; every cached session becomes non-resumable |, |
| **SACK send side** | Written on fork branch `amiga-tcp-sack-transmit` (`d8af79c5`), never landed. Adds `nx_tcp_sack_option_get.c` and the retransmit skip. **Needs a transmit-side drill case before it can land**: merged onto the pinned commit 2026-08-05 it builds clean (+652 bytes) and breaks nothing, 59 cases and 568 checks green across `tcp`, `sack`, `dsack`, `retransmit`, `dupack`, `rto` and `rwndupdate`, but the same drills give byte-identical results with the change reverted, so none of them reaches the new code. All twelve `sack.drill` cases are receive side, asserting the blocks we emit for our own holes; nothing asserts that a peer's blocks make a retransmission skip a segment. Two conflicts on merge, both additive against the D-SACK members added since | `nx_tcp.h:93`, `tests/tcpdrill/scripts/sack.drill` |
| **RFC 2308 §5 negative cache.** A name that does not exist is looked up again on every call | The SOA MINIMUM has to be held against a name with no record to attach it to, which means a synthetic entry type and storage for a name with no data. `NX_DNS_NAME_ERROR` now exists and reaches `ami_ns_dns_error()`, so nothing outside this row is in the way | `nxd_dns.c:3587`, `netstack_dns_status.c` |
| **Bailiwick check on cached records, with CNAME chain following** | One item, not two: `NX_DNS_ENABLE_EXTENDED_RR_TYPES` is undefined, so the A record after a CNAME has the CNAME target as its owner and is accepted only because no owner-name check exists. Adding the check alone fails every CNAME-hosted name | `nxd_dns.c` |
| **`src/bsdsocket` has one host test**, `test_inet`, reaching 7 of 29 files | The other 22 are blocked on 253 Amiga constants and on structures the host cannot shape, and their ABI is already held by 80 `_Static_assert`s on the cross build. What is left un-held is behaviour needing the real ABI: `bsd_route_mtu()`, `bsd_udp_from_peer()`, the 4-tuple filter. That is guest-suite work, so this row is about `bsdsocktest` coverage, not host coverage | `src/bsdsocket/`, measured under *Host-testing* below |
| **`src/tools` is 32,412 lines behind five host tests.** `httppath`, `httpif`, `httplock`, `fetchurl`, `httpframe`, all of them httpd's | The other twenty-five commands have none. The 2026-08-04 diagnostics rewrite changed every one of them and nothing on the host could have caught a mistake; the cross build was the only gate, and it only proves they compile | `src/tools/` |
| **`src/tlslib` is 4,628 lines behind three.** No test of the handshake, of the record layer beyond the fuzzers, or of resumption past the expiry rule | `tls_conn.c`'s alert handling is still exercised only by `run-https.sh` on hardware. `tls_resume.c`'s expiry is now `tls_expiry.c` and host-tested; what remains there is the ticket and session-ID handling, which needs a server to answer | `src/tlslib/` |
| **`src/sana2` has one test, added 2026-08-04**, covering `sana2_copy.c` alone out of 3,704 lines | The driver-facing code runs at interrupt time, which is where a mistake takes the machine down rather than failing a check | `src/sana2/` |
| **A command is mostly C runtime.** `ping` is 16,196 bytes in 0.17.3, of which its own code is about 2,050 | libnix's crt0 chain pulls in stdio and the C++ AVL allocator through `__stdiowin.o` and `__initcpp.o`; `atexit()` was the other route and is gone. `tool_printf` goes through dos.library `VPrintf` and `ami_alloc` through `AllocVec`, so nothing we wrote calls what remains. `src/tools/CMakeLists.txt:62-76` records why the link line was left alone once before | link map of `tool_ping` |
| **Parameterise `run-tcphandler.sh`'s peer address** | Seven connections in it name 10.0.2.2 outright. It runs under Amiberry now, whose SLIRP puts the gateway at the same address, so it passes; the address being written down seven times is what stops it moving to a bridged backend or a real peer | `tests/tools/run-tcphandler.sh` |
| **`HOST_TEST_TARGETS` count guard does not catch an unbuilt target** | It compares registered tests against targets, and a test registers whether or not its target was built, so five went to `main` reporting Not Run | `tools/ci.sh:207-211` |
| **EKU, nameConstraints and critical-extension rejection, together** | Accepting a certificate that marks nameConstraints critical while not enforcing it is exactly the failure the critical bit exists to prevent, so doing one without the others is worse than doing none. The known-critical set must be `{basicConstraints, keyUsage, subjectAltName, extendedKeyUsage}`, Let's Encrypt intermediates mark EKU critical. Untestable without hardware | `nx_secure_x509_extension_find.c:191` |

### The release gate, and what is left of the emulators

| Item | State | Cite |
|---|---|---|
| **Register a self-hosted runner** | The one thing between the release end-to-end test and it running by itself. `emulator.yml` runs `install/test/run-workbench.sh` and `tests/tools/run-addifup.sh` on tag pushes already, and skips both while `AMINETXDUO_KICKSTART_RUNNER` is unset, which is every tag so far. Minting a registration token needs admin scope the working PAT does not have | `.github/workflows/emulator.yml` |
| **`tools/enforcer-run.sh` is the last fs-uae caller** | Enforcer needs a real MMU, so this builds its own 68030 configuration rather than calling a runner. Every other harness moved on 2026-08-04; this one is a port, not a substitution | `tools/enforcer-run.sh:96` |

Both are known and neither is in the way of a release: the gate runs by hand
today, and `tools/ci.sh` no longer asks for fs-uae at all.

### The read path, measured 2026-08-05

| Item | State | Cite |
|---|---|---|
| **Receive-side direct dispatch — measured and rejected, do not land** | SANA-II readers processing packets inline under the IP protection mutex, ring refilled before processing. Collapses data-to-ACK turnaround from 8-16 ms median to 1.6 ms, throughput-neutral in steady state at 14 and 28 MHz, and it DOUBLES the convergence transient it was meant to cure: 142 and 128 spurious retransmissions against the deferred path's 70 and 93, two eight-rep runs per arm, with the transient persisting to rep 6 instead of rep 4 and mean read down 1508 -> 872/1192. The peer's retransmission timer tracks SRTT + 4x its variance: the deferred path's batching pads the median to ~12 ms so the guest's 20-80 ms burst spikes sit under the timer, and the 1.6 ms median pulls the timer tight enough that every spike fires a probe. Lower median, same spikes, worse outcome. The latency is load-bearing. A 68030+ gate was proposed and measured too: at 26 MHz the two arms tie, 50 against 55 spurious retransmissions and means within rep noise, so there is no speed regime where inline processing wins and nothing for a CPU gate to gate in. The transient itself shrinks with CPU speed unaided | 2026-08-05 captures, `tests/trace/tcpaudit.py` |
| **First-bulk-burst convergence transient** | A peer whose RTO was calibrated by the fast handshake retransmits spuriously against this receiver's burst ACK latency until its estimator adapts, about four 512 KB reps on one persistent connection. All 70 retransmissions in the measured run were answered with RFC 2883 D-SACKs, one for one; the guest driver received more frames than the port-filtered wire capture carried, so nothing was lost anywhere. Self-healing and lossless; both candidate mitigations are measured dead ends, ring depth 4 -> 16 and the direct dispatch above. What would move it is smaller burst-latency VARIANCE, not a smaller median | `tests/trace/tcpaudit.py`, fitz capture 2026-08-05 |

### Host-testing `src/bsdsocket` and `src/tools`, a measured plan

Spiked 2026-08-04. `bsdsocket_internal.h` reaches `tx_api.h`, `nx_api.h` and
thirteen Amiga headers. ThreadX and NetX Duo both ship Linux ports that satisfy
the first two; the thirteen are stubs, and once they exist the compile stops on
eight incomplete types, all public and stable: `Library`, `List`, `MinList`,
`MinNode`, `Node`, `SignalSemaphore`, `timerequest`, `Task`. `in_pktinfo` and
`in6_pktinfo` collide with the host's `netinet/in.h` and need excluding.

The spike put the cost at one shim of a few hundred lines, after which any file
in `src/bsdsocket` would compile natively. The first half held and the second
did not; the measurement below is what the shim actually reaches.
`tests/sana2/host/shim` (179 lines) and `src/config/test/shim` are the
precedents, and their own comments say the difference between them is
deliberate, so this is a third rather than a merge.

**The Amiga `struct timeval` was the blocking dependency and is cleared.**
`{ULONG tv_secs; ULONG tv_micro;}` against POSIX's `{time_t tv_sec;
suseconds_t tv_usec;}`: same tag, different members. Fixed in `e25e274` by
renaming the tag in `tests/bsdsocket/host/shim/host_prelude.h` after the libc
headers, so libc keeps `struct timeval` for what it declared and the tree sees
`struct ami_timeval`. Sound because nothing in `src/bsdsocket` hands a timeval
to libc; the only calls taking one are timer.device's.

**Measured on `e25e274`, with the generated and `port/threadx-amiga/inc`
include paths.** Seven of twenty-nine compile: `bpf`, `bsdsocket_vectors`,
`inet`, `library_runtime`, `netstatus`, `netx_call`, `nxcontext`. What remains:

| Class | Extent | What it takes |
|---|---|---|
| Missing Amiga/Roadshow constants | 253 distinct identifiers: `SBTC_*`, `IFQ_*`/`IFC_*`/`IFA_*`, `ACTION_*`/`ERROR_*`, `RTA_*`/`RTM_*`, `AAM*`/`CAAM*`, `FD_*`/`FDCB_*` | Restate them in the shim, where they would agree only with themselves |
| The host's structures are not the Amiga's | 20 `_Static_assert` failures; `cmsg.c` is the only file whose errors are all of this class | Amiga-shaped `sockaddr_in`, `msghdr`, `cmsghdr`, `iovec` in the shim, plus `-m32` for the 4-byte pointer |

**The ABI these files carry is already pinned, and better than a host test
would pin it.** 80 `_Static_assert`s across `cmsg.c:66`, `in6.c:51`,
`transfer.c`, `netstatus.c` and `netmonitor.c` fix struct sizes, member offsets
and member widths, `AF_INET6 == 23`, and the `CMSG_ALIGN`/`CMSG_LEN`/
`CMSG_SPACE` arithmetic. They are evaluated on every cross build against the
real NDK headers. Compiling the same files on the host would evaluate them
against the shim, so the assertion would hold by construction and prove
nothing. This is why a test census showed these files as uncovered: the
coverage is a compile-time property, not a test.

**Using the NDK's own headers instead of the shim was tried three times and is
worse**: `-I`, `-idirafter` and `#include_next`, each failing on the
`struct timeval` member `lhm_Date` in `libraries/bsdsocket.h`. Do not repeat
it.

| Phase | What | State |
|---|---|---|
| 0 | The shim, under `tests/bsdsocket/host/shim` | Done. 14 header stubs plus `host_prelude.h` |
| 1 | `inet.c` | Done. `tests/bsdsocket/host/test_inet_host.c`, 49 checks against the BSD manual page: short forms, octal and hex radix, `inet_aton` against `inet_addr` on broadcast, `inet_pton` strictness |
| 2 | `cmsg.c`, `errno.c`, `routing.c`, `addrinfo.c` | Not worth the shim's cost. `cmsg.c`'s only host errors are its own ABI assertions; the other three need a share of the 253 constants |
| 3 | Extract httpd's `If` header and lock evaluation as `httppath.c`, `httpif.c` and `httpframe.c` were extracted | Done. `If` was already `httpif.c`; the lock table is now `httplock.c` and `src/tools/test/test_httplock.c`, 88 checks over RFC 4918 §6.6, §7.1, §9.6, §9.6.1 and §9.10 |
| 4 | `tls_resume.c` expiry, `tls_conn.c` alert-versus-FIN | Expiry done: `tls_expiry.c` and `src/tlslib/test/test_tls_expiry.c`, 34 checks. `tls_conn.c`'s alert-versus-FIN is open and needs a record-layer harness rather than a split, so it is not the same shape of work |

What is left of phase 4 is `tls_conn.c`'s alert-versus-FIN, which unlike
everything above cannot be reached by splitting a file: it is a question about
a record arriving on a socket, so it needs a harness that can feed one. Phase 2
belongs to the guest suites:
`bsdsocktest`
and `tests/sockopt` reach the real ABI, which is the property those files turn
on. `tests/sockopt/host/test_optnum_host.c:14` is the pattern where a host test
does pay for itself against Amiga numbering, by parsing the tree's headers
rather than including them.

Not started: the twenty-five commands other than httpd. Their `ReadArgs`
templates are the testable part and are worth a pass of their own.

### Performance, measured positions

**The 50 Hz ThreadX tick costs 9% of an A600 and 1% of an A1200.** Measured
2026-08-07, `tests/perf/run-stackprof.sh -c` (RAM: arm only, stack up, no
traffic), 512 KB, 3 reps, against `-s none` on the same rig:

| model | arm | baseline | ours | cost |
|---|---|---|---|---|
| A1200/68020 | read | 5904 | 5670 | -4.0% |
| A1200/68020 | write | 3455 | 3199 | -7.4% |
| A600/68000 | read | 1002 | 820 | **-18.2%** |
| A600/68000 | write | 623 | 502 | **-19.4%** |

`tasks (busy)` on A600 is `ThreadX tick 9%` against 1% on A1200, and
`timer.device/LVO-36` 5.5% against a 2.4% baseline. The tick's work is a fixed
instruction count, so a 7 MHz 68000 with no cache pays roughly ten times the
share an 020 does. `-DTX_TIMER_TICKS_PER_SECOND=5UL` confirms causation: A600
read 820 -> 891, +8.7%, 39% of the deficit, ranges +/-0.5%.

The same experiment on A1200 moves nothing (5670 -> 5703, inside the ~1%
between-run spread), which is why measuring only on an 020 hides this.

Not a fix: a fixed lower rate costs timer granularity everywhere, and
`NX_IP_PERIODIC_RATE` follows `TX_TIMER_TICKS_PER_SECOND` into NetX Duo's TCP
timing (`third_party/netxduo/common/inc/nx_api.h:153-158`). Tickless is the
fix -- arm `timer.device` for the next expiry rather than a fixed interval. The
catch-up arithmetic it needs already exists,
`port/threadx-amiga/src/tx_initialize_low_level.c:436`.

Residual after the tick: A600 read is still -11.1% at 5 Hz, unexplained.
Excluded on A1200 by measurement: Chip RAM (`MEMF_FAST` first changed nothing,
commit 3edcd6b), allocator fragmentation (AmiTCP_NG spends 19.6% in exec's
allocator against our 15.6% and still wins the read), and total residency cost
(both stacks run the same 1.28 s against a 1.20 s bare machine).

AmiTCP_NG cannot be compared on A600: the 4.1.5-beta library builds 68020-only,
365 68020 opcodes against 1 in our 68000 build.


**Where transfer time goes.** Sampling profiler, `tools/profiler/`, 1 MB TCP,
A1200/68020, 1000 Hz, 4411 samples, 0.2% unattributed:

| category | wire | loopback |
|---|---|---|
| NetX Duo protocol | 25.6% | 16.7% |
| ThreadX + Amiga port | 23.3% | 16.2% |
| Kickstart (Exec) | 19.2% | 19.4% |
| copy (net68k asm) | 19.0% | 34.5% |
| checksum | 12.3% | 12.6% |

Supersedes the earlier "78% is inside NetX Duo protocol processing", which was
derived by subtracting measured primitives from a measured transfer.
**ThreadX + Exec is 42.5%, larger than NetX Duo's 25.6%.** Largest non-copy cost
is scheduling glue: `_tx_thread_interrupt_restore`/`_disable` 6.9%, Exec
`Reschedule`/`Switch`/`Dispatch`/`Supervisor` 10.5%. Top entries (wire):
`n68k_copy_bytes` 18.2%, `n68k_sum_longwords` 9.5%, `_tx_thread_interrupt_restore`
4.2%, `Supervisor` 3.4%, `Reschedule` 2.8%; top 24 = 73.0%.

**The 42.5% is a tight single-socket loop, not an application.** Same categories
against a real `fitz` run, Exec idle excluded:

| | NetX Duo | copy+checksum | Exec | ThreadX | ThreadX+Exec |
|---|---|---|---|---|---|
| bracket test, wire | 25.6% | 31.3% | 19.2% | 23.3% | **42.5%** |
| fitz, whole run | 33.2% | 32.3% | 20.7% | 13.8% | **34.5%** |
| fitz, read arm | 33.2% | 34.8% | 20.3% | 11.7% | **32.0%** |
| fitz, write arm | 33.1% | 30.0% | 21.0% | 15.8% | **36.8%** |

ThreadX's own share falls 23.3% → 13.8% because a real client spends much of a
read waiting, amortising the per-call bracket. Quote 42.5% only for a tight loop.

**Profiler implementation constraints.** A CIA timer cannot be used and fails
silently: `AddICRVector()` arbitrates the vector, not the hardware. CIA-B timer B
ran correctly at 1000 Hz until the first `ami_millis()` took it back via
timer.device MICROHZ; CIA-B timer A stopped with `ciaicr=$85`, an interrupt
raised and never acknowledged (Exec EXTER race). Source is now audio channel 3 at
level 4, no latching chip in the acknowledge path, and level 4 sees inside the
level 2/3 handlers where a SANA-II receive runs. `prof_start()` measures each
candidate over eight windows and rejects any that does not hold rate.
Attribution: Exec keeps INLINE code in some jump-table slots rather than a `JMP`
(`Forbid`/`Permit` among them), 7.2% was lost until a PC inside
`[base-negsize, base)` was attributed to the slot. `Disable()` masks INTENA so
those sections are unsampled; `Forbid()` is sampled normally, which is where the
bracket lives. Containment verified 97/98/96% on 68020, 100% on 68030. Not yet
run on 68000, fs-uae aborts host-side on the A500 profiles before boot.

**Stack comparison, fixed rig.** `tests/perf/run-stackprof.sh`, bridged Amiberry
A3000, KS 3.1 40.68, one `a2065.device`, released `fitz`,
`FitzBench KB=4096 CHUNK=32768 REPS=3`. Both stacks took the same DHCP lease,
an earlier comparison ran its arms at different addresses.

| | AmiNetXDuo read | Roadshow read | AmiNetXDuo write | Roadshow write |
|---|---|---|---|---|
| throughput | 980, 982 KB/s | 1259, 1909 KB/s | 1929, 2058 KB/s | 1254, 1301 KB/s |
| Exec idle loop | 65.1, 65.3% | 23.9, 50.7% | 19.0, 20.2% | 10.9, 12.7% |
| CPU per MB | 363, 366 ms | 409, 417 ms | 399, 432 ms | 704, 716 ms |

We read at half the rate while spending less CPU per byte, so the read gap was
never CPU work. Cause identified 2026-08-02: duplicate-ACK suppression disabled
fast retransmit (fixed, `4b41379a`), and the lab rig's own 0.35% reordering was
rewarding that defect. The remaining ~15% is wake latency, 9.2 ms from response
header on the wire to first ACK against Roadshow's 7.0, machine idle throughout.

**bifat's four-stack benchmark**, real hardware, `timecmd copy` each way. We are
first or second on send everywhere and third or fourth on receive everywhere;
AmiTCP 4.6 is the exact mirror, so both sit at opposite ends of one trade rather
than one being slow. Receive, ours vs AmiTCP 4.6: A3000/060 + X-Surf-100
699/1103 · A500/1230-50 + X-Surf-500 389/569 · A1200/060 + CNet16 400/454 ·
A500 68000/14 MHz + X-Surf-500 115/175. Send, ours: 1044, 346, 580, 142. The
deficit widens with link speed (worst -37% on X-Surf-100 behind an 060, smallest
on CNet16 where the card dominates), which indicates a per-packet cost.

**net68k primitives, 68000**, WinUAE 6.0.3, cycle-exact A500 profile. The
byte-loop fallback costs **6.1x, not the 4x the source claims**: 5.4 µs/B against
0.89 for the `movem.l` path. `n68k_copy.S` takes it whenever `to` and `from`
disagree in bit 0, because a misaligned word access on a 68000 is an address
error rather than a slow path. It should never fire on receive, the alignment
census gives 0 mod 4 (application buffers, packet prepend pointers) and 2 mod 4
(eight of nine real drivers), both matching the destination's parity. Open
question: does any real driver hand over an odd buffer.

**Foreign stacks do not fold the checksum into the copy**, measured with
`tests/tapprobe/`. Roadshow reads the source exactly once (scribbling `0xEE` over
it the instant the hook returned left every reply intact at all four alignments)
and its hook costs 133 ns/B at best alignment against 158 for a plain `movem.l`
copy of the same data, no budget for per-longword arithmetic. AmiTCP_NG settled
from its GPL source.

**PARKED: melded copy-and-checksum.** Branches `meld` and `wiring` (`bfa9937`);
nothing on `main`, default build unaffected. `n68k_copy_sum_longwords()` measured
256.00 ns/B against 378.56 for the separate pair, 32.4% on a 68020. Both halves
have since improved and the melded routine has not (copy 159, checksum 149.8), so
the margin is now 17%. Wired into receive behind `AMINETXDUO_RX_COPY_SUM`
(default OFF) the receive pair went 389.33 → 312.92, about 20%; the stamp write,
prefix subtraction and acceptance checks eat a third of the primitive's gain.
**Parked because the device buffer is misaligned on 8 of 9 real drivers**,
ariadne, ariadne_ii, x-surf, x-surf-100 (Z2 and Z3), hydra, a2065 and cnet all
hand `S2_CopyToBuff` a pointer at 2 mod 4 and the fast path declines. Under a
400 pkt/s flood the a2065 gave 1266 consecutive misses and zero stamps. Only
`eb920.device` is aligned and could not complete a bulk transfer. Reviving this
means rewriting the melded loop around `movem.l` and the chained `addx` first.

### Harness

| Item | Status |
|---|---|
| `tcpdrill`'s device starts offline; Roadshow stops after `S2_ADDMULTICASTADDRESS`, arms `S2_ONEVENT` and waits, so its interface never comes up. Invisible to us because we send `S2_ONLINE` ourselves | Open. Blocks pointing tcpdrill at a foreign stack |
| `run-tcpdrill.sh` does not pass `-x` to `fsuae-run.sh` although the drills carry timing assertions. `AMINETXDUO_PERF=1` forces it | Open |
| The fs-uae lock is per-checkout (`$ROOT/build/.fsuae.lock`), so `-x` does not serialise against another checkout on the same host | Open |
| `tcpdrill` queued IPv6 ND frames as results | Fixed 2026-08-02 |
| `tcpdrill`'s `idle` counted 20 ms per `Delay(1)`; `pump()` between them made `idle 700` take ~1520 ms | Fixed 2026-08-02. `wait_frame()` has the same under-count, which makes `within=` looser than it reads, left alone, tightening it moves every timing case at once |

### AmiTCP_NG

Comes up on a Kickstart 3.1 directory boot once `rexxsyslib.library` and the
math libraries are in `LIBS:`. The earlier `errno 43` `EPROTONOSUPPORT` from
`AddNetInterface` was not a protocol-domain fault and not an OS-version
requirement. Measured 2026-08-02 as the third arm: read 1108 KB/s against our
983 and Roadshow's 1824.

## Decided against, do not "fix"

### SANA-II DMA buffer management, scanned 2026-08-07

The receive copy is 18.6% of an A1200 wire transfer, `n68k_copy_sum_longwords`
and `n68k_copy_bytes` at 955 and 791 samples of 9376. Both copies are mandated:
`S2_CopyToBuff` moves the frame out of the driver, `recv()` moves it into the
caller's buffer. `S2_DMACopyToBuff32` / `S2_DMACopyFromBuff32` (`sana2.h`,
`S2_Dummy + 8`, `+ 9`) are the only interface that could remove the first one.

Every driver on hand, scanned for the tag constants it looks up:

| Driver | Buffer tags asked for |
|---|---|
| a2060, eb920, eb920-i6, ppp-serial, rs485 | `CopyToBuff`, `CopyFromBuff` |
| a2065, ariadne, ariadne_ii, hydra, slip, x-surf, x-surf-100 | + `PacketFilter` |
| cnet, cnet16 | + `DMACopyToBuff32`, `DMACopyFromBuff32` |
| ppp-ethernet | + `DMACopyToBuff32`, `DMACopyFromBuff32`, `CopyFromBuff16` |

Three of fifteen, none in general use, and not the a2065 every figure in this
tree is measured on. No driver asks for the non-DMA `CopyToBuff32` /
`CopyFromBuff32` at all, and only `ppp-ethernet` wants `CopyFromBuff16` although
`AMI_SANA2_OFFER_COPY16` offers the pair.

Offering the DMA pair costs a packet pool in DMA-reachable memory, with the
alignment and coherency that implies, to benefit cnet. Rationale sits beside the
tag list in `src/sana2/sana2_device.c`. A driver could compute a tag rather than
carry the constant, so this is evidence and not proof; none of the fifteen
appears to.


### From the 2026-08-04 sweep

**TCP**

| Item | Cost of doing it |
|---|---|
| Nagle | Nowhere to live: `nx_tcp_socket_send()` transmits the caller's packet directly and there is no output coalescing buffer, so it means a new buffer, a new timer, and a `TCP_NODELAY` that currently only stores a flag. Nagle plus this stack's 200 ms delayed ACK is the classic interactive stall |
| Sender SWS avoidance | Cheaper than Nagle (`nx_tcp_socket_send_internal.c:533-544` takes any non-zero window) but bites only against a peer advertising tiny windows, which no modern peer does, and changes what leaves the machine on every connection with no way to measure the result here |
| Restart-after-idle, RFC 5681 §4.1 | The burst it guards against is already capped at 8 segments (~12 KB) by `NX_TCP_MAXIMUM_TX_QUEUE`; the restart window would make that 3. Against that: 4 bytes and a clock read per socket per send, and changed congestion behaviour on every connection |
| RFC 3708 spurious-retransmission detection | Needs the SACK send side landed first, a per-segment record of what was retransmitted and when, and an undo of the congestion state. In-flight data is capped at 8 segments, so the undo is worth at most 8 segments |
| UDP checksum at enqueue rather than dequeue | `nx_udp_socket_receive.c:282-408`. Moving it makes the IP thread pay for every datagram including ones no application reads; at dequeue the cost falls on the reader and only for datagrams read. On a 7 MHz 68000 the checksum is the expensive part of the UDP path. Residual: a corrupt datagram briefly holds a queue slot and can make `select()` report readable |
| FIN-WAIT-2 timeout | The state is correctly absent: the FIN has been acknowledged, there is nothing to retransmit. Timing out a socket the application still holds breaks `shutdown(SHUT_WR)` then read-to-EOF, which is what a half-close is for. BSD and Linux time out only *orphaned* ones, and `bsd_closing_sweep()` already resets those at 60 s (`socket.c:605`, `:740-768`). Residual: that sweep is driven by `socket()`/`CloseSocket()`/library close rather than a timer, so an idle machine holds one `AmiSocket` until the next program does something |

**TLS**

| Item | Cost of doing it |
|---|---|
| Revocation, any kind | OCSP needs an HTTP client inside `tls.library` and a second TCP connection per handshake, on a machine where the handshake already takes 7–23 s. CRL fetch needs an unbounded download and DER walk in 1 MB. Stapling is the only affordable shape and needs `status_request` plus a CRL-signature path in nx_secure, for a web where most servers still do not staple |
| Constant-time CBC padding | ~290 extra bytes of HMAC-SHA256 per received record, ~10 ms per record at 7 MHz, a permanent ~20% record-layer tax. ChaCha20-Poly1305 is first in the preference list and is AEAD with no padding, and Lucky13 needs ~2²³ connections against a machine taking seconds per handshake. Encrypt-then-MAC is the better answer and is listed under *Recommended* |
| Static-RSA suites | Last in the preference list, so reached only when the server offers nothing better; on this hardware the alternative for those servers is plaintext HTTP rather than ECDHE. Cost of keeping: no forward secrecy for exactly those connections |
| keyUsage failing open when the extension is absent | RFC 5280 §4.2.1.3 makes an absent keyUsage unrestricted, and OpenSSL's `ku_reject()` behaves the same way. Requiring keyCertSign would break legacy CAs to enforce something the RFC does not say. The check that matters, basicConstraints `cA TRUE`, is enforced |
| X25519 and Ed25519 wiring | Not a table row: nx_secure's ECDHE is built on `NX_CRYPTO_EC` point encoding and X25519 is 32 opaque bytes with no point format, so it is a new key-exchange path (~300 lines plus an `NX_CRYPTO_METHOD`). Measure first, our P-256 has a precomputed base table, so the win may only be on the shared secret. Ed25519 certificates are effectively nonexistent on the public web; that half is declined outright |
| Resumption secrets at rest | There is no key on an Amiga to protect them with; a machine-derived key is obfuscation. The trade-off is disclosed at `tlslib.h:57-64` with two opt-outs, `TLSA_NoResume` and `TLSA_SessionFile ""` |

**Resolver and DHCP**

| Item | Cost of doing it |
|---|---|
| Ctrl-C sampled per retry rung rather than per 200 ms | Deliberate, documented at `netstack_retry.h:52-58`. Doing better means slicing the wait inside `_nx_dns_response_receive()` and threading an abort callback through every return path in the DNS client, for 2–4 s of latency with the one or two servers a lease hands out. Dropping `AMI_NET_ASK_CEILING` to 1 s is the cheap alternative and the wrong trade: it doubles query traffic on exactly the slow links where the ceiling matters |
| TC handling, TCP fallback, EDNS0 | Smaller than it reads: an A, AAAA or PTR answer always fits in 512 bytes, and the retry ladder already stops on a truncated response (`netstack_retry.c:51` treats an attempt returning before its wait as answered), so the cost today is an error message rather than a failed lookup. EDNS0 without a fallback ladder risks names not resolving behind middleboxes that drop EDNS0 queries; with one it is substantial. TCP fallback needs a socket, two-byte length framing and a second parse path |
| DHCP option 121, classless static routes | A feature, not a fix. Requesting it without parsing and installing it adds bytes to every DISCOVER for no behaviour. Doing it properly requires RFC 3442's precedence rule, when 121 is present the client **must** ignore options 3 and 33, and getting that wrong removes the default gateway. There is also no route-installation path to extend: option 33 is retrieved and reported (`netstack.c:2350`) and never passed to `nx_ip_static_route_add()`. Build and test the option-33 install path first |
| RFC 3396 long-option concatenation, and option 52 overload | Both claims are true and both are near-unreachable here: we request six options (1, 3, 6, 12, 15, 33) whose combined payload is far below the 312-byte options area, so a server has no reason to split an option or overload `file`/`sname`. Concatenation also changes `_nx_dhcp_search_buffer()`'s contract from "pointer into the packet" to "assembled into a scratch buffer", touching both callers. Revisit if option 121 is ever adopted, that is the option that makes servers do both |

**IPv6**

| Item | Cost of doing it |
|---|---|
| Preferred lifetime and `NX_IPV6_ADDR_STATE_DEPRECATED` | `NXD_IPV6_ADDRESS` carries no lifetime field at all (`nx_api.h:2329-2359`); the lifetime lives on the prefix (`:1119`) and is aged at `nxd_ipv6_prefix_router_timer_tick.c:165`. The RA's preferred lifetime is read and discarded at `nx_icmpv6_process_ra.c:291-306`. Adding this means per-address lifetimes and an ager in the fork, plus RFC 6724 rule 3 in source selection, which `_nxd_ipv6_interface_find` has no hook for |
| Privacy addresses (RFC 8981) and opaque IIDs (RFC 7217) | 8981 needs the lifetime infrastructure above plus regeneration timers and DAD-collision retry, and `NX_MAX_IPV6_ADDRESSES` is 6 across two interfaces, no spare slots for rotating temporaries. 7217 is the cheap alternative (a hash replacing the EUI-64 at `nx_icmpv6_process_ra.c:388-394` and `nxd_ipv6_address_set.c:152-162`) and gets most of the tracking benefit, but needs a per-installation secret surviving reboot and would change every existing machine's address on upgrade, which matters for anyone with firewall rules or AAAA records keyed on the current one |
| MLD | The recorded rationale was wrong and is corrected in place; the feature remains unimplemented. A snooping switch filters multicast rather than forwarding it, so "the switch forwards anyway" was never a reason. What makes it declinable is narrower: every group this stack joins is either link-local scope, which snooping does not filter, or joined through IGMP on the IPv4 side |

**httpd and WebDAV**

| Item | Cost of doing it |
|---|---|
| Free-space precheck on a chunked PUT | A chunked body cannot be size-checked before it arrives, that is why a client chunks. `httpd_sink_put` already turns a short write into 507 |
| 412 bodies carrying HTML rather than `<D:error>` | RFC 4918 defines no precondition element for the cases that produce a 412 here; §16 covers `no-conflicting-lock`, which is the 423 and is fixed. Clients key off the status |
| LOCK on an unmapped URL creating a locked empty resource, §7.3 | Conflicts with a design choice already recorded in the code: creating the resource means an abandoned lock leaves an empty file behind |
| A real XML parser, nesting, entities, CDATA, comments, and UTF-16 request bodies | Days of work for a machine with 1 MB. UTF-16 bodies have never been observed from Finder or the Windows redirector |
| Dead properties, RFC 4918 §9.2 SHOULD | AmigaOS offers a 79-byte filenote or a sidecar; a sidecar costs `Lock`+`Examine`+`Open`+`Read` per entry per PROPFIND, doubling listing cost on a 68000 and the file count of every drawer, and a `.props-<name>` scheme collides on OFS's 30-character truncation. Measured client cost of having none: Explorer's creation/access times and attribute bits, nothing else |
| Unbounded multistatus, §9.8.3 | The 8-entry / 768-byte / 8-property bound is deliberate. Honouring it needs a spill file or a streamed 207, and a streamed 207 cannot be retracted once the head is out |
| Top-level DELETE aimed straight at a hard link | `Lock()` follows the link and `Examine()` reports the target's type, so catching this needs a parent-directory scan. The entry has to be placed by the machine's owner, `httpd` creates no links, which makes it a footgun rather than a remote attack. The `ExNext()` recursion, which was the reachable half, is fixed |

**Entropy**

| Item | Cost of doing it |
|---|---|
| Better entropy gathering than E-clock jitter | There is none below timer.device V51, and the supported floor is 2.04. `gather_clock()` credits 8 bits only when `tv_secs != 0`, so a machine with no RTC credits nothing and `gather_jitter()`'s 12-bit cap plus the memory and task scraps is what is left: about 18 bits. Inventing a source the hardware does not have is worse than recording the number |

**Infrastructure**

| Item | Cost of doing it |
|---|---|
| Profiler source-file and line attribution | **Declined on measurement, 2026-08-04.** The writer side is all present, the gcc driver's link spec carries `%{g:-amiga-debug-hunk}`, `ld`'s amiga script references `.debug_line`, and `m68k-amigaos-addr2line` ships with `-j --section=`. The reader side does not work: `profspin` built at `-Os` with `-g` on both compile and link, and again with `-Wl,--amiga-debug-hunk` forced, yields `.text`, `.data`, `.bss` and no debug section under `objdump -h`. Implementing it would mean hand-parsing the debug hunk or patching BFD, not the ~200 lines of Python the existing `(hunk, offset)` machinery would otherwise need |
| Profiler call counts, inclusive/exclusive split | Sampling cannot produce counts. An instrumenting mode sharing the report format is a second tool, not an extension. No use case yet |
| `src/mbuf` slab cleanup | Cannot leak today: no `mbuf_*` LVO is wired (`src/bsdsocket/CMakeLists.txt:126-129`, `bsdsocket_vectors.c:132-141`), so `ami_mbuf_cleanup()` having no production caller costs nothing until one is. A note now sits at the call-less definition |
| A real baton-slot sweep | Needs a `struct Task *` liveness test; the obvious one is wrong in exactly the window that matters. The wipe at stack shutdown is landed and is the half that can be done correctly |

**`SO_BROADCAST` is accepted and not enforced**, recorded at `options.c:185-190`.
BSD makes it a permission, `sendto()` to a broadcast address is `EACCES`
without it, and this stack has never asked for it, so enforcing it now would
start failing sends that work today, on a library that has users.

**`SO_OOBINLINE` is accepted and its value deliberately not stored**,
`options.c:266`. The urgent byte is delivered in the stream whatever the caller
sets, so reporting back a 0 would be the one answer that is certainly wrong.
Withholding the byte, the option OFF, means rewriting a queued segment the
TCP state machine still owns and counts in its sequence space, to hide a byte
the caller is about to see again (`oob.c:52-62`, RESEARCH 17).

**Host-side cycle counting: Moira and Musashi both rejected**, 2026-08-01,
branch `agent/moira-eval`.

- Moira's 68000 timing is exact only in a configuration it does not ship.
  `MOIRA_PRECISE_TIMING` defaults false, making `SYNC(x)` a no-op, so
  data-dependent costs are discarded, `MULS.W` is charged its worst case 54
  whatever the operand. `MOIRA_MIMIC_MUSASHI` defaults true and its own comment
  says to turn it off. Both are unconditional `#define`s. Patched it matches
  M68000PRM 30/30 including `MOVEM.L (An)+` at 12+8n; unpatched 29/30.
- **Moira's 68020 has no instruction cache**, measured: the same loop body from
  64 to 640 bytes costs exactly 8.000 cycles per pair at every size, with no
  turn at 256 where FS-UAE's 020 shows one. **Must not be used to choose unroll
  depth.**
- Errors against real measurement do not share a sign: 68020 copy +23%, 68000
  checksum at 20 B −41%, alignment penalty +1.2% where the machine gives +31%.
  Structural, Moira is a CPU, not a machine: flat always-ready RAM, no chip-RAM
  contention, no prefetch overlap, no cache, no unaligned-access penalty, which
  is the effect `n68k_copy.S` aligns its destination to avoid.
- Musashi is worse: its 68020, 68030 and 68040 share one cycle table (verified
  by dumping it) and that table is the 020 best case, i.e. a permanent 100%
  I-cache hit. `USE_ALL_CYCLES()` charges a whole timeslice to a spin loop,
  inventing a hotspot on exactly the busy-waits a network stack does.
- **Nobody has a cycle model above the 68020.** WinUAE's own 020+ cores have
  cycle accumulation `#if 0`'d out in `gencpu.cpp`.

**The ThreadX tick rate stays at 50**, 2026-08-01. Swept 20/40/50/60/80/100 Hz,
8 runs per arm, interleaved, 48 runs all PASS. Between-arm span is not larger
than within-arm spread on any metric and is not monotone in rate (80 Hz reads
higher than 60).

Mechanism: **wakeups stay at ~850 whatever the rate**, because the source is
`timer.device UNIT_VBLANK` and the knob does not touch it. Below 50 Hz the extra
wakeups are empty (59% idle at 20 Hz); above it they deliver catch-up bursts
(841 of 848 wakeups deliver more than one tick at 100 Hz). Instantaneous skew
was 0 in all 48 runs; nothing was clipped, lost, deferred or over budget.

20 Hz delivers 339 ticks instead of 867 and skips 493 wheel walks for 0 KB/s.
The argument against it is not throughput: `src/bsdsocket/options.c:83` derives
`SO_RCVTIMEO`/`SO_SNDTIMEO` granularity from `1000000/NX_IP_PERIODIC_RATE`, so
20 Hz coarsens socket timeout resolution from 20 ms to 50 ms.

Two traps for anyone who tries. `-D` cannot set the knob:
`port/netxduo-amiga/inc/nx_user.h:34` hard-defines `NX_IP_PERIODIC_RATE 50`
unconditionally and is included before `nx_port.h`'s `#ifndef` fallback, so
overriding `TX_TIMER_TICKS_PER_SECOND` alone silently rescales every TCP timer
by the ratio, a sweep that looks plausible and measures nothing. Both headers
must move together. And there is no delayed-ACK confound to isolate:
`nx_tcp_enable.c:111` computes `_nx_tcp_ack_timer_rate` as
`ceil(NX_IP_PERIODIC_RATE / NX_TCP_ACK_TIMER_RATE)`, so the ACK interval
self-scales to a constant 200 ms at every rate.
- **RFC 3542's extension headers stay unimplemented.** `IPV6_RTHDR`,
  `HOPOPTS`, `DSTOPTS`, `RTHDRDSTOPTS`, `PATHMTU`, `RECVPATHMTU`,
  `USE_MIN_MTU`, `DONTFRAG` and `NEXTHOP` are extension-header and path-MTU
  state NetX Duo does not expose, so there is nothing under them to reach. The
  rest of RFC 3542 is built; what its constants were fixed at, and why they
  cannot move, is in `docs/NDK-ADDENDUM.md`.

- **A browse reports the whole peer cache, not the browse window**, and stays
  that way, 2026-07-31. `netstack_mdns_browse_collect()` walks
  `nx_mdns_service_lookup()` by index, which is the whole cache, and nothing
  ages entries from our side, the module expires them by TTL, and an unplugged
  machine sends none of RFC 6762 §10.1's goodbyes. Filtering to entries actually
  refreshed inside the window means walking the RR cache instead of the public
  lookup: `nx_mdns_rr_elapsed_time` and `nx_mdns_rr_remaining_ticks` carry the
  freshness and `NX_MDNS_SERVICE` does not. Not worth that until someone reports
  a switched-off machine lingering, which every other mDNS browser does too.

- **RFC 3678 source filtering stays out**, 2026-07-31.
  `IP_ADD_SOURCE_MEMBERSHIP`, `IP_BLOCK_SOURCE` and the `MCAST_*` family need
  IGMPv3, which the vendored NetX Duo does not implement, it speaks IGMPv2
  and the source lists have nowhere to go. RFC 1112 membership is what shipped
  (`src/bsdsocket/mcast.c`) and is what SSDP, UPnP and a ported mDNS actually
  call.

- **RFC 6724 default address selection**, 2026-07-31: does not apply here. It
  sorts a list of candidate destinations, and `getaddrinfo()` returns at most
  one address per family (the resolver under it answers with a single address,
  not a set, see `src/bsdsocket/addrinfo.c`). With two entries at most its
  rules collapse to "which family first", which is answered deliberately: IPv6
  then IPv4. It would start to matter only if the resolver ever returned
  address sets.

- **`vsyslog()` stays `ENOSYS`**, 2026-07-31. The two tags that aim it,
  `SBTC_LOG_FILE_NAME` and `SBTC_LOG_HOOK`, are refused (above), so a syslog
  that reached only the serial debug log would give a caller no way to direct
  its output or read it back. `LOGSTAT`/`LOGMASK`/`LOGFACILITY`/`LOGTAGPTR`
  stay stored and unread, which costs a caller nothing. If syslog is ever
  wanted it is those three together, not the call on its own.

- **`AAMR_AddressInUse` / `AAMR_MaskChangeFailed` are never produced**,
  2026-07-31. Answering the first truthfully means duplicate address detection
, probing for the address before committing to it, which is the real
  feature and a change in what the stack puts on the wire, needing a second
  machine holding the address to test against. The result code is downstream of
  that, and inventing one without the probe would be a worse answer than
  `AAMR_Ignored`. Raise it as DAD if it is wanted, not as a code.

- **`SBTC_IP_FILTER_HOOK` and the `mbuf_*` family**, 2026-07-31. The hook hands
  a filter an mbuf chain, so servicing it means synthesising BSD mbufs around
  NX_PACKETs for every IP packet in and out, a packet-buffer abstraction the
  stack does not otherwise have, on the hot path, for a facility whose only
  real caller is a firewall nobody has asked for. Both stay stubbed together.

- **`IFQ_MaxReadRequests` / `IFQ_MaxWriteRequests` stay unanswered**,
  2026-07-31. The autodoc types them `(LONG)` where all 40 neighbours are
  `(LONG *)`, and on a query a bare `LONG` has nowhere to put the answer, so it
  is almost certainly a typo, but writing through a `ti_Data` that a caller
  passed as a scalar would corrupt its memory, and there is no way to tell the
  two apart at the call. They fall to the `default:` branch, which ignores
  unknown tags rather than refusing them, so nothing else in the list is lost.
  The same reasoning is in `src/bsdsocket/interfaces.c` beside the tag.

- **`SBTC_LOG_FILE_NAME` and `SBTC_LOG_HOOK` are refused**, 2026-07-31. The
  autodoc sanctions it in their own entries: "This tag is an extension to the
  AmiTCP V4 API and cannot be expected to be supported by older
  'bsdsocket.library' versions." Neither constant is in the NDK headers either,
  so a caller has to define it before it can pass one. Refusing costs a rare
  caller a tag list it was told to expect to lose.

- **`sendto()` with an address on a connected UDP socket.** Doc says `EISCONN`;
  everything portable dropped that rule.

- **UDP send with no destination returns `EDESTADDRREQ`**, not `-udp-`'s
  `ENOTCONN`. Ours is what 4.4BSD returns and what portable code checks.

- **`listen()` on an unbound socket fails** rather than auto-binding.

- **`MSG_OOB` refused by the msghdr forms**; `MSG_DONTROUTE` accepted and
  ignored.

- **`listen()` backlog is 8**, where the doc's BUGS claims a silent limit of 5.

- **`Dup2Socket` allows any in-range slot**, the doc's `EBADF` wording would
  make `dup2` onto a free slot illegal, defeating the call.

- **Request-count defaults** (ours sized from the packet pool, not 32/32/4).

- **`IP_DEFAULT_TTL` is 128**, doc says 64. Deliberate, 2026-07-31.

- **`gai_strerror()` takes its argument in A0.** The autodoc says D0; the SFD
  and pragmas say A0, and callers link against the pragma. The doc is wrong.

- **`bpf_open()` returns 0 for channel 0**, against the doc's *"handle > 0"*,
  RESEARCH §60 decoded Roadshow's own libpcap using it as a 0-based channel.

- **`OpenLibrary` allows Tasks and non-opener Processes**, which the doc
  denies. More permissive, never less.

- **`GetDefaultDomainName()` refuses rather than truncates.**

- **Packet pool sizing left alone**, 2026-07-31, the heuristic reaches the
  floor on small machines (1 free of 17 observed on 1 MB), so the minimum must
  stay.

- **No stripped drawer beyond `68000-minimal`**, 2026-07-31, the full build is
  what generates useful bug reports.

- **Two physical NICs**, dropped 2026-07-31: `romtype_restricted()` keeps only
  the first card, so it is untestable, and the complexity is not worth a rare
  case. Recovery SHA `d22a33e`.

- **A Kickstart 1.3 build**, dropped 2026-07-31: TheWire13 already covers that
  platform. Our side was closer than expected, tag walking is hand-rolled so
  `utility.library` is never needed, the library's DOS surface is all V33, and
  the 68000 build ships, but `ReadEClock` is V36 and everything 0.14.0 did
  for clock correctness rests on it; a 1.3 fallback is the 50 Hz CIA/VBlank
  source, exactly one tick of resolution, which retires the timer budget. The
  tools are the volume (`ReadArgs` 27 sites, `FreeArgs` 191, `VPrintf` 12).
  Never established whether any SANA-II driver runs under V34 at all, which was
  the gating question.

## Withdrawn, do not re-raise

Rows that did not survive re-checking. Kept because each was raised from a
plausible reading of a real symptom, and the same reading will recur.

| Claim | What is actually the case |
|---|---|
| The CI analyze stage silently skips cppcheck | It does not. `stage_analyze` is only ever invoked as `stage_analyze \|\| true`, and bash suppresses `set -e` inside a function called as part of an `\|\|` list. Reproduced both directions |
| `ugl_crypt()` returning `"*"` is a live lockout bypass | A hazard for third-party callers, not a live bypass: it also sets `UG_ENOSYS`, and nothing in the tree calls it |
| `clients/dropbear/build.sh` and `dist/make-dist.sh` are release-only steps CI never runs | Both are invoked by `.github/workflows/release.yml:155` and `:162`. They run on a release tag rather than on every push, which is a narrower statement than the row made and not by itself a defect |
| Stale root-level duplicates, `src/tools.h`, `stage-developer.sh`, `aminetxduo_lib.sfd`, `tmp_x/` | All gone. None is in the tree |
| **Keepalive is untested and needs `TCP_KEEPIDLE`** | Neither. `CMakeLists.txt:265-270` exposes `-DAMINETXDUO_TCP_KEEPALIVE_INITIAL=` and `keepalive.drill`'s own header names the arm it needs. Built with `=5` on hardware: 4 cases, 0 failed, 34 checks. The row came from running the script against a default build, where every case fails correctly because two hours have not passed |
| UDP demux ignores the 4-tuple | The filter exists: `bsd_udp_from_peer()` (`transfer.c:516-542`) applied at `:1123` with a local-address filter, and the recv loop drops and re-waits rather than failing. `_nx_udp_socket_bind` refuses a second socket on a bound port (`nx_udp_socket_bind.c:281-283`), so the `:240` port compare has one candidate. Residual, not the row: a wrong-peer datagram is still enqueued and counts against queue depth |
| Socket-option surface entirely untested | Stale. `tests/sockopt/sockopt_test.c:405-967` covers it; commit `e9274c7` records that it was run and found 24 defects |
| `tests/perf/prof/` is a dead fork of `tools/profiler/` | Wrong in three ways. Not dead, `tests/perf/CMakeLists.txt:190` adds it behind `AMINETXDUO_PROFILER`. The incompatible sample record is deliberate: `tools/profiler/prof.h:103-107` split the magics so an old reader cannot misread a new file. And the lineage is inverted, `profreport.py:114-117` calls the `tests/perf/prof` format "a different, older format", so `tools/profiler/` is the descendant |
| Broadcast SYN answered, destination unchecked | Fixed as `1e0e80f0` (`nx_tcp_packet_process.c:151-177`). The half-open slot half is also bounded: `bsd_listen_rearm()` tops the parked list up to `as_Backlog` (`socket.c:1889-1900`) from `listen()` and `accept()`. `tests/tcpdrill/scripts/bcast.drill` asserts both |
| `REQUIRE_RENEGOTIATION_EXT` undefined lets a handshake complete with an un-upgraded server | Impact is nil. `nx_secure_tls_client_handshake.c:248` renegotiates only when `renegotation_enabled && secure_renegotiation`, and the latter is set only by a valid RFC 5746 extension; otherwise a HelloRequest returns `NX_SECURE_TLS_NO_RENEGOTIATION_ERROR`. Defining the macro would refuse *initial* handshakes with unpatched servers, breaking connections without closing an attack |
| Resumption needs `SetProtection` | Nothing behind it. AmigaOS `FIBF_GRP_*`/`FIBF_OTR_*` are active-high and default to 0 on a new file, so group and other already have no access. The call would be a no-op |
| `is_seeded` is not on the private context vector, so `tls.library` cannot check it | `nxc_random_entropy_bits()` is on the vector at `nxcontext.h:203` and `tls_netx.c:285` already forwards to it. `tls.library` can answer today with no new slot and no ABI break. The entropy *quantity* problem in the same row is real and is under *Recommended* |
| `o02_duplicate_segment` fails | Fixed 2026-08-04 (`nx_tcp_socket_packet_process.c`, `packet_data_length > 0` on the RFC 793 §3.9 reply). Its counterpart `d06_no_dsack_without_permission` asserted the opposite, `notx 400` where a plain ACK belongs, and was the one that was wrong |
| MUST-23: `NX_TCP_MAXIMUM_RETRIES 7` → 255 s | The numbers were stale: the tree had 6 retries → 127 s. R2 for SYN now has its own budget and is 191 s, verified on the host |
| The IPv4 ID row's three particulars | The retransmitted header's length, TTL and checksum are **not** stale, a retransmission rewrites every one to the same value. `NX_ENABLE_IP_ID_RANDOMIZATION` **is** defined, at `port/netxduo-amiga/inc/nx_user.h:458-460`, gated off by default. There are three TCP socket-create sites, not four; the fourth is UDP. The real defect the row missed: `nx_packet_identical_copy` was never cleared, only set, so a packet that went out identical once reused the original ID on every later retransmission, where the ACK and window had moved. Fixed |
| Question comparison being case-sensitive breaks DNS-0x20 | Backwards. 0x20 *requires* an exact comparison, and nothing here randomises the case it sends, so the exact compare bought no entropy and cost real resolutions: a caller spelling a name with any capital got `NX_DNS_MISMATCHED_RESPONSE` from every server that normalises. The cache half already folded case, so the two halves of the client disagreed about what a name is. Fixed |
| `nxd_dhcp_client.c:6974` compares seconds against ticks | Only `:6939` does. The rebind arm at `:6974` converts to ticks at `:6971` and then compares ticks to ticks, correct code thirty lines below the broken code |
| Document root with a trailing slash may resolve to its parent | Not true as written: `http_path_resolve` already guards the join (`httppath.c:223`) so `root + "/"` never doubles a slash. The only unguarded case was the root used by itself, which is now normalised unconditionally so the question does not arise |
| Hard-linked directories walked through because `ST_LINKDIR` tests as a directory | The mechanism is wrong: `Lock()` follows the link and `Examine()` reports the *target's* type, so the top-level check can never see a link. The only place `ST_LINKDIR` is visible is the `ExNext()` scan, which is where the recursion was, and where it is fixed. The top-level case remains and is under *Decided against* |
| PROPPATCH naming no settable property emits an invalid `<D:response>` | Partly wrong: properties named but none settable already emitted a valid 403 propstat (`httpd.c:3952`). Only `props == 0` produced the invalid response. An unlisted bug next to it was worse, the 9th and later properties were dropped silently, so a large PROPPATCH reported on eight and the client read that as an answer about all of them. Both are 400 now |
| `AMI_MDNS_PRIORITY` is defined at `netstack_mdns.c:48` | `:45`. The row's line was wrong; the finding was not |
| PKCS#1 "OID discarded" is exploitable | Not once the padding is exact. The hash algorithm comes from the certificate's own `signatureAlgorithm`, which is inside the signed body and is now pinned to the inner copy, and the digest length and bytes are both compared. A digest-OID table would need new OID constants for no gain |

## Environment and tooling

| Item | Detail |
|---|---|
| The httpd drill cannot be driven from the machine hosting the guest | Amiberry bridges over pcap, and a GET from that host to the guest's address never answers; the same GET from another machine on the LAN answers 200. `run-httpd.sh` says "the guest never answered from this host", which reads as a stack fault and is not one |
| A task can hold about five library bases that use a `WaitSelect()` timeout | Each base takes an event signal from the calling task (`library.c:244`) and the first timeout takes another for the timer (`select.c:475`), out of a Task's 32 bits. `OpenLibrary()` refuses once they run out, which is correct. Found 2026-07-31 when eight opens got five; `run-cycledrill.sh` now asks for four |
| `run-fitzbench.sh`'s write figure is not a rate | Stops timing when the write call returns, not when data drains. Guest-timed 1718 KB/s against a measured wire rate of 364. Reads agree between clocks; writes diverge ~4.7× |
| `run-fitzbench.sh` refuses a same-host virtual peer | Over the uncomputed TX checksums that `ethtool -K <iface> tx off` fixes. Can be relaxed. Query by full path, `/usr/sbin` is not on a non-login ssh PATH |
| cppcheck stage skips itself | Baseline from 2.20.0, gate hosts have 2.17.1. Install 2.20.0 or regenerate |
| A guest program launched from `S:Startup-Sequence` sees `argc == 1` | The line the harness writes is `<prog> <args> >DH0:stdout.txt` and AmigaDOS does pass the words, but this toolchain's `crt0` does not build `argv` from them, so an `argc`/`argv` test silently takes its default path. Unrelated to the `&__argv` bug below: `fix-toolchain-crt0.py --check` reports the pinned root `11 immune, 1 skipped`. Read the line with `GetArgStr()` (no template) or `ReadArgs()`, which is what every `src/tools/*.c` already does | `tests/crypto68k/c68k_test.c:504` |
| A model/CPU pairing that cannot boot times out with no output at all | An A1200, A3000, A4000 or CD32 Kickstart uses 68020 instructions, so `-c 68000` on one reaches the timeout having printed nothing -- no guru, no serial, no `stdout.txt` -- which reads as a slow test. `AMINETXDUO_KICKSTART_A1200` is not exported by the lab env, so `-m A1200` also falls back to the generic ROM. `amiberry-run.sh` now exits 2 on the pairing and warns on the fallback; for a 68000 use `-m A600` | `tools/amiberry-run.sh:151` |
| FS-UAE cannot boot headless | `FATAL: [GLAD] …`. Harnesses take `-A` for Amiberry and `-a ARGS` to pass arguments |
| A pinned toolchain install can carry the argv bug in all eleven `crt0.o` | GCC 16.1.1b locally built reports `11 buggy`, has the compiler fix for the frame skew, not newlib's `120371e` for the argv declaration. Anything built there against `-lc` without `fix-toolchain-crt0.py` hands ported clients `&__argv` |

**A600 networking works. Use `-N a2065`.** Corrected 2026-08-07; the paragraph
that stood here said A600 networking was impossible and was wrong twice over.
Stock Amiberry (`0fd577e`, unpatched), `quickstart=A600,0`, default 68000,
Kickstart 40.63, `~/amiga-assets/devs/a2065.device`, `slirp`. Nothing new is
needed. Amiberry autoconfigs Zorro II on the A600 profile:

    Card 5: Z2 0x00e90000   64K IO  7990 Ethernet
    Interface "eth0" configured, address = 10.0.2.15 ... exit status 0 after 24s

**The A600 has no Zorro slots physically; Amiberry's A600 profile has a Zorro II
bus anyway.** Reasoning from the real machine's expansion is what produced the
wrong answer, twice.

**PCMCIA is a CPU limit, not an A600 limit.** `-N ne2000_pcmcia` with
`cnet.device` bisects to `cpu_type` and nothing else — ROM, chipset prefs
(`CP_A600` and `CP_A1200` set identical PCMCIA values, `cfgfile.cpp:10342` and
`:10401`), memory, clock multiplier, 24-bit address space and `cnet16.device`
all ruled out:

| `cpu_type` on `-m A600 -N ne2000_pcmcia` | result |
|---|---|
| 68000 (default), 68010, `cpu_compatible=false`, `cpu_multiplier=16` | `Could not add interface "eth0" (Input/output error)` |
| **68020**, and 68020 with `address_space_24=true` or `cpu_multiplier=1` | leases 10.0.2.15 |

Both CPUs insert the card and walk ~196,000 attribute reads; they diverge inside
card.resource's CIS tuple walk at `PC=00FC31C4`, attribute offset `0xe2`. The
68000 emits an extra odd-byte read per register access — `GAYLE_READ 00DAA000`
*and* `00DAA001` for the single instruction at `PC=00FC2B52` where the 68020
emits only the first — which is `gayle_attr_wget` splitting into two
`gayle_attr_bget`s (`gayle.cpp:1806-1807`) against `gayle_attr_read`'s halved
indexing, `pcmcia_attrs[addr/2]` (`gayle.cpp:1164`). Whether that returns a
wrong value is not yet proven. `tools/amiberry-run.sh` refuses the pairing and
prints this table.

Not a memory limit, which was the confound: the failing A600 run had 4.6 MB
free, and an A1200 at `chipmem_size=2;bogomem_size=0;fastmem_size=0`, 1 MB of
chip and nothing else, the supported floor, brings the same card up and leases
an address with 374,760 bytes still free. That is also the first demonstration
of the README's 1 MB floor with a live interface rather than by inference;
`run-oommsg.sh` only proves the other end, that 512 KB cannot start the stack.

**Two interfaces: proved on a host, not provable on a guest.** TCP leaves from
the address `bind()` named (`nxd_tcp_client_socket_source_connect()` in the
fork); `tests/netstack/host/test_tcp_source_connect_host.c` asserts it against an
`NX_IP` with two `nx_ip_interface[]` entries, compiling the real connect, route
lookup and SYN build.

**Correction to an earlier note here**: `AddNetInterface eth0` does *not* hang
with a second card present. With `a2065` on Zorro and the NE2000 PC Card on
PCMCIA, different buses, Fast RAM held to 4 MB to stay clear of the credit-card
window at $600000, `AddNetInterface eth0 eth1` returns 0, `eth0` leases, and
`ShowNetStatus` lists both with `eth1` offline. The hang belonged to two Zorro
cards. The real limit is that `cnet.device` will not open while the A2065 is
present although it opens alone, and Amiberry offers exactly two network boards,
so that is the only pair available. `SrcProbe` takes a second address and a
destination for a machine that can host one.

**The `sana2_rx.c` reader orphan does not reproduce under emulation.**
`ami_sana2_rx_stop()`'s last-resort path logs `reader N did not stop; leaking
its stack` and leaks 32 KB when a driver ignores `AbortIO`, which a2065.device
2.16 is documented to do. Thirteen full teardowns under Amiberry logged it zero
times. Nothing is outstanding in the code, the free sits outside the started
gate where the teardown owns it, and `run-cycledrill.sh` greps every run and
fails on `AMINETXDUO_CYCLE_ORPHAN_FATAL=1`. What is missing is a sighting, and
only real hardware can provide one.

**`tests/clients/run-argvexit.sh` removed 2026-07-31; it never completed a run.**
Under Amiberry the guest boots, `ToolsSmoke` starts, `DH0:tools.txt` reaches the
`===== SYS:ArgvExit =====` header and stops, `ArgvExit` never returns from
`SystemTagList()`. The toolchain was ruled out separately:
`fix-toolchain-crt0.py --check` reports `11 ok, 1 skipped` for the frame skew and
`2 call site(s) already push __argv by value`, the immune case. The 256 KB
per-invocation stack leak it was written for is covered by
`clients/dropbear/run-fsuae.sh -A`, which runs `dbclient` six times in one boot
with `AvailMem()` printed after each, that found the leak (266,368 bytes a run)
and proved the fix (0). **A harness that has never run looks like coverage and
is not.**