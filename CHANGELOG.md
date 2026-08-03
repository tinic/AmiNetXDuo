# Changelog

User-visible changes, newest first. Internal work is in the git log.

New entries go under `Unreleased` and nowhere else. A version heading below it
has shipped and is history; three entries landed in one during 2026-08-01 and
had to be moved out, because a branch started before a release still shows that
version at the top when it merges.

## Unreleased

- An IPv6 connection through a router with a narrower link than the local one now works. The stack listens to the router's report of how much it can carry and sizes its packets to it, having previously ignored the report and sent packets that could not get through. A report claiming an implausibly small size is refused rather than believed, and a report is only accepted from a machine actually being addressed, so a stranger cannot slow a connection down
- `MTU=` in `DEVS:NetInterfaces` had no effect and now does, downwards from whatever the driver reports
- Ctrl-C stops a name lookup, and a lookup that cannot be answered gives up when its timeout says to. A 30 second timeout was being applied to each attempt in turn rather than to the whole call, so a name server that never replies held the program for over two minutes with one server configured and closer to thirteen with five, and nothing could interrupt it
- Looking up a name while another program is looking one up no longer reports the name as not existing
- A lookup that failed because no server could be reached is now distinguishable from one that failed because the name does not exist

## 0.16.8

- Reading is faster on a 68030, and how much depends on the machine. On an emulated A3000 a 4 MB transfer went from 796 to 1714 KB/s; a 68020 is unchanged and a 68000 is a couple of percent slower, because on those the machine itself is the limit and not the network. Three changes together: the stack now tells the far end which segments arrived after a gap, tells it when a segment arrived twice so it can undo a needless retransmission, and measures the retransmission timeout instead of assuming one second
- A certificate chain is refused unless every issuer in it is marked as a certificate authority. Without that check, anyone holding an ordinary certificate from a trusted root could sign one for any name and it would be accepted
- Three faults found by fuzzing, each reading one byte past the end of a message: an ASN.1 tag, a ServerHello, and a CertificateRequest. All three were fixed on 31 July and had not reached a released build until now
- A second TLS connection opened while the first is busy no longer risks reading the first one's data. Two files this project keeps its own copy of had missed the fix
- `ping fileserver` now tries `fileserver.your.domain` when the short name is not found. The domain a DHCP server supplies was asked for and then discarded, so a machine addressed by DHCP had no default domain at all
- `ShowNetStatus` says where the host name came from -- the interface file, DHCP, `ENV:HOSTNAME` or nowhere. A stale `ENV:HOSTNAME` that outranks a newer setting is now visible instead of puzzling
- An interface's `ID=` is used as the host name when nothing else sets one
- `fetch` no longer writes the server's real response into the file as though it were the body when the server sends an interim reply first, and follows a redirect to a relative address instead of trying to resolve it as a host name. It also sends the port in the `Host:` line, so a server on a non-standard port gets the right site
- A `group` file with Mac line endings no longer corrupts memory as it is read
- `IPV6_MULTICAST_HOPS` of 0 keeps the datagram on this machine instead of putting it on the wire, and a raw IPv6 socket can no longer set the checksum offset and get the V6ONLY flag instead
- The user guide is in the archive. `Docs/` has been shipping with nothing in it but the ReadMe since the guide moved in July, and the installer's own final page tells the reader to look there
- The smallest 68000 build is compiled on every CI run. It ships, and until now it was first compiled during the release job

## 0.16.7

- Reading is back to the speed it was at 0.16.4. A change in 0.16.6 stopped the stack sending the duplicate acknowledgment that tells the far end a segment is missing, so every lost segment waited for a timeout instead of being resent immediately. Reads on a 68020 with a real card fell from 395 to 242 KB/s and on a 68000 from 125 to 102; writes were unaffected. The receive window that release also widened went back to what it was, having been measured as worth nothing once the acknowledgments work
- `httpd` serves a drawer over read-write WebDAV, so a drawer on this machine can be written to as well as read from Windows, macOS and Linux -- files and drawers can be created, renamed, copied and deleted from the far end's own file manager. Deleting or copying a large tree no longer stops the server answering anyone else while it runs
- A file whose name is longer than the filesystem accepts is refused instead of being silently shortened. On a floppy that cuts at 30 characters, two names differing only after the thirtieth were the same file, and writing the second replaced the first

