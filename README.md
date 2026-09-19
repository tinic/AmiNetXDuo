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
PCMCIA cards and the 3C589. `anxgenet.device`, the same driver core with only
the Raspberry Pi 4/CM4's own Ethernet in it, drives that port behind a
PiStorm32 running Emu68 (`DEVICE=anxgenet.device`, `UNIT=0`). The installer
offers to put both in `DEVS:Networks`; on Emu68 it reads the device tree and
creates definitions for the supported Ethernet and Wi-Fi devices it finds.
An interface file selects one with `DEVICE=`. `anxgenet.device` needs Emu68
1.1 alpha.1 or newer: that is the
first release line that maps GENET's `/scb` range into the Amiga address space.
Emu68 1.0.3 is not supported.

`anxwifipi.device` is the same machine's Wi-Fi: the Pi 4's own Broadcom 43455
on SDIO, behind a PiStorm32 running Emu68 (`DEVICE=anxwifipi.device`,
`UNIT=0`). It is the MPL-2.0 fork of Michal Schulz's WiFiPi.device kept at
`github.com/tinic/WiFiPi.device` (branch `gcc16`, submodule
`third_party/wifipi`), built here with the tree's toolchain, with the
single-copy receive path above and counters behind `S2_GETSPECIALSTATS`. It
needs the Wi-Fi firmware Emu68 installs in `DEVS:Firmware`, and a supplicant
to join a network -- WirelessManager from Aminet's `driver/net/prism2v2` reads
`ENVARC:Sys/Wireless.prefs` and associates through the SANA-II wireless
commands; the interface file names the device as any other. On an A1200 +
PiStorm32 Lite on a 5 GHz network at -69 dBm: 34-36 Mbit/s in, 52-61 out,
6 ms round trips.

**`anxwifipi.device` is experimental.** The card's interrupt line is shared
with the SD card host, and the gic400.library in the Emu68 ROM takes one
server per line, so the driver has no interrupt: a task at the lowest
priority watches the card's status register while frames flow (it takes
whatever CPU is idle during a transfer, and a CPU meter shows it), and a
timer tick looks otherwise -- 0.4% of the machine idle, up to 20 ms on the
first frame of an exchange after a quiet second. The interrupt path needs a
gic400.library with shared lines (`github.com/tinic/emu68-gic400-library`,
branch `shared-lines`) in the ROM; until then the driver is polled, and
association, roaming and power-save behaviour have been exercised on one
access point.

**Receive offload (GRO).** `anxgenet.device` verifies every IPv4 and IPv6
frame's header and TCP or UDP checksum itself, from the sum its copy already
produced, and marks each TCP segment that continues the one before it. The
stack skips its own checksum pass on a verified frame and hands TCP one
segment where the wire carried up to sixteen, the receive side of a
large-receive offload; it does the same for IPv6. A 1 Gbit/s GENET behind a
PiStorm32 Lite receives 900 Mbit/s this way and sends 580 (iperf, 2026-09-19;
142 and 65 before the offload). The two SANA-II
extensions that carry it are published for any driver to implement,
`Developer/include/aminetxduo/anxs2ext.h`. They are negotiated additions to
SANA-II, not a replacement network API: a driver may offer VERIFIED checksum
results without implementing CONTINUES/GRO, and an ordinary SANA-II driver
uses the unchanged receive path.

### Measured on

What the numbers in this file come from, and what CI boots. A card or a
machine not in this table is not known to fail; it is not known.

| Machine | Kickstart | Card and driver | Measured |
|---|---|---|---|
| A1200 + PiStorm32 Lite, Emu68 1.1 | 3.1 | GENET, `anxgenet.device` | 900 in / 580 out Mbit/s |
| A1200 + PiStorm32 Lite, Emu68 1.1 | 3.1 | 3Com 3C589 PCMCIA, `anxnet.device` | 8.3 Mbit/s |
| A1200 + PiStorm32 Lite, Emu68 1.1 | 3.1 | Broadcom 43455 Wi-Fi, `anxwifipi.device` | 34-36 in / 52-61 out Mbit/s, 5 GHz at -69 dBm |
| A3000, 68030/25 | 3.9 | X-Surf 100 (Zorro III), `anxnet.device` | 3.9 in / 3.0 out Mbit/s |
| Amiberry, A1200 (68020) | 3.1 | A2065, `anxnet.device` | 4.8 in / 4.4 out Mbit/s |
| Amiberry, A3000 (68030) | 3.1 | X-Surf 100 (Zorro III), `anxnet.device` | 30 in / 31 out Mbit/s |
| Amiberry, A600 (68000) | 2.05 | NE2000 PCMCIA and `cnet.device` | boots, DHCP, transfers (CI) |
| Amiberry, A500+ / A2000 (68000) | 2.04, 3.1 | A2065, Ariadne II | boots, DHCP, transfers (CI) |
| Amiberry, 68060 | 3.1 | A2065 | builds and boots (CI arm) |

