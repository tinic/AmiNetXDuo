[![CodeQL](https://github.com/tinic/AmiNetXDuo/actions/workflows/github-code-scanning/codeql/badge.svg)](https://github.com/tinic/AmiNetXDuo/actions/workflows/github-code-scanning/codeql)

# AmiNetXDuo

An IPv4+IPv6 TCP/IP stack for classic AmigaOS, with the network commands that go
with it. It provides `bsdsocket.library`, the socket API that Amiga network
software already speaks, on top of
[Eclipse ThreadX NetX Duo](https://github.com/eclipse-threadx/netxduo). It drives
existing SANA-II network cards.

> It gets a DHCP lease, configures itself by SLAAC, answers ARP and neighbour
> discovery, and pings its gateway. It resolves DNS, moves TCP in both
> directions, does HTTPS, and accepts incoming connections from other
> machines. It scores **142 of 142** on the independent
> [`bsdsocktest`](https://github.com/tbdye/bsdsocktest) conformance suite.
> Dropbear's `dbclient` runs on it, so the Amiga can `ssh`.

## Why

AmigaOS has never included a TCP/IP stack. Networking has always come from a
third-party shared library, and each existing option has a limitation. AmiTCP
3.0b2 is free and dates from 1994. AmiTCP 4.x and Miami are proprietary and
effectively unobtainable. Roadshow, the one modern stack, is commercial and
closed.

AmiNetXDuo is MIT-licensed throughout, with no 4.4BSD or GPL-derived code
anywhere in it. It speaks the same socket API and reads the same configuration
files as Roadshow, so existing software and existing habits carry over. It also
speaks **IPv6**: on a network with an IPv6 router, a machine picks up an address
without being configured for one.

The commands that take a host also take an IPv6 address: `ping`, `traceroute`,
`nc`, `telnet`, `tftp`, `whois`, `fetch`, `sntp`, `nslookup` and `host`.
`ShowNetStatus` and `netstat -i` show the IPv6 addresses that an interface
holds. `netstat -r` shows the IPv6 routes, and `AddNetRoute` and
`DeleteNetRoute` change them. `arp` lists the neighbours that an IPv6 network is
reached through.

## Existing software and IPv6

Software that resolves a name with `getaddrinfo()` and connects to whatever it
returns works over IPv6 without being changed. The call reports whichever kind
of address the network has. A program that passes that address straight to
`connect()` never has to name a family at all.

Software written for IPv4 keeps working over IPv4, which covers most of what is
already on an Amiga. A program is IPv4-only in three cases:

- It resolves with `gethostbyname()`, which cannot return an IPv6 address.
- It keeps an address in 32 bits.
- It reads a dotted quad out of a configuration file.

For such a program, an IPv6 host is a small change rather than a rewrite. The
change is to resolve with `getaddrinfo()`, and to hand what it returns to
`connect()` without looking inside it. The rest of the program stays as it is.

## Requirements

Any 68000 or better, AmigaOS 2.04 or newer, and 1 MB of RAM. A SANA-II network
card is also necessary. The installer offers these drivers by name:
`a2065.device`, `ariadne.device`, `ariadne_ii.device`, `hydra.device`,
`eb920.device`, `x-surf.device`, `x-surf-100.device`, `cnet.device` for PCMCIA,
and `uaenet.device` for emulators. Any other driver name can be typed in.

The archive also carries `anxnet.device` in `Devs/Networks/`. Install it by hand
for IPv6 on an X-Surf or an X-Surf 100. The drivers of those two cards refuse
the multicast addresses that IPv6 needs.

## Installing

Download the `.lha` from [Releases](https://github.com/tinic/AmiNetXDuo/releases),
unpack it, and run `Install-AmiNetXDuo`. It asks a few questions and writes a
working configuration.

The configuration follows the layout of Roadshow: `DEVS:NetInterfaces/<name>`,
`DEVS:Internet/routes`, `DEVS:Internet/name_resolution` and the standard
`/etc`-style netdb files. Existing documentation and existing habits therefore
apply. The `ReadMe` in the archive covers the rest, including how to write the
files by hand.

## Commands

| | |
|---|---|
| `NetSetup` | ask the questions for one interface, and write the configuration files |
| `AddNetInterface`, `Online`, `Offline` | bring an interface up and take it down |
| `ShowNetStatus`, `netstat` | interface state, routes, connections |
| `ShowNetServices` | what else on this network is offering something |
| `ping`, `host` | reachability and name lookups |
| `nslookup` | ask the DNS for one kind of record, from a named server |
| `arp` | the machines on this network that have answered, and what they are |
| `sntp` | set the clock from a time server |
| `fetch` | retrieve an `http://` or `https://` URL |
| `httpd` | share a drawer, so other machines can mount it as a drive. `-T` also serves a Shell in a browser |
| `nc` | connect or listen, TCP and UDP, port ranges, timeouts |
| `iperf` | measure throughput against an `iperf` server, or act as one |
| `telnet` | negotiates the options that a real server requires |
| `ssh` | Dropbear's dbclient, public-key authentication. The ReadMe covers keys |
| `NetCapture` | capture what is on the wire to a `.pcap` file that Wireshark and tcpdump open, filtered by host, port or protocol |
| `NetTrace` | the same file for a transfer it runs itself, with the throughput number beside it |
| `traceroute` | trace the path to a host |
| `tftp`, `whois` | small TFTP and WHOIS clients |
| `CheckNetConfig` | read the configuration and report what is wrong with it |
| `CheckNetDevice` | what `anxnet.device` found, card by card, and why any card was refused |
| `AddNetRoute`, `DeleteNetRoute` | where packets go that are not for this network |
| `GetNetStatus`, `NetShutdown` | status for scripts, and a clean shutdown |
| `RemoveNetInterface` | take one interface out of the running network |
| `ConfigureNetInterface` | change the address of a running interface, or renew and release its DHCP lease |
| `hostname` | the name of this machine, and where the name came from |

The installer copies all of them into `C:`.

## A Shell in a web browser

`httpd -T` serves an AmigaDOS Shell at **`http://<address>/shell`**, beside the
drawer that it already shares. The installer offers to start both when the
machine boots.

It is a real console, not a pipe. `Ed` and `More` work, and so do the cursor
keys and the history. A password typed at the prompt of `ssh` is not drawn on
the screen. A program that asks how big the window is gets an answer. On an
A1200 the prompt appears in 44 ms, and a press of Return shows the output about
23 ms later. That is comparable to a Shell on the machine itself.

**There is no password.** Anyone who can reach the port gets the Shell, exactly
as they get the drawer.

## Finding the machine by name

The Amiga answers to **`<hostname>.local`** on the local network. Another
machine can therefore reach it by name with no DNS server and no configuration.
`.local` names work from the Amiga in the other direction too. This matters
most when there is no DHCP server at all: the machine gives itself an address
and is still reachable.

The responder is per interface, and it is off until it is asked for, because it
costs time on a slow machine. The installer asks for it. The setting is
`MDNS=YES` in `DEVS:NetInterfaces/<name>`, or `ConfigureNetInterface <name>
MDNS=YES` on an interface that is already up. A `.local` lookup needs the same
setting. With no interface that asks for it, nothing is started in either
direction.

A lookup needs no separate command. `ping`, `host`, `fetch` and any older
program that resolves a name all get it, because the lookup happens inside the
resolver that everything already uses.

The same machinery finds what everything else is offering. `ShowNetServices`
with nothing after it lists the kinds of service that answer on the network. A
named kind lists the machines behind it with their addresses and ports. `ALL`
lists every instance of every kind:

```
ShowNetServices
ShowNetServices _http._tcp
ShowNetServices ALL
```

Printers, NAS boxes, media players and anything that runs Bonjour or Avahi
appear without being configured anywhere. The list is what this machine has
heard recently, not an inventory of the network. Nothing on a `.local` network
can say when the answers have stopped coming, so something listed can since have
gone.

## HTTPS / SSH

`fetch` handles `https://` URLs. Certificates are checked against the usual set
of root authorities.

`ssh` needs no special configuration at the far end. The prompt appears after
about ten seconds. Almost all of that time is the cryptography, not the network.

Keys must be in **Dropbear's own format**. An OpenSSH key copied straight across
is not read. Name the key on the command line with `-i`. Generate the key on a
PC rather than on the Amiga, which has neither the entropy nor the speed for it.
The ReadMe in the archive has the exact commands.

The first connection to a site takes twenty seconds or more, and some servers
give up before it finishes. The second attempt to the same site takes less than
a second, because the session is kept on disk and survives a reboot.

## Building from source

**[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)** covers the build, its options, the
test suites, continuous integration and the measurement method.

## Licence

AmiNetXDuo is an independent implementation of a published ABI. No
implementation code from AmiTCP, AROSTCP, Miami or Roadshow has been used,
copied or disassembled.

The ABI itself comes from interface definitions and documentation that exist to
be read. Those sources are named here:

- Olaf Barthel's freely distributable Roadshow SDK headers and autodocs
- the NDK's `pragmas/bsdsocket_pragmas.h`
- the `.fd` and `.sfd` function-descriptor files that AmiTCP and Roadshow
  publish

For example, the 39 vectors of `usergroup.library` were settled by a comparison
of AmiTCP's `fd/usergroup_lib.fd` with Roadshow's `sfd/usergroup_lib.sfd` and
the NDK pragma, all three agreeing.

MIT. ThreadX and NetX Duo are MIT-licensed as well (© Microsoft and the Eclipse
ThreadX contributors). **Both are maintained forks.** The ThreadX fork at
`github.com/tinic/threadx` is pinned one commit beyond upstream `44d7c95c`; its
standalone change makes hosted-port stack-build failure observable to generic
thread create/reset code and fixes stack-range overlap detection. The NetX Duo
fork carries one patch per defect. Each patch sits on its own branch off
upstream `473d1928`, and each is written as a standalone change. Its branches
are at `github.com/tinic/netxduo`, and the engineering record states what each
one fixes. The one further exception is the CA root set in
`DEVS:Internet/certificates`. It is Mozilla's, under MPL 2.0, file-scoped, and
affects nothing else here.