## 0.16.6 -- withdrawn

This release was taken down. The read speed below was measured on a test rig that reorders packets, where the change responsible was rewarding the loss recovery it had switched off; on real hardware reads got slower, not faster. 0.16.7 puts it back. The entries are kept as a record of what was claimed.

- Reading is about twice as fast. A 4 MB file off a file server on a 14 MHz 68020 went from 979 to 1953 KB/s. The receive window was exactly 32768 bytes, which cannot hold a 32 KB block of application data plus the header in front of it, so every block arrived in two instalments and each instalment cost a full round-trip wait. It is now 33 whole Ethernet segments. A program holding five or more sockets at once was never affected and does not change
- Out-of-sequence data is acknowledged after it is queued rather than before, so a segment that closes a gap is no longer reported as though the gap were still open. The far end was retransmitting data that had already arrived

## 0.16.5

- A `send()` that takes only part of what it was offered now reports the part it took. A program that resent the remainder was sending some of it twice, which showed on large transfers and not on small ones
- The Developer drawer's `Profile` names functions inside `bsdsocket.library`, so a profile of an ordinary program shows where the stack spent its time rather than one bar for the whole library. A library it cannot read is still named, as before
- `httpd` serves a drawer over HTTP and read-only WebDAV, so this machine can be mounted as a drive from Windows, macOS and Linux with nothing installed at the far end -- Finder's Connect to Server, Explorer's Map network drive and the Linux file manager all speak it. `httpd Work:Public 8080`. It answers several clients at once, and it refuses any address that leaves the drawer, including the AmigaOS form `/RAM:` that a check for `..` does not see

## 0.16.4

- TCP transfers are faster. A megabyte over the wire on a 14 MHz 68020 went from 234 to 283 KB/s and over loopback from 610 to 708 KB/s, by taking two costs out of the scheduling underneath the stack rather than out of the protocol: a thread handing work to another thread no longer wakes a third one to do it, and the lock taken around every critical section is no longer a function call. The same work is removed on every processor, but 68020 is where it has been measured. Neither change alters what the stack sends
- A stream read that the already-received data covers no longer takes the lock the stack uses to enter the network kernel. It reached no network state to need it, and a program that reads in small pieces paid for one on every call

## 0.16.3

- DHCP asks for an address under the same client identifier Roadshow uses, so a machine keeps the address and the router reservation it had before the stack was changed. Without it the router treated the same card as a different machine and handed out a different address, which broke every reservation and every note of "the Amiga is at". The request also asks for the domain name and the static route list on a DHCP interface configured in `DEVS:NetInterfaces`, which only an interface configured by hand used to get
- The internet checksum is a quarter faster, and every packet pays it in both directions: 201 to 150 nanoseconds a byte on a 14 MHz 68020, and 24% off it on a 68000. It was the most expensive thing the stack did per byte
- The copy every received frame is handed through is 8 to 13% faster, at all four alignments a card can present it at, and the packet handling built on that copy moves with it

