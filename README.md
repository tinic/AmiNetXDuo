# AmiNetXDuo

A TCP/IP stack for classic AmigaOS, and the networking commands to go with it.
It provides `bsdsocket.library` — the socket API that Amiga network software
already speaks — on top of
[Eclipse ThreadX NetX Duo](https://github.com/eclipse-threadx/netxduo), and it
drives the SANA-II network cards you already have.

> **Status: it works, and it is not finished.** It gets a DHCP lease, answers
> ARP, pings its gateway, resolves DNS, moves TCP in both directions and does
> HTTPS. It scores **141 of 142** on the independent
> [`bsdsocktest`](https://github.com/tbdye/bsdsocktest) conformance suite, where
> Roadshow scores 138. Real `curl` runs on it unmodified, and so does `ssh`.
>
> It has **only ever been tested under emulation, never on a real Amiga**, so
> treat it as something to try rather than something to depend on.

## Why

AmigaOS has never included a TCP/IP stack; networking has always come from a
third-party shared library, and every existing option has a catch. AmiTCP 3.0b2
is free but dates from 1994. AmiTCP 4.x and Miami are proprietary and
effectively unobtainable. Roadshow, the one genuinely modern stack, is
commercial and closed.

AmiNetXDuo is MIT-licensed throughout, with no 4.4BSD or GPL-derived code
anywhere in it. It speaks the same socket API and reads the same configuration
files as Roadshow, so existing software and existing habits carry over. And it
supports **IPv6** — as far as we can establish, a first for a classic Amiga
stack — though that is off by default for now.

## Requirements

Any 68000 or better — an unexpanded A500 or A600 included — AmigaOS 3.1 or
newer, and 4 MB of RAM. A SANA-II network card: `a2065.device`,
`ariadne.device`, `ariadne2.device`, `amiganet.device`, `xsurf.device`,
`xsurf100.device`, `cnet.device` and the PCMCIA drivers are all offered by name
in the installer, and anything else can be typed in.

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
`/etc`-style netdb files — so existing documentation and habits apply. There is
a user guide in `Docs/` inside the archive.

## Commands

| | |
|---|---|
| `AddNetInterface`, `Online`, `Offline` | bring an interface up and take it down |
| `ShowNetStatus`, `netstat` | interface state, routes, connections |
| `ping`, `host` | reachability and name lookups |
| `sntp` | set the clock from a time server |
| `fetch` | retrieve an `http://` or `https://` URL |
| `nc` | connect or listen, TCP and UDP, port ranges, timeouts |
| `telnet` | with enough option negotiation not to confuse a real server |
| `NetTrace` | capture packets to a `.pcap` file Wireshark can open |
| `traceroute` | trace the path to a host |
| `tftp`, `whois` | the usual small clients |
| `CheckNetConfig` | read the configuration and say what is wrong with it |
| `AddNetRoute`, `DeleteNetRoute` | where packets go that are not for this network |
| `GetNetStatus`, `NetShutdown` | status for scripts, and a clean shutdown |

The installer copies all of them into `C:`.

## curl and ssh

The archive has a `Clients` drawer holding two ported Unix programs:

| | |
|---|---|
| `curl` | the real thing, with the options you already know |
| `ssh` | connects to an ordinary modern server, using a key |

The installer puts them in the `AmiNetXDuo` drawer alongside the
documentation, rather than in `C:`, because each needs something this archive
cannot provide for you:

- **`mathieeedoubbas.library` in `LIBS:`.** It is Commodore's, so it is not in
  the archive, but every Workbench installation has one.
- **A much larger stack than a Shell gives a command.** Type `stack 200000`
  once in the Shell you are going to run them from.

Copy them into `C:` yourself if you would like them on your path.

## Finding the machine by name

The Amiga answers to **`<hostname>.local`** on the local network, so another
machine can reach it by name without any DNS server or configuration — and
`.local` names work from the Amiga in the other direction too. This matters
most when there is no DHCP server at all: the machine gives itself an address
and is still reachable.

It needs no separate command. `ping`, `host`, `fetch` and any older program
that resolves a name all get it, because the lookup happens inside the resolver
everything already uses.

## HTTPS

`fetch` handles `https://` URLs, and certificates are properly checked against
the usual set of root authorities. So does `curl`, if you want more options.

`ssh` needs no special settings at the far end. Expect around ten seconds
before the prompt appears; almost all of that is the cryptography rather than
the network.

One thing to expect on a machine this slow: **the first connection to a site can
take twenty seconds or more, and some sites will give up before it finishes.**
Try again — the second attempt to the same site usually takes under a second,
because the secure session is remembered, even across a reboot.

## What is not there yet

- Nothing has run on **real hardware** yet.
- IPv6 works but is not in the standard build.
- Accepting connections *from* the internet is untested.

## Compatibility

AmiNetXDuo is an independent implementation of a *published ABI*. No AmiTCP,
AROSTCP, Miami or Roadshow code has been used, copied or disassembled. Olaf
Barthel's freely distributable Roadshow SDK headers and autodocs are used solely
as an ABI reference, for function offsets, tag values, structure layouts and
documented behaviour.

Software built for other Amiga TCP/IP stacks runs on this one unmodified, which
is the point of implementing the same ABI rather than a new one.

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

MIT. ThreadX and NetX Duo are MIT-licensed as well (© Microsoft and the Eclipse
ThreadX contributors) and are consumed as unmodified git submodules. The one
exception is the CA root set in `DEVS:Internet/certificates`, which is
Mozilla's, under MPL 2.0 — file-scoped, and affecting nothing else here.
