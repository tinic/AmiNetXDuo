# Security

## Reporting something

Use [GitHub's private vulnerability reporting](https://github.com/tinic/AmiNetXDuo/security/advisories/new).
It is enabled on this repository, so a report stays private until there is a fix
to publish alongside it. A public issue is fine for anything already public, or
for a crash whose cause is not yet known to be reachable from the network.

There is no bounty and no guaranteed response time. This is a hobby project.

A useful report says which build (`--version` on any command prints it), which
SANA-II driver and card, and what the machine received or was asked to do. A
serial log from a debug build (`-DAMINETXDUO_LOG_LEVEL=2`) is worth more than a
description of the symptom.

## What an attacker gets if this stack has a bug

AmigaOS has no memory protection and no privilege separation. `bsdsocket.library`
runs in the same address space as every other program on the machine, and
`sana2.device` receive callbacks run at interrupt level. A buffer overrun here is
not contained by anything: it is arbitrary memory corruption on a machine where
the debugger, the file system and the user's data share one flat address space.

That is a property of the platform in 1985 and cannot be engineered away. It is
the reason the parsers matter more than they would on a modern OS, and the reason
this document names its untested parts rather than only its tested ones.

## The trust boundary

**Untrusted — anything reachable from the network:**

| what | parsed by |
|---|---|
| IPv4/IPv6, TCP, UDP, ICMP, ICMPv6, ARP, neighbour discovery | NetX Duo |
| DHCP and DHCPv6 responses, router advertisements | NetX Duo |
| DNS responses, including compression pointers | `src/netstack/netstack_dns.c` |
| mDNS queries and responses — unauthenticated multicast from any host on the segment | `src/netstack/netstack_mdns.c` |
| TLS records and X.509 certificate chains | NX Secure, in `tls.library` |
| SSH protocol | Dropbear's `dbclient`, vendored in `clients/` |

**Trusted, and worth knowing that it is:**

- `DEVS:NetInterfaces/`, `DEVS:Internet/` and `ENV:` — whoever writes those
  configures the machine, and is assumed to be its owner.
- The SANA-II driver. It is third-party code the stack hands buffers and
  callbacks to, and it can write wherever it likes. This is not theoretical:
  `x-surf-100.device` 1.16 stops calling the buffer-management callbacks it was
  given and walks `ios2_Data` as an AmiTCP mbuf chain when it finds an `AMITCP`
  public port, which for this stack meant copying received frame bytes to an
  address read out of a structure that is not an mbuf. Fixed in 0.12.2 by
  removing the port across `OpenDevice()`; `docs/RESEARCH.md` §71 has the
  disassembly. A driver is inside the boundary whether or not it deserves to be.
- BPF filter programs. `bpf_*` compiles and runs filters from a local process;
  the VM bounds-checks, but the interface is local, not remote.

## What is tested, and how

- **`bsdsocktest`** — an independent conformance suite written for this ABI by
  someone else. 142/142 on a bridged real network; 130/142 with 12 skipped on
  loopback, where the skipped tests need a second machine. Roadshow scores 138.
- **Fuzzing** — `fuzz_config` over the configuration parsers and `fuzz_bpf` over
  the filter VM, both with sanitisers on the host.
- **Static analysis** — GCC `-fanalyzer` over the whole tree against a triaged
  baseline of 13 findings, in CI, warnings fatal. cppcheck against a separate
  baseline of 16, run locally rather than in CI because its output moves between
  its own releases.
- **Seven build configurations** in CI, including 68000, 68040 and 68060, and the
  builds with IPv6 and with TLS turned off.
- **Enforcer and MungWall** runs under emulation, which is how illegal accesses
  and freed-memory writes surface on a machine with no MMU.
- **Real hardware** — an A3000/060 with an X-Surf-100, by a user, which is where
  two bugs were found that emulation had not.

## What is not tested

Stated because a security policy that lists only its strengths is not much use.

- **The DNS and mDNS response parsers are not fuzzed.** They parse
  attacker-controlled bytes and they are the two most exposed pieces of code
  written for this project. DNS compression pointers are a well-known source of
  loops and over-reads. This is the largest known gap.
- **No audit against published Eclipse ThreadX advisories.** The vendored
  NetX Duo and ThreadX are 6.5.1, plus seven local patches; whether any known
  advisory touches the paths compiled here has not been checked.
- **`SO_BROADCAST` is permissive.** It is recorded on the socket and read by
  nothing, so a broadcast `sendto()` without it succeeds where 4.4BSD returns
  `EACCES`. A divergence, not a memory-safety issue.
- **TLS certificate validation** has not been tested against a suite of
  deliberately malformed or hostile chains.
- The stack has been driven by its own tests, an independent conformance suite
  and a handful of real clients. It has not been through an adversarial review
  by anyone.

## Provenance

The code was written by Claude (Anthropic's Opus 5) under human direction, which
[README.md](README.md) states and every commit records in `Co-Authored-By`.
Nothing about that changes what the tests show, in either direction. Anyone
weighing whether to trust this stack should weigh the evidence above, and the
gaps above, on the same terms as for any other implementation.

`docs/RESEARCH.md` is a contemporaneous record rather than a summary written
afterwards: it includes measurements that were wrong, conclusions that were
withdrawn, and approaches that were tried and abandoned. For a reader trying to
judge how carefully something was built, that is more informative than a
changelog.

## Supported versions

The latest release. This is a 0.x project and fixes go forward, not backward.