## 0.16.2
- The installer's "no network card driver" message no longer appears before the page it tells the reader to use. It came up first, ended the installation, and advised choosing "Intermediate User" -- on a page that had not been shown yet and could not be reached
- Keeping an existing interface configuration says that it is being kept as-is and not checked, so a card or driver that has changed since it was written is not silently assumed to still be there
- The installer asks which build to install instead of only detecting one. The processor it finds is still the default and a novice install is unchanged, but a disk being prepared on one machine for another can now be given the right library. The choice names what the smallest build leaves out
- `ShowNetStatus` reports which stack is running and which build of it, and `GetNetStatus VERSION` prints the same for a script. Both report the LIBRARY's version rather than their own: `C:` and `LIBS:` are updated separately, so a machine can have new commands over an older library and the one in memory is the one worth knowing about. Neither starts the network to find out
- Every file says which release it is from. `Version full file C:ping` reads `ping 0.16.2 (1.8.2026) AmiNetXDuo <commit>`, and `bsdsocket.library` answers at all -- it carried no version string before, so there was no way to tell an installed copy apart. One number for the whole set instead of a private one per command, the date from the build rather than from whoever last edited the file, and the commit so two builds of the same release can be told apart. The name is in there because Roadshow ships commands called `AddNetInterface`, `ping`, `arp` and `netstat` too
- `STATE=down` on the only interface no longer stops the network library opening. It could not be undone from the machine it happened on: nothing could open the library, so there was no `Online` to bring the interface up with and no `ShowNetStatus` to see it -- editing the interface file was the only way out
- `AddNetInterface` on an interface configured down says so, instead of advising a check of the cable
- `NetStackQuery()` and `NetStackControl()` are published, at -0x366 and -0x36c, so a third-party `netstat` or `ifconfig` can be written. They are what `ShowNetStatus`, `netstat` and `arp` are built on and nothing else. `aminetxduo/netstatus.h` joins the Developer drawer, and a caller checks `lib_Revision` against `AMI_NETSTATUS_MIN_REVISION` before calling. Published means fixed: `NetStatusHeader` and every `NETCTRL_*` request structure are part of the interface from here on
- Correction to 0.16.0: that release listed "Closing a TLS connection while another program is using one no longer risks reaching through the closed one". The change is real and stays, but it hardens something a program could not actually reach -- both writers of the connection registry already hold `Forbid()` across the whole update, and every lookup is a task asking for its own connection. It should not have been listed as a fault anyone could meet

## 0.16.1

- `AddNetInterface` that cannot bring an interface up gives the machine its memory back. A failure after the card had opened -- a PCMCIA card in the slot that will not initialise, a cable that is not there, no DHCP answer -- left the network running with nobody using it, and the memory it holds was gone until the machine was switched off. Measured at 580,704 bytes on a machine with 8 MB free, and about 400 KB on a 1 MB one, which is most of what such a machine has
- `AddNetInterface` no longer says an interface came up when it did not. After one failure of the kind above, every later run reported success against the network the failed one had left behind, so a machine with nothing on the wire looked configured
- The network can be started again after a failed attempt. Once one had failed, the count of who was using the stack could never reach zero, so it could not be taken down and could not be restarted; a reboot was the only way out
- A card that is fitted and will not answer is no longer reported as a missing driver. `AddNetInterface` said "There is no cnet.device on this machine" at a machine whose cnet.device had just opened, because it looked for the driver file in four directories instead of asking the card. It now asks, and says that the driver and the unit number are not what to look at
- `AddNetInterface` gives back the library it opened when it finds another TCP/IP stack installed

## 0.16.0

