# AmiNetXDuo

A TCP/IP stack for classic AmigaOS, and the networking commands to go with it.
It provides `bsdsocket.library` — the socket API that Amiga network software
already speaks — on top of
[Eclipse ThreadX NetX Duo](https://github.com/eclipse-threadx/netxduo), and it
drives the SANA-II network cards you already have.

> **Status: it works, and it is not finished.** It gets a DHCP lease, answers
> ARP, pings its gateway, resolves DNS and moves TCP in both directions. Real
> `curl` runs on it. It has **not been run on real hardware** — everything so
> far was measured on an emulated 68020 and 68030 under Kickstart 3.1 — so
> treat it as something to try, not something to depend on.

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

A 68020 or better, AmigaOS 3.1 or newer, and 4 MB of RAM. A SANA-II network
card: `a2065.device`, `ariadne.device`, `ariadne2.device`, `amiganet.device`,
`xsurf.device`, `xsurf100.device`, `cnet.device` and the PCMCIA drivers are all
offered by name in the installer, and anything else can be typed in.

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
| `fetch` | retrieve an `http://` or `https://` URL |
| `nc` | connect or listen, TCP and UDP, port ranges, timeouts |
| `telnet` | with enough option negotiation not to confuse a real server |
| `ftp` | passive and active mode, the standard command set |
| `NetTrace` | capture packets to a `.pcap` file Wireshark can open |

The installer copies all of them into `C:`.

## HTTPS, and curl

`fetch` handles `https://` on its own. Upstream **curl 8.21.0 also runs on a
14 MHz 68020** against this stack, unpatched, and pulls a 657 KB file
byte-identical at 117 KB/s.

Encryption on a machine this slow comes with one honest caveat. A first TLS
handshake costs about 7 seconds for a simple site and around 23 for a
three-certificate chain, and many large sites sit behind a CDN that hangs up
after roughly fifteen. **Reconnecting to a site you have visited before takes
about half a second**, because the session is cached and survives both a reboot
and a change of program — so sites that fail on the first attempt often work on
the second. Certificates are verified against 119 Mozilla roots, and the host
name is checked, unless you ask otherwise.

TLS is on by default, so a normal build ships `tls.library` and the trust
store. `-DAMINETXDUO_TLS=OFF` leaves both out if you want a smaller stack, and
`fetch` still works over `http://` without them.

## What is not there yet

- Nothing has run on **real hardware**.
- IPv6 is built only with `-DAMINETXDUO_IPV6=ON`.
- No TCP window scaling, so the window is capped at 64 KB, and no SACK.
- Inbound connections are untested against the wider internet, because the
  emulator we develop against provides no way to reach the guest from outside.

## Compatibility

AmiNetXDuo is an independent implementation of a *published ABI*. No AmiTCP,
AROSTCP, Miami or Roadshow code has been used, copied or disassembled. Olaf
Barthel's freely distributable Roadshow SDK headers and autodocs are used solely
as an ABI reference, for function offsets, tag values, structure layouts and
documented behaviour.

`bsdsocket.library` scores **128 of 142** on
[`bsdsocktest`](https://github.com/tbdye/bsdsocktest), an independent
conformance suite, with nothing failing — the remainder are skipped.

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
