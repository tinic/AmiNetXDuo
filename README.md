[![CodeQL](https://github.com/tinic/AmiNetXDuo/actions/workflows/github-code-scanning/codeql/badge.svg)](https://github.com/tinic/AmiNetXDuo/actions/workflows/github-code-scanning/codeql)

# AmiNetXDuo

An IPv4+IPv6 TCP/IP stack for classic AmigaOS, and the networking commands to go with it.
It provides `bsdsocket.library` — the socket API that Amiga network software
already speaks — on top of
[Eclipse ThreadX NetX Duo](https://github.com/eclipse-threadx/netxduo), and it
drives the SANA-II network cards you already have.

> It gets a DHCP lease, configures itself by SLAAC, answers ARP and neighbour
> discovery, pings its gateway, resolves DNS, moves TCP in both directions,
> does HTTPS, and accepts incoming connections from other machines. It scores
> **142 of 142** on the independent
> [`bsdsocktest`](https://github.com/tbdye/bsdsocktest) conformance suite, where
> Roadshow scores 138. Dropbear's `dbclient` runs on it, so the Amiga can `ssh`.
>
> Most of what is measured here was measured under emulation. It has since run
> on real hardware — an A3000 with an X-Surf-100, where a user measured
> 795 KB/s reading and 939 KB/s writing over Fitz, and found two bugs that are
> fixed.
>
> Note that if you want to try IPv6 under WinUAE you will need a patch [(tinic/winuae@d9df1d8)](https://github.com/tonioni/WinUAE/commit/d9df1d8357ade4f9631491cf9f482e159554bfeb)

## How this was written

Claude (Anthropic's Opus 5) wrote the code, under human direction and testing.
Every commit says so in its `Co-Authored-By` line, and
[docs/RESEARCH.md](docs/RESEARCH.md) is the engineering record kept as the work
happened — what was measured, what was tried and abandoned, and the conclusions
that later turned out to be wrong.

Whether that is worth trusting is a question about evidence rather than about
authorship, so the evidence is the part worth checking: an independent
conformance suite, seven build configurations in continuous integration, a
triaged static-analysis baseline, fuzzers, and every bug a user has reported
recorded with its fix and a test that reproduces it.

## Why

AmigaOS has never included a TCP/IP stack; networking has always come from a
third-party shared library, and every existing option has a catch. AmiTCP 3.0b2
is free but dates from 1994. AmiTCP 4.x and Miami are proprietary and
effectively unobtainable. Roadshow, the one genuinely modern stack, is
commercial and closed.

AmiNetXDuo is MIT-licensed throughout, with no 4.4BSD or GPL-derived code
anywhere in it. It speaks the same socket API and reads the same configuration
files as Roadshow, so existing software and existing habits carry over. It
speaks **IPv6**: on a network with an IPv6 router a machine picks up an address
without being configured for one. The commands that take a host take an IPv6
address — `ping`, `traceroute`, `nc`, `telnet`, `tftp`, `whois`, `fetch`,
`sntp`, `nslookup` and `host` — and
`ShowNetStatus` and `netstat -i` show the IPv6 addresses an interface holds.
`netstat -r` shows the IPv6 routes, `AddNetRoute` and `DeleteNetRoute` change
them, and `arp` lists the neighbours an IPv6 network is reached through.

## Requirements

Any 68000 or better, AmigaOS 2.04 or newer, and 1 MB of RAM. A SANA-II network
card: `a2065.device`, `ariadne.device`, `ariadne2.device`, `amiganet.device`,
`xsurf.device`, `xsurf100.device`, `cnet.device`, the PCMCIA drivers and
`uaenet.device` for emulators are all offered by name in the installer, and
anything else can be typed in.

The archive carries a separate build for the 68000, the 68020–68040 and the
68060, and the installer works out which one your machine wants. Encrypted
(`https:`) connections need a 68020 or better; on a 68000 the cryptography
takes longer than the other end will wait, so it is left out and everything
else works as normal.

## Installing

Download the `.lha` from [Releases](https://github.com/tinic/AmiNetXDuo/releases),
unpack it, and run `Install-AmiNetXDuo`. It asks a couple of questions and
writes a working configuration.

Configuration follows Roadshow's layout — `DEVS:NetInterfaces/<name>`,
`DEVS:Internet/routes`, `DEVS:Internet/name_resolution` and the standard
`/etc`-style netdb files — so existing documentation and habits apply. The
`ReadMe` in the archive covers the rest, including writing the files by hand.

## Commands

| | |
|---|---|
| `NetSetup` | set up an interface by answering questions -- start here |
| `AddNetInterface`, `Online`, `Offline` | bring an interface up and take it down |
| `ShowNetStatus`, `netstat` | interface state, routes, connections |
| `ping`, `host` | reachability and name lookups |
| `nslookup` | ask the DNS for one kind of record, from a server of your choosing |
| `arp` | which machines on this network have answered, and what they are |
| `sntp` | set the clock from a time server |
| `fetch` | retrieve an `http://` or `https://` URL |
| `nc` | connect or listen, TCP and UDP, port ranges, timeouts |
| `telnet` | with enough option negotiation not to confuse a real server |
| `ssh` | Dropbear's dbclient, public-key auth; see the ReadMe for keys |
| `NetTrace` | capture packets to a `.pcap` file Wireshark can open |
| `traceroute` | trace the path to a host |
| `tftp`, `whois` | the usual small clients |
| `CheckNetConfig` | read the configuration and say what is wrong with it |
| `AddNetRoute`, `DeleteNetRoute` | where packets go that are not for this network |
| `GetNetStatus`, `NetShutdown` | status for scripts, and a clean shutdown |

The installer copies all of them into `C:`.

## Finding the machine by name

The Amiga answers to **`<hostname>.local`** on the local network, so another
machine can reach it by name without any DNS server or configuration — and
`.local` names work from the Amiga in the other direction too. This matters
most when there is no DHCP server at all: the machine gives itself an address
and is still reachable.

It needs no separate command. `ping`, `host`, `fetch` and any older program
that resolves a name all get it, because the lookup happens inside the resolver
everything already uses.

## HTTPS / SSH

`fetch` handles `https://` URLs, and certificates are properly checked against
the usual set of root authorities.

`ssh` needs no special settings at the far end. Expect around ten seconds
before the prompt appears; almost all of that is the cryptography rather than
the network.

Keys must be in **Dropbear's own format** — an OpenSSH key copied straight
across will not be read — and you name the key on the command line with `-i`.
Make it on your PC rather than on the Amiga, which has neither the entropy nor
the patience. The ReadMe in the archive has the exact commands.

The first connection to a site takes twenty seconds or more, and some servers
give up before it finishes. The second attempt to the same site takes under a
second: the session is kept on disk and survives a reboot.

## Prior art

Two other modern-stack projects appeared in July 2026 and are worth knowing
about. [lwip-amiga](https://github.com/rondoval/lwip-amiga) combines lwIP with
`bsdsocket.library`, but uses a custom `netdev` driver ABI rather than SANA-II,
which restricts it to PiStorm and Emu68.
[AmiTCP_NG](https://github.com/MW0MWZ/AmiTCP_NG) is a GPL fork of AmiTCP 3.0b2
with a clean-room Roadshow ABI. There is a fuller survey in
[docs/RESEARCH.md §2](docs/RESEARCH.md#2-prior-art).

## Building it yourself

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake
cmake --build build --parallel
```

`tools/fetch-toolchain.sh` downloads the pinned m68k cross-compiler if you have
none. See **[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)** for the build options,
the test suites, continuous integration and how everything is measured, and
**[docs/RESEARCH.md](docs/RESEARCH.md)** for the engineering record.

## Licence

AmiNetXDuo is an independent implementation of a published ABI. No
implementation code from AmiTCP, AROSTCP, Miami or Roadshow has been used,
copied or disassembled.

The ABI itself comes from interface definitions and documentation that exist to
be read, and naming them is more useful than a blanket denial: Olaf Barthel's
freely distributable Roadshow SDK headers and autodocs, the NDK's
`pragmas/bsdsocket_pragmas.h`, and the `.fd`/`.sfd` function-descriptor files
AmiTCP and Roadshow publish — `usergroup.library`'s 39 vectors, for instance,
were settled by reading AmiTCP's `fd/usergroup_lib.fd` against Roadshow's
`sfd/usergroup_lib.sfd` and the NDK pragma, all three agreeing.
[docs/RESEARCH.md](docs/RESEARCH.md) names the source behind each vector table.

MIT. ThreadX and NetX Duo are MIT-licensed as well (© Microsoft and the Eclipse
ThreadX contributors). ThreadX is an unmodified submodule. **NetX Duo is not** —
it is a fork carrying seven patches, each on its own branch off upstream
`473d1928` and each written as a standalone change to submit upstream:
`amiga-mdns-big-endian`, `amiga-tcp-isn-entropy`, `amiga-tcp-accept-race`,
`amiga-tcp-retry-limit`, `amiga-ipv6-raw-hop-limit`,
`amiga-ipv6-raw-traffic-class` and `amiga-ipv4-broadcast-loopback`. What each
one fixes is in the engineering record. The one further exception is the CA root
set in `DEVS:Internet/certificates`, which is
Mozilla's, under MPL 2.0 — file-scoped, and affecting nothing else here.