- Two `BeginInterfaceConfig()` calls for different interfaces at the same time no longer risk one of them never being answered, which left the program that made it waiting on its message forever
- Starting a packet capture on a machine where the network had not yet read the clock no longer stops multitasking for the length of a device open, once for every frame captured
- Opening `usergroup.library` while another program is reading `DEVS:passwd` no longer stops multitasking for the length of that disk access
- `recvmsg()` and `sendmsg()` accept a control buffer declared with `CMSG_BUFFER()`. The macro did not give the buffer the alignment it exists to give it, and the library then refused about half of them -- `MSG_CTRUNC` with no ancillary data on receive, `EINVAL` on send -- depending on where the linker had put it
- `gethostbyname_r()` and `gethostbyaddr_r()` accept a buffer that does not start on a longword boundary. On a 68000 the first write into it was an address error
- `bpf_ioctl()` answers `EINVAL` for a buffer at an odd address rather than taking an address error on a 68000
- New `docs/ALIGNMENT.md`: every unaligned-access site and every thread and process stack in the tree, with the measurements
- Two programs joining a multicast group at the same moment no longer take the same tracking row. The one that lost never left its group when it closed, and the one that won dropped a group it did not join
- `TCP:` can be started again after `Status TCP: DIE`. Once it had been taken down, every program that opened the library afterwards got one with no `TCP:` device until the library was unloaded
- Two programs asking for the time at the same moment no longer collide over one timer request, which could take the clock back to zero and make anything measuring elapsed time see about 49 days
- Two programs configuring different interfaces at the same moment no longer swap each other's requests; the second is told the allocator is busy
- Closing a TLS connection while another program is using one no longer risks reaching through the closed one
- Closing a raw socket after the network has already gone down no longer leaves raw sockets unable to receive for the rest of the session
- A program that calls `WaitSelect()` with a timeout gets its signal bit back when it closes the library. Every open and close of `bsdsocket.library` used to cost the program one of the 32 signal bits it will ever have, so a program that opened and closed the network around each job stopped being able to allocate one after 32 of them
- Closing the network no longer costs a signal bit on the calling task when a socket call had to be abandoned partway through
- Expunging the library gives `timer.device` its open back, as does expunging `tls.library`
- A SANA-II device that will not give back a queued write no longer has the memory it is about to write into freed underneath it, which is what already happened for a queued read
- The raw-framing probe is off unless it is asked for, in a build that does not go through CMake as well as one that does. On a device that ignores `AbortIO()` -- Commodore's `a2065.device` among them -- it never returns
- Fixed three ways a hostile or broken server could read past the end of a buffer during a TLS handshake, and two more in the code underneath it. A certificate two bytes long, a 38-byte ServerHello and a zero-length CertificateRequest each walked off the end of the record buffer; a signature length was checked against the wrong bound; and every RSA modulus tripped a signed shift. On a machine with no memory protection a read past a buffer is not a crash, it is whatever happened to be next
- `ping`, `ShowNetStatus` and `AddNetRoute` give back the memory they read `DEVS:Internet` into. Each run lost about 12 KB until the next reboot, which on a 1 MB machine is roughly seventy runs
- `connect()` from a socket bound to one of the machine's addresses now leaves from that address instead of being refused
- New `Developer` drawer in the archive: the headers and linkable glue for everything this stack offers that the NDK does not declare, so a program built against it can call `if_nametoindex()`, `if_indextoname()`, `if_nameindex()` and `if_freenameindex()`, and name the IPv6 constants, without writing the vector offsets out by hand
- Expunging the library gives all its memory back. An open, close, expunge and reopen cycle used to lose 12,612 bytes of the machine's free memory every time, so repeatedly starting and stopping the network eventually ran it out
- A browsed service whose address did not arrive with it is now asked for, so a row that said "no address" gives one. Only the rows that need it wait, and the whole listing spends at most two seconds on it
- `ShowNetServices ALL` lists every instance of every type answering, rather than only the types. It costs one more listening window, not one per type
- A socket bound to one of the machine's addresses now sends from it. UDP and raw datagrams leave with the bound address as their source, and so does a TCP connection: on a machine with two interfaces, a `connect()` from an address on one of them leaves from that address rather than from whichever one the routing table preferred
- A destination the bound address cannot reach is refused with `ENETUNREACH`, on `connect()` as well as on a datagram. Such a datagram used to be handed to the stack and dropped inside it with the send already reported as successful
- `sendto()` and `sendmsg()` on a raw socket honour an IPv6 zone -- `fe80::1%2` leaves by interface 2 -- as a UDP socket already did
- IPv4 multicast works: `IP_ADD_MEMBERSHIP`, `IP_DROP_MEMBERSHIP`, `IP_MULTICAST_IF`, `IP_MULTICAST_TTL` and `IP_MULTICAST_LOOP`, so a program that discovers things on the local network -- SSDP, UPnP, a ported mDNS -- can open the socket it expects instead of getting "Protocol not available"
- `bind()` to a multicast group address is accepted, which is how a program listening for a group is written
- The `68000-minimal` drawer leaves multicast out along with the other optional features, which is 3,888 bytes
- IPv6 multicast works too: `IPV6_JOIN_GROUP`, `IPV6_LEAVE_GROUP`, `IPV6_MULTICAST_IF`, `IPV6_MULTICAST_HOPS` and `IPV6_MULTICAST_LOOP`, with `struct ipv6_mreq` and the option numbers in the new `aminetxduo/in6.h`. A joined group is delivered to the socket that joined it and dropped when it closes. No Multicast Listener Report is sent -- there is no MLD in this stack -- so a group works on the local segment, which covers the link-local groups mDNS, LLMNR and SSDP use, and a switch that prunes by MLD snooping will not forward a wider one
- `bind()` to an IPv6 group address is accepted, as it already was for IPv4
- A socket bound to a multicast group can send to it. The bound group was treated as the address to send from, which no interface has, so every send from such a socket failed with `EADDRNOTAVAIL` -- and binding the group then sending to it is how an SSDP client is written
- `recvmsg()` can now report which interface and local address a datagram arrived on, and its hop limit: `IPV6_RECVPKTINFO`, `IPV6_RECVHOPLIMIT` and, for IPv4, `IP_PKTINFO` and `IP_RECVDSTADDR`. A server on a machine with more than one address could not previously tell which of them a query was sent to, so it could only answer from whichever the routing table preferred
- `sendmsg()` can name the source address and outgoing interface for one datagram, and `setsockopt(IPV6_PKTINFO)` sets a standing one, so a server can answer on the interface a query came in on. An interface or address the machine does not have is refused rather than quietly replaced
- Raw ICMPv6 sockets take an `ICMP6_FILTER`, so a program watching for one kind of ICMPv6 message is no longer handed every neighbour solicitation on the network as well
- `sendmsg()` can set the hop limit of one datagram with an `IPV6_HOPLIMIT` object, which is what a traceroute over UDP is made of
- `IP_TTL` and `IPV6_UNICAST_HOPS` reach the wire on a UDP socket. They were stored and read back and never applied, so every UDP datagram left with the stack default whatever the program asked for
- `sendmsg()` on a raw socket takes the same source address and interface a UDP one does
- The loopback interface has a name and a number, `lo0`, from `if_nametoindex()`, `if_indextoname()` and `if_nameindex()`. A datagram over `::1` or `127.0.0.1` reports it like any other arrival instead of reporting nothing, and a send can name it
- New header `aminetxduo/cmsg.h` with the structures and `CMSG_*` macros the above needs. The NDK's own `CMSG_NXTHDR` cannot be compiled -- it uses an `ALIGN()` no NDK header defines -- and its `CMSG_FIRSTHDR` returns a pointer where it should return `NULL`; both are replaced, and `CMSG_LEN` and `CMSG_SPACE` added
- `ssh` asks for 8 KB of stack instead of 256 KB, and gives it back. Its deepest measured use is 5,008 bytes; the old figure was a guess made for a program this never built. And every invocation used to lose the whole 256 KB of the machine's free memory until the next reboot
- `ssh` closes `bsdsocket.library` when it finishes, and stops the console reader it started. Neither happened before: both are registered with `atexit()`, and on this compiler's startup code `exit()` ends the program without running anything registered that way. So every `ssh` left the network library with one more user than it had, which is enough to stop it ever being unloaded -- and an interactive session left a reader process running and two of the program's 32 signal bits gone until the machine is switched off. An `ssh` that connects and finishes now costs the machine nothing at all, measured; it used to cost 4,224 bytes

