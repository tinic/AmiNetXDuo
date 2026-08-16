# Security

## Reporting a problem

Use [GitHub's private vulnerability reporting](https://github.com/tinic/AmiNetXDuo/security/advisories/new).
It is enabled on this repository, so a report stays private until there is a fix
to publish beside it. A public issue is acceptable for anything already public,
and for a crash not yet known to be reachable from the network.

There is no bounty and no guaranteed response time. This is a hobby project.

A useful report states which build (`--version` on any command prints it), which
SANA-II driver and card, and what the machine received or was asked to do. A
serial log from a build configured with `-DAMINETXDUO_LOG=ON` is worth more than
a description of the symptom. A shipped build compiles the log away entirely, so
silence on the serial port from a shipped build is not evidence.

## Supported versions

The latest release. This is a 0.x project and fixes go forward, not backward.

## What a bug here costs

AmigaOS has no memory protection and no privilege separation.
`bsdsocket.library` runs in the same address space as every other program on the
machine, and `sana2.device` receive callbacks run at interrupt level. Nothing
contains a buffer overrun. It is arbitrary memory corruption on a machine where
the debugger, the file system and the user's data share one flat address space.
That is a property of the platform and cannot be engineered away.

## The trust boundary

Untrusted — anything reachable from the network:

| Input | Parsed by |
|---|---|
| IPv4/IPv6, TCP, UDP, ICMP, ICMPv6, ARP, neighbour discovery | NetX Duo |
| DHCP and DHCPv6 responses, router advertisements | NetX Duo |
| DNS responses, including compression pointers | `src/netstack/netstack_dns.c` |
| mDNS queries and responses — unauthenticated multicast from any host on the segment | `src/netstack/netstack_mdns.c` |
| TLS records and X.509 certificate chains | NX Secure, in `tls.library` |
| SSH protocol | Dropbear's `dbclient`, `clients/dropbear/` |
| HTTP requests, chunked bodies and WebDAV XML, from any client that reaches the port, **with no authentication anywhere in the server** | `src/tools/httpd.c` |

Trusted, and listed because that trust is not obvious:

| Inside the boundary | Why that matters |
|---|---|
| `DEVS:NetInterfaces/`, `DEVS:Internet/`, `ENV:` | whoever writes those configures the machine, and is assumed to be its owner |
| The SANA-II driver | third-party code that the stack hands buffers and callbacks to, and it can write wherever it likes. This is not theoretical. `x-surf-100.device` 1.16 stops calling the buffer-management callbacks it was given. It walks `ios2_Data` as an AmiTCP mbuf chain whenever it finds an `AMITCP` public port, which copied received frame bytes to an address read out of a structure that is not an mbuf. `src/netstack/netstack_rexx.c` removes that port across `OpenDevice()`. A driver is inside the boundary whether or not it deserves to be |
| BPF filter programs | `bpf_*` compiles and runs filters from a local process. The VM bounds-checks, but the interface is local, not remote |

## What is tested

| | |
|---|---|
| Conformance | `bsdsocktest`, an independent suite written for this ABI by someone else. `tests/conformance/run-fsuae.sh` |
| Fuzzing, host, ASan + UBSan, in `ctest` | `fuzz_config` (every parser that reads a file out of `DEVS:`), `fuzz_bpf`, `fuzz_dns`, `fuzz_usergroup`, `fuzz_dhcp`, `fuzz_tls_record`, `fuzz_tls_x509`, `fuzz_httpframe` |
| Fuzzing needing a 32-bit build (`tools/ci.sh host32`) | `fuzz_mdns` and `fuzz_tls_crypto`. NetX Duo's mDNS cache keeps pointers in `ULONG` slots, and the TLS crypto paths cast a pointer to a 32-bit `ULONG` in the signature bounds check itself. The stage counts the tests it ran, so a 64-bit configuration cannot report green having registered none |
| Fuzz depth | `fuzz_dns` drives the real client through `_nx_dns_response_receive()`, the name unencoder and the resource walk. `fuzz_mdns` enters at `_nx_mdns_thread_entry()`, so the module's own receive loop, interface lookup and packet processing run for real |
| Static analysis | GCC `-fanalyzer` over the whole tree against a triaged baseline, in CI, warnings fatal. cppcheck against a separate baseline, run locally rather than in CI because its output moves between its own releases |
| Build configurations | every arm in `CROSS_CONFIGS` (`tools/ci.sh`), including 68000, 68040 and 68060, and the builds with IPv6, TLS, mDNS, multicast, BPF and each TCP option turned off |
| Emulation | Enforcer and MungWall, which is how illegal accesses and freed-memory writes surface on a machine with no MMU. Also every supported network card, one guest each (`tools/ci.sh cards`) |
| Real hardware | an A3000/060 with an X-Surf-100, by a user, which is where two bugs were found that emulation had not |

## What is not tested

This section exists because a security policy that lists only its strengths is
not useful.

- **No audit against published Eclipse ThreadX advisories.** Whether any known
  advisory touches the paths compiled here has not been checked.
- **TLS certificate validation** has not been run against a suite of
  deliberately malformed or hostile chains.
- **`SO_BROADCAST` is permissive.** It is recorded on the socket and read by
  nothing, so a broadcast `sendto()` without it succeeds where 4.4BSD returns
  `EACCES`. This is a divergence, not a memory-safety problem.
- The stack has not been through an adversarial review by anyone.

## Provenance

The code was written by Claude (Anthropic's Opus 5) under human direction, which
[README.md](README.md) states and every commit records in `Co-Authored-By`.
Nothing about that changes what the tests show, in either direction. Judge the
evidence and the gaps above on the same terms as any other implementation.
`docs/RESEARCH.md` indexes what was found and whether it still holds.