Every CI run boots Kickstart 2.04, 2.05 and 3.1 guests on 68000 and 68020
and runs DHCP, TCP and UDP transfers, `Online`/`Offline`, the installer and
the card-eject path; the two real machines above run the release archive
daily. Roadshow and AmiTCP_NG coexist on the same disk, selected at boot
(`SYS:Stacks`).

## Installing

Download the `.lha` from [Releases](https://github.com/tinic/AmiNetXDuo/releases),
unpack it, and run `Install-AmiNetXDuo`. It asks:

| Question | Default |
|---|---|
| Which stack: Everything, Minimal, Micro | Everything |
| Install `anxnet.device` | yes |
| Where the `AmiNetXDuo` drawer of documentation and examples goes | any drawer; nothing in it is needed for the network |
| Into the system (`LIBS:`, `C:`, `DEVS:`), or into its own drawer, added last to those assigns; refused if `LIBS:` already has a `bsdsocket.library` (Intermediate and Expert) | into the system |
| Which card, and the interface name | the driver found in `DEVS:` |
| Answer to `<hostname>.local` on the network | no |
| DHCP, or a fixed address, netmask and gateway | DHCP |
| The machine's name | `amiga-` and the card's last three address octets |
| Start the network at boot | yes |
| Start `httpd` at boot, exposing mounted volumes with a browser file manager and Shell, on which port | no |

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
| `httpd` | expose mounted volumes over HTTP and WebDAV, so other machines mount them as one drive. `-F` adds a browser file manager, `-T` a Shell, and `-C` the machine's display |
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

The installer copies all of them into `C:`. Every command that resolves a name
takes `-4` and `-6`.

## Files, a Shell and the display in a web browser

`httpd -F` serves a file manager at **`http://<address>/files`**. It browses the
mounted volumes and opens, uploads, creates, renames and deletes files and
drawers through the same WebDAV interface that desktop file managers mount.
Its CodeMirror editor provides line numbers, undo and redo, search, and
Ctrl/Cmd+S saving without leaving the browser. Saves carry the file's ETag,
so a copy changed after it was opened is refused instead of overwritten.
It detects extensionless Amiga text such as `startup-sequence`, while keeping
binary files as downloads.
Nothing is fetched from the Internet; the whole page is about 107 KB compressed.
Starting `httpd` with an explicit drawer, such as `httpd Work:Public 8080 -F`,
keeps the older restricted-share form and exposes only that drawer.

`httpd -T` serves an AmigaDOS Shell at `/shell`, beside the volumes that it
already shares. `httpd -C` serves the machine's display at `/console`, chipset
and RTG screens alike. The installer offers to start the volume server, file
manager and Shell when the machine boots.

The Shell is a real console, not a pipe. `Ed` and `More` work, and so do the
cursor keys and the history. A program that asks how big the window is gets an
answer. On an A1200 the prompt appears in 44 ms, and a press of Return shows
the output about 23 ms later.

**There is no password.** Anyone who can reach the port gets the Shell, the
display and every mounted volume.

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

The whole of AmiTCP's `socket_lib.fd` (45 vectors) is implemented. Of
Roadshow's 125, 21 answer `ENOSYS`: the tunables API (`ObtainRoadshowData`
and friends), the IP filter/NAT API (`ipf_*`) and the kernel-memory API
(`mbuf_*`); `SBTC_HAVE_*` reports each truthfully, and
[`docs/GAPS.md`](docs/GAPS.md) lists every one with what a program loses.
`crypt()` is a stub: a ported server that authenticates against `passwd`
cannot. A survey of 6,627 Aminet archives
([`docs/aminet-survey`](docs/aminet-survey)) found no caller of any of the
21.

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
| The `/files` text editor | CodeMirror 6 and its support packages, MIT | bundled into `src/tools/web/files.html`; notice in `src/tools/web/vendor/codemirror` |
| The CA root set | MPL 2.0, Mozilla's, file-scoped | `DEVS:Internet/certificates`, from `third_party/cacert` |
| `anxwifipi.device` | MPL 2.0 (Michal Schulz's WiFiPi.device, forked; ISC Broadcom/OpenBSD headers and an Apache-2.0 register header inside it, listed in its NOTICE). File-scoped: its sources are the submodule's and are published there | `third_party/wifipi`, built by `src/wifipi/CMakeLists.txt` |