## 0.15.1

- New `Developer` drawer in the archive: the headers and compiler glue for what bsdsocket.library has that the NDK does not declare. `if_nametoindex()`, `if_indextoname()`, `if_nameindex()` and `if_freenameindex()`; and the AF_INET6 names -- `IPPROTO_IPV6`, `PF_INET6`, `INET6_ADDRSTRLEN`, `IPV6_V6ONLY`, `IPV6_UNICAST_HOPS`, `IPV6_TCLASS`, `IN6ADDR_ANY_INIT`, `IN6ADDR_LOOPBACK_INIT`, the `IN6_IS_ADDR_*` macros, `struct sockaddr_storage` and `AI_ADDRCONFIG`. Put its `include` on the compiler's include path, `#include <proto/aminetxduo.h>`, and open `bsdsocket.library` as usual; there is no second library and no link library. `Developer/examples/` has two working programs and `Developer/ReadMe` has the rest -- including the warning that this NDK's `struct sockaddr_in6` keeps its family byte at a different offset from `struct sockaddr_in`, so the two cannot be cast through `struct sockaddr *`
- Two programs adding an interface at the same moment can no longer be given the same one: the slot was picked and then the device opened, which takes long enough for the second to pick it again
- `STATE=down` in `DEVS:NetInterfaces` is honoured. It was read from the file and then ignored, so an interface configured down came up anyway; `Online` brings it up as usual
- `GetRouteInfo()` reports the interface each route belongs to. Static routes and the default gateway reported none at all, and the rest counted from 0 where the convention is to count from 1, so a program matching a route to an interface was off by one
- `ShowNetServices` no longer says the list is what answered just now. It is what this machine has heard recently, and something listed may since have gone

