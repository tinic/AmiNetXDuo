[![CodeQL](https://github.com/tinic/AmiNetXDuo/actions/workflows/github-code-scanning/codeql/badge.svg)](https://github.com/tinic/AmiNetXDuo/actions/workflows/github-code-scanning/codeql)

# AmiNetXDuo

An IPv4+IPv6 TCP/IP stack for classic AmigaOS, with the network commands that go
with it. It provides `bsdsocket.library`, the socket API that Amiga network
software already speaks, on top of
[Eclipse ThreadX NetX Duo](https://github.com/eclipse-threadx/netxduo). It
drives existing SANA-II network cards, and ships a SANA-II driver of its own.

> It gets a DHCP lease, configures itself by SLAAC, answers ARP and neighbour
> discovery, and pings its gateway. It resolves DNS, moves TCP in both
> directions, does HTTPS, and accepts incoming connections from other
> machines. It scores **142 of 142** on the independent
> [`bsdsocktest`](https://github.com/tbdye/bsdsocktest) conformance suite.
> `ssh` and `scp` run on it, and so do NFS mounts and ixemul programs.

## Why

AmigaOS has never included a TCP/IP stack. Networking has always come from a
third-party shared library, and each existing option has a limitation. AmiTCP
3.0b2 is free and dates from 1994. AmiTCP 4.x and Miami are proprietary and
effectively unobtainable. Roadshow, the one modern stack, is commercial and
closed.

AmiNetXDuo is MIT-licensed, with the exceptions named under Licence. It speaks
the same socket API and reads the same configuration files as Roadshow, so
existing software and existing habits carry over. It also speaks **IPv6**: on
a network with an IPv6 router, a machine picks up an address without being
configured for one.

## Requirements

Any 68000 or better, AmigaOS 2.04 (Kickstart 37) or newer, 1 MB of RAM, and a
SANA-II network card. One binary serves every 68k: the library picks the code
for the processor when it opens, and nothing has to match the machine.

The archive carries three stacks. The installer asks which one to put on.

| Stack | Leaves out | RAM while up |
|---|---|---|
| Full | nothing | 426 KB |
| Minimal | IPv6, `.local` lookups, the packet filter, HTTPS, IPv4 multicast, the ARexx host, `TCP:` | 256 KB |
| Micro | all of that, and carries one interface and no route or address-allocation calls. DHCP works | 212 KB |

The installer offers these cards by name: A2065, Ariadne, Ariadne II,
AmigaNet, LAN Rover, X-Surf, X-Surf 100, PCMCIA (`cnet.device`,
`etherlink3.device` for the 3Com 3C589, or `prism2.device`) and `uaenet.device`
for emulators. Any other driver name can be typed in.

`anxnet.device`, the SANA-II driver AmiNetXDuo builds itself, drives the A2065,
the Ariadne, the Ariadne II, the X-Surf, the X-Surf 100, NE2000-compatible
PCMCIA cards and the 3C589. The installer offers to put it in `DEVS:Networks`;
an interface file selects it with `DEVICE=anxnet.device`.

## Installing

Download the `.lha` from [Releases](https://github.com/tinic/AmiNetXDuo/releases),
unpack it, and run `Install-AmiNetXDuo`. It asks:

| Question | Default |
|---|---|
| Which stack: Everything, Minimal, Micro | Everything |
| Install `anxnet.device` | yes |
| Where the `AmiNetXDuo` drawer of documentation and examples goes | any drawer; nothing in it is needed for the network |
| Into the system (`LIBS:`, `C:`, `DEVS:`), or into its own drawer selected at boot by `ActivateAmiNetXDuo` beside another stack's files (Intermediate and Expert) | into the system |
| Which card, and the interface name | the driver found in `DEVS:` |
| Answer to `<hostname>.local` on the network | no |
| DHCP, or a fixed address, netmask and gateway | DHCP |
| The machine's name | `amiga-` and the card's last three address octets |
| Start the network at boot | yes |
| Start `httpd` at boot, sharing a drawer with a browser file manager and Shell, on which port | no |

The configuration follows the layout of Roadshow: `DEVS:NetInterfaces/<name>`
defines an interface, `DEVS:Internet/routes`, `DEVS:Internet/name_resolution`
and the `/etc`-style netdb files hold the rest. **A definition does not start
its interface.** `S:Network-Startup` does, one `AddNetInterface` line per
interface, and `S:User-Startup` runs that file. An installation from 0.26.x or
earlier needs that one line added.

The archive also holds `Docs/AmiNetXDuo.guide`, the manual; `Examples/`, a
commented copy of every configuration file; and `Developer/`, the headers that
the NDK does not declare and the CPU profiler.

## Commands

| | |
|---|---|
| `NetSetup` | ask the questions for one interface, and write the configuration files |
| `AddNetInterface`, `RemoveNetInterface` | start an interface from its file, and take one out of the running network |
| `Online`, `Offline` | put a started interface on the wire and take it off |
| `ConfigureNetInterface` | change the address or MTU of a running interface, renew or release its DHCP lease, turn `.local` answering on |
| `ShowNetStatus`, `netstat` | interface state, addresses, routes, connections, multicast groups, counters |
| `ShowNetServices` | what else on this network is offering something |
| `ping`, `host`, `nslookup` | reachability, name lookups, and one kind of record from a named server |
| `arp` | the machines on this network that have answered, IPv4 and IPv6 |
| `sntp` | set the clock from a time server |
| `fetch` | retrieve an `http://` or `https://` URL |
| `httpd` | share a drawer over HTTP and WebDAV, so other machines mount it as a drive. `-F` adds a browser file manager, `-T` a Shell, and `-C` the machine's display |
| `nc` | connect or listen, TCP and UDP, port ranges, timeouts |
| `iperf` | measure throughput against an `iperf` server, or act as one |
| `telnet` | negotiates the options that a real server requires |
| `ssh`, `scp` | Dropbear's clients, public-key authentication, `scp` in both directions and recursive |
| `NetCapture` | capture what is on the wire to a `.pcap` file, filtered by host, port or protocol |
| `NetTrace` | the same file for a transfer it runs itself, with the throughput number beside it |
| `traceroute`, `tftp`, `whois` | trace the path to a host, and small TFTP and WHOIS clients |
| `CheckNetConfig` | read the configuration and report what is wrong with it |
| `CheckNetDevice` | what `anxnet.device` found, card by card, and why any card was refused |
| `AddNetRoute`, `DeleteNetRoute` | where packets go that are not for this network |
| `GetNetStatus`, `NetShutdown` | status for scripts, and a clean shutdown |
| `hostname` | the name of this machine, and where the name came from |
| `ActivateAmiNetXDuo` | select the stack's own drawer at boot, in the drawer layout |

The installer copies all of them into `C:`. Every command that resolves a name
takes `-4` and `-6`.

## Files, a Shell and the display in a web browser

`httpd -F` serves a file manager at **`http://<address>/files`**. It browses the
shared drawer and downloads, uploads, creates, renames and deletes files and
drawers through the same WebDAV interface that desktop file managers mount.
Nothing is fetched from the Internet; the whole page is about 6 KB compressed.

`httpd -T` serves an AmigaDOS Shell at `/shell`, beside the drawer that it
already shares. `httpd -C` serves the machine's display at `/console`, chipset
and RTG screens alike. The installer offers to start the drawer, file manager
and Shell when the machine boots.

The Shell is a real console, not a pipe. `Ed` and `More` work, and so do the
cursor keys and the history. A program that asks how big the window is gets an
answer. On an A1200 the prompt appears in 44 ms, and a press of Return shows
the output about 23 ms later.

**There is no password.** Anyone who can reach the port gets the Shell, the
display and the drawer.

## Finding the machine by name

The Amiga answers to **`<hostname>.local`** on the local network, so another
machine reaches it by name with no DNS server and no configuration. `.local`
names resolve from the Amiga in the other direction too. This matters most
when there is no DHCP server at all: the machine gives itself an address and is
still reachable.

The responder is per interface and off until asked for, because it costs time
on a slow machine. The installer asks. The setting is `MDNS=YES` in
`DEVS:NetInterfaces/<name>`, or `ConfigureNetInterface <name> MDNS=YES` on an
interface that is already up. The minimal and micro stacks do not carry it.

`ShowNetServices` lists the kinds of service that answer on the network; a
named kind lists the machines behind it with their addresses and ports; `ALL`
lists every instance of every kind. Printers, NAS boxes, media players and
anything that runs Bonjour or Avahi appear without being configured anywhere.

## HTTPS and SSH

`fetch` handles `https://` URLs. Certificates are checked against Mozilla's
root set. The cryptography is in `tls.library`, a separate library with its own
SDK that also runs on Roadshow, AmiTCP and Miami, and that handles the server
side of a handshake and ALPN as well.

A first handshake is the cryptography, not the network: about 4 to 9 seconds
on a 68020, a minute or two on a 68000. A second connection to the same site
takes under a second, because the session is kept in
`DEVS:Internet/tlssessions` and survives a reboot. The minimal and micro stacks
do not install `tls.library`.

`ssh` needs no special configuration at the far end and reaches the prompt in
about five seconds on an A1200. Keys must be in **Dropbear's own format**; an
OpenSSH key copied straight across is not read. Name the key with `-i`, and
generate it on a PC, which has the entropy and the speed for it. The manual
has the exact commands.

## Existing software

Software that resolves a name with `getaddrinfo()` and connects to whatever it
returns works over IPv6 without being changed. Software written for IPv4 keeps
working over IPv4. A program is IPv4-only when it resolves with
`gethostbyname()`, keeps an address in 32 bits, or reads a dotted quad out of a
configuration file; for such a program the change is to resolve with
`getaddrinfo()` and hand the result to `connect()`.

`usergroup.library` reads an existing AmiTCP 4 `passwd` and `group` as they
stand. `AmiTCP:` is assigned, so ixemul programs find what they open by path.
NFS mounts with `ch_nfsc` authenticate. Up to four interfaces are online at
once, and the first one named to `AddNetInterface` owns the default route.

## Building from source

**[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)** covers the build, its options,
the test suites, continuous integration, the measurement method and the CPU
profiler.

## Licence

AmiNetXDuo is an independent implementation of a published ABI. No
implementation code from AmiTCP, AROSTCP, Miami or Roadshow has been used,
copied or disassembled. The ABI comes from interface definitions and
documentation that exist to be read: Olaf Barthel's freely distributable
Roadshow SDK headers and autodocs, the NDK's `pragmas/bsdsocket_pragmas.h`,
and the `.fd` and `.sfd` function-descriptor files that AmiTCP and Roadshow
publish.

MIT, with these exceptions, each confined to the files it names:

| What | Licence | Where |
|---|---|---|
| ThreadX and NetX Duo | MIT (Microsoft and the Eclipse ThreadX contributors). Both are maintained forks: `github.com/tinic/threadx` is four commits past upstream `44d7c95c` (hosted-port stack creation and overlap checks, and a pre-relinquish port hook); `github.com/tinic/netxduo` is an integrated line of patches past upstream `473d1928`, pinned by the submodule, with each defect's patch also on its own branch | `third_party/threadx`, `third_party/netxduo` |
| The NE2000 core of `anxnet.device` | BSD-2-Clause, adapted from NetBSD's `dp8390` driver | `src/netdev/dp8390.c`, `ne2000.c`, `ed.c`, `netdev_mcaf.c` |
| `ssh` and `scp` | Dropbear's MIT-style licence | `third_party/dropbear`, `clients/dropbear` |
| The CA root set | MPL 2.0, Mozilla's, file-scoped | `DEVS:Internet/certificates`, from `third_party/cacert` |