## 0.15.0

- New command `ShowNetServices`: what else on the local network is offering something. With nothing after it, the kinds of service answering; naming one -- `ShowNetServices _http._tcp` -- lists the machines behind it with their addresses, ports and, with `TXT`, their advertised settings. Printers, NAS boxes and media players turn up without being configured anywhere. The list is what answered in a few seconds and says so, since nothing on a `.local` network can say when the answers have stopped
- The AMITCP ARexx host answers `QUERY SERVICES <type|ALL> [<seconds>]` with the same list, one service per line, so a script can loop over what is on the network
- A UDP datagram too large for the interface it would leave by is now refused with `EMSGSIZE` and not sent, as documented. Such a datagram used to be assembled and then discarded inside the stack with the send already reported as successful, or to exhaust the packet buffers first and come back as `ENOBUFS`. The limit is the interface's own: 1472 bytes over a 1500-byte Ethernet, 65507 over loopback
- `accept()` reports `EFAULT` when given an address to fill in without the length that says how much room it has, instead of quietly not filling it in
- `accept()` on a datagram socket reports `EOPNOTSUPP` rather than `EINVAL`, which now means only what it should: a stream socket that never called `listen()`
- `WaitSelect()` accepts an `nfds` larger than the descriptor table and truncates it, as documented. A program that lowered `SBTC_DTABLESIZE` and kept passing `FD_SETSIZE` used to get `EINVAL` every time
- `GetSocketEvents()` sets `errno` when it reports `FD_ERROR`, as documented, and leaves the socket's error code in place for `getsockopt(SO_ERROR)` to read and clear
- `socket()` reports `EPROTONOSUPPORT` for a socket type it does not implement, which is the code its documentation lists
- `AddInterfaceTagList()` leaves an interface bare again, as its documentation says: addressing it is `ConfigureInterfaceTagList()`'s job. This reverses part of 0.14.1, which had made a re-added interface keep its old address and so stopped `BeginInterfaceConfig()` ever running DHCP
- `AddDomainNameServer()` now nests, as its documentation says: two programs can share a name server and the first one to exit no longer stops the other one resolving. `ObtainDomainNameServerList()` reports the real count, and still marks entries that came from `DEVS:Internet/name_resolution` as statically configured
- The default domain is now used: a host name with no domain in it that fails to resolve is tried again with the default domain appended, so `ping fileserver` reaches `fileserver.lan`. It can be up to 255 characters, as documented
- `inet_pton()` accepts only dotted decimal, as documented. It used to share `inet_addr()`'s parser and read `0177.0.0.1` as 127.0.0.1, which is a trap for a program that uses it to tell an address from a name. `inet_addr()` still takes the octal, hex and short forms it always has
- Fixed a read from address zero when an interface was queried while being removed, which produced a garbage interface name on a plain 68000 and an Enforcer hit on a machine with an MMU
- Fixed a query reading an interface's device after it had been freed, in the same window
- Setting an interface address without also setting its netmask no longer reverts a DHCP lease that arrived while the call was running
- `IFQ_State` reports whether the stack will transmit, not whether the cable is in, so an interface configured up with the cable out reads `SM_Up` as its documentation defines it. `IFF_UP` follows the same state and the two no longer disagree
- `ObtainInterfaceList()` returns an empty list when the network is not running, instead of `NULL`, which its documentation reserves for being out of memory
- `AddInterfaceTagList()` and `ConfigureInterfaceTagList()` accept tags they do not act on rather than refusing the whole call, so a caller passing the Roadshow tags now gets its interface
- `SM_Down` now stops the interface transmitting without closing the device, which is what its documentation describes; taking the device offline is `SM_Offline`
- `route delete default` reports `ESRCH` unless the address given is the gateway actually installed, instead of removing whatever gateway was there
- `IFC_Metric` accepts a metric of 0, the value it reads back
- `shutdown()` on a socket that was never connected reports `ENOTCONN`
- `gethostname()` truncates into a short buffer and succeeds, as documented, rather than failing with the buffer untouched. With no name configured it answers `localhost`, from the autodoc, instead of an invented `amiga`
- A BPF channel now belongs to the library base that opened it: it is closed when that base closes, so a program that exits without closing one no longer strands it for the life of the stack, and one program can no longer read, reconfigure or close another's channel
- `SBTC_HAVE_LOCAL_DATABASE_API` answers `TRUE`, which it always should have -- all nine of the network, protocol and service database calls are implemented
- Added a `Libs/68000-minimal` drawer: the same library with every optional feature compiled out, for the smallest machines
- Upgrading the commands without the library, or without rebooting so the new library is the one in memory, now says so. The reporting commands used to describe that half-installed pair as something else -- the stack refusing to report on itself, or a library built without service discovery

## 0.14.3

- Fixed closing the last `bsdsocket.library` handle never returning, so a program that opens the library, uses it and exits no longer hangs
- `AddNetInterface` now says when the network could not start for want of memory, instead of sending you to check the cable

## 0.14.2

- `netstat -h` now reports what the network owns -- allocations, sockets and packet buffers, each with a high-water mark -- so a suspected leak can be confirmed or refuted without a debugger
- It still answers on a machine that has stopped responding, so the numbers can be read from one that is stuck
- `ShowNetStatus MEMORY` reports the fewest packet buffers ever free

## 0.14.1

- Fixed a scheduler-state window that corrupted ThreadX suspension lists and produced Enforcer hits under load
- Fixed `ping` failing with error 9 in non-Release builds, which also affected `bind` and `connect`
- Re-adding an interface now keeps its netmask and restores the default route
- An interface whose device does not open no longer leaves another interface using its address

## 0.14.0

- System clock now derives from the E-Clock, so time stays true when ticks are not delivered
- Timer catch-up defers ticks instead of discarding them, so timers run late rather than never
- Fixed a spurious 100-minute stall report roughly every 280 seconds
- Fixed uptime and tick rate reporting wrong values after 100 minutes
- Added `netstat -h`, reporting scheduler and baton counters without taking the baton
- Added ARexx commands for interface control and status
- Bounded the timer task to 16 ms of each 20 ms period
- `bsd_WaitSelect` stack use cut from 864 to 116 bytes
