# Changelog

User-visible changes, newest first. Internal work is in the git log.

New entries go under `Unreleased` and nowhere else. A version heading below it
has shipped and is history; three entries landed in one during 2026-08-01 and
had to be moved out, because a branch started before a release still shows that
version at the top when it merges.

## Unreleased

## 0.20.1

- `AddNetInterface` adds an interface to a network that is already running. It never did: against a running stack it opened the library, waited for any address the machine happened to have, and reported success without adding anything, so an interface taken out with `RemoveNetInterface` could not be put back until a reboot
- `AddNetInterface` says when it did not work. It now fails when the interface was refused, naming the device, the file or the slot, and warns when the interface attached but has no address; success means an interface that is addressed, or one configured to stay down
- An interface added back after a removal can transmit. Its reader threads were being given memory the library had already registered elsewhere, which ThreadX refused, leaving the interface attached and addressed with its link down
- A program can add an interface through the library, `NETCTRL_INTERFACE_ADD` alongside the existing remove
- An address change no longer signals a program that has exited. A program that asks for `SBTC_SIG_ADDRESS_CHANGE_MASK` and then quits without closing the library used to leave a base behind that the stack still signalled, writing into memory the machine had given to something else; it is now skipped, and named on the serial log
- The commands stop losing 2568 bytes of memory every time they run. Starting the network means keeping the library open, or the stack would go down again with the command; the library holds that reference itself now, so a command closes its own base like any other program. Ten `AddNetInterface` runs cost what one does
- The commands that read the network's state say "network not started" once rather than twice
- `ShowNetStatus` on a machine with no interfaces configured and no drivers installed said what to look at and then listed nothing, which is the machine that heading is for
- `AddNetInterface` says when `LIBS:bsdsocket.library` is missing rather than only that the network would not start
- `ShowNetServices` says when more services answered than it had room to show, and `fetch` on a 68000 says there is no `tls.library` for one rather than that none is installed
- `AddNetRoute` finishes two sentences that stopped mid-clause
- TCP acknowledges by how much data is outstanding rather than by counting segments, with the threshold taken from the receive buffer and ramped up from two full segments as a connection opens. On a 68020 with a PIO card that is 387 fewer acknowledgements for every megabyte read, each one a frame the processor built, the driver sent and the other end handled; reads there are 24% faster than 0.20.0 and now ahead of Roadshow. A small machine is unaffected either way, because free memory sizes the window there long before this does
- A receive window that had drained less than it had filled was advertised as though four gigabytes had come free, on every such read. That is most reads on a machine slower than the other end

## 0.20.0

- Bringing the network up takes 2.5 seconds where it took 8, measured to the point where both an IPv4 and a global IPv6 address are usable rather than to the point where the command returns. IPv6 is now configured before the wait for a lease rather than after it, so the DHCP exchange and the IPv6 work overlap instead of running one after the other, and a boot pays for the longer of the two
- A DHCP lease is taken as soon as the server acknowledges it, and the RFC 5227 probes for a duplicate run alongside it rather than in front of it. The same frames go out; an address in use elsewhere is now found seconds into use rather than ahead of it, and is still declined
- Duplicate address detection sends one solicitation per address instead of three, which is the default RFC 4862 itself gives
- The router solicitation goes out while the link-local address is still tentative, as RFC 4861 allows. Address autoconfiguration no longer waits for duplicate address detection to finish before it can start, and completes about two seconds earlier
- Timer expiration is processed where the tick is delivered. A pending timer used to hold the timer wheel still until a separate task ran, and every tick in between was counted as time but did nothing, so DHCP retransmission backoff, the T1 and T2 lease timers and the stack's own one-second work all ran late under load
- Waiting for an address waits on the notification the stack already had instead of looking fifty times a second. A wait for a DHCP server that never answers cost 1,500 passes over every interface, each one contending with the threads it was waiting for
- `RemoveNetInterface` takes a single interface out of the running network, closing its SANA-II device so the hardware is free and releasing its configuration slot so the same name can be added again. An interface still carrying TCP connections is refused unless `FORCE` is given
- The commands that list interfaces no longer read past the end of their own table when the library reports more interfaces than they have room for, which a half-installed pair of library and command could do
- TCP carries the RFC 1323 timestamps option. Round trip times are now measured from the timestamp the other end echoes rather than inferred from which segment an acknowledgement covers, so a retransmitted segment still yields a sample and the retransmission timeout follows the real path instead of drifting; PAWS rejects a segment whose timestamp has gone backwards. It costs the write direction about 5%, which buys the read direction nothing and loses it nothing, and reads are what a file transfer spends its time on
- TCP offers the RFC 1323 window scale option, so a receive window is no longer capped at the 65535 the field holds. On a local network this changes nothing measurable, because the window was never what limited a transfer there; it is the long or fast path, where a window that small cannot keep the wire busy, that this is for
- A connection that negotiates window scaling now also carries the timestamp and selective-acknowledgement options it agreed to. The scale option ended in a byte that reads as end-of-option-list, so anything written after it was invisible to the other end

## 0.19.1

- The stack's timer now wakes from the vertical blank, and on every second frame rather than every one, instead of issuing a timer.device request for each tick. The clock is still taken from the E-Clock so nothing about timing changes; what goes is the request round trip and half the wakeups, and other work on the machine while the stack is resident runs about 13% faster on a 68000 and 2.5% on a 68020
- Memory the stack allocates asks for Fast RAM before anything else. A packet pool or a thread stack placed in Chip is contended with the chipset, and that cost is paid by everything else on the machine rather than by the stack

## 0.19.0

- A received TCP or UDP checksum is verified as the frame is copied rather than by walking the packet a second time. The SANA-II copy already reads every longword, so it returns the sum it saw; fragments, IPv6 and anything that is not plain TCP or UDP still take the ordinary path. Listed under 0.18.0 but built out of that release
- Handing the stack between an application task and the network threads no longer wakes a scheduler task to do it. Every socket call crossed that boundary twice and paid two context switches each way for work the yielding task can do itself; a bulk transfer is about 2% shorter
- `WaitSelect()` with a timeout no longer starts a timer it does not use. A call that finds a socket already readable never blocks, and was still opening, arming and cancelling a timer.device request every time: loopback throughput rises about 4%
- `select()` clears only the descriptors the caller named rather than all 1024
- Every binary now says which processor it was built for, so `Version full file LIBS:bsdsocket.library` answers it. The archive ships one of each and nothing on the file said which

## 0.18.0

- An `https:` handshake no longer stalls the whole stack while it does public-key arithmetic. The AmigaOS ThreadX port only reschedules at a ThreadX call, and a scalar multiplication makes none, so the IP thread could not answer the network for tens of seconds at a time and the far end gave up and reset the connection. The arithmetic hands the machine back between iterations now
- Copying a received frame is faster on a 68000 when the source and destination disagree in their low address bit. A word or longword access to an odd address is an address error on that part, so the whole copy falls back to a byte loop -- which is eight of the sixteen alignment pairs, not a rare tail
- The 50 Hz timer task stops doing a 32-iteration software divide every tick. It kept its uptime in milliseconds, and converting an E-Clock reading needed a divisor too large for a 68000's `DIVU`, in the highest-priority task on the machine
- **mDNS is off unless an interface asks for it.** Answering `.local` is now `MDNS=YES` in that interface's file in `DEVS:NetInterfaces`, and an interface file that does not mention it gets no responder: on upgrade, a machine that answered `.local` stops until the keyword is added. The responder joins a multicast group on the interface, so every `.local` query any machine on that network sends became work for this Amiga whether or not it concerned this one, and on a 68000 that is felt by everything else. When no interface asks, it is not started at all. The installer asks, and `ShowNetStatus` says which interfaces are answering
- `fetch https://` says when a server gave up on a slow handshake rather than only that the connection closed. Some servers allow about 15 seconds for a handshake and a three-certificate chain costs this hardware more, which looked like a fault in the trust store or the configuration and is neither
- An ssh handshake is several times quicker. The curve25519 field arithmetic is hand-written now, one multiply and one square where the compiler emitted a call per limb: 1.65x on a 68020, 4.47x on a 68060 and 4.74x on a 68000, measured on a real handshake rather than a bench
- `ssh` is installed on a 68000. It was built for the 68020 only and the installer copied nothing below that, on the grounds that it would be slow; the archive carries one build per processor now and the installer picks the one this machine can run
- `https:` works on a 68000. A TLS 1.3 handshake against an RSA-2048 server took 232 seconds at 13 MHz and now takes 60, because the limb multiply-accumulate the handshake spends half its time in is hand-written for the parts with no 32x32 into 64 multiply, which is the 68000 and the 68060 both
- A 68060 TLS transfer costs a third less processor time, from the same multiply-accumulate: 7,473 samples down to 5,108 across two handshakes, with the function itself 4,853 down to 2,491
- A program that closes `bsdsocket.library` and then keeps using it no longer takes the machine down. Each opener gets its own library base, and freeing it meant a later call jumped into whatever had claimed that memory since; the base is kept now and answers such a call with zero. AveNTP does this and could not run at all
- `https:` reaches servers that require TLS 1.3. The record layer read the 1.3 inner content type, one byte, into a two-byte field: on a little-endian host that byte lands in the low half and is the content type, on a 68k it lands in the high half over whatever was in the low half, so every 1.3 handshake ended in an unexpected-message alert having decrypted the record correctly. RSA and ECDSA server certificates both verify
- A TLS transfer costs 40% less processor time. Six loops in the vendored bignum, multiply, square, add, subtract and the modular reduction, were doing by hand what `src/crypto68k` already had in 68020 assembly over the same limb type, and the GCM authenticator ran a byte at a time where a longword does; measured across two handshakes at one clock, 6,557 down to 3,902 samples
- A 68060 no longer traps on the 64-bit divisions in the crypto path. The helper that exists to supply that division was itself emitting `divu.l` in the 64/32 form, which a 68060 does not implement and 68060.library emulates one instruction at a time
- A 68020, 68030 or 68040 multiplies 32x32 into 64 bits with one instruction. It was four 16-bit multiplies on every target, which is what a 68000 needs, and what a 68060 needs because it dropped that form
- Everything the archive installs is stripped now. The three libraries in each drawer, `ssh` and the profiler in `Developer/` each carried a symbol table nothing on the Amiga reads: 13% of `ssh`, 11% of `bsdsocket.library`, 20% of `usergroup.library`, around 200 KB of files in all, and less than that off the download, which was already compressing them. The commands were stripped before this

## 0.17.4

- `ssh` no longer installs on a 68000, where it could only crash. It is built for the 68020 and the whole `C:` drawer was copied to every machine, so a 68000 owner was given it, ran it, and took an illegal instruction. The page that explains why a 68000 gets no encrypted connections now names it too
- `ssh` has a build for the 68060. Its inner loop is curve25519, which carries eight 64-bit `MULU.L` per field multiply and 21,482 field multiplies per handshake; the 68060 dropped that instruction and traps every one of them to 68060.library. The 68060 build has none
- File server reads are close to write speed now. The receiver announced re-opened window space a single segment at a time, so a bulk read was paced by ~1050 window-update round trips per megabyte; announcing at half the receive buffer instead takes a 68020 fitz read from 640 to a steady 1700+ KB/s, writes unchanged

## 0.17.3

- The three libraries are smaller. `bsdsocket.library` drops 10,104 bytes, from 349,244 to 339,144, by collecting the sections nothing reaches and giving internal linkage to the 33 symbols only one file uses

## 0.17.2

- A machine that installed 0.17.0 or 0.17.1 no longer stops part way through its Startup-Sequence. The line the installer writes into `S:User-Startup` runs `AddNetInterface`, which opens `bsdsocket.library`, which brings the stack up and waits for DHCP; the first lease arrived while the library still held the lock that the code answering it wanted, and neither side moved again. A static address was unaffected

## 0.17.1

- The archive is compressed. Every release before this one packed every file whole, so the download was about twice the size it needed to be: 0.17.0 is 86 files at a 100.0% ratio. Nothing else in it differs from 0.17.0

## 0.17.0

- A program that sends to a closed port is told so instead of waiting out its timeout. ICMP error messages never reached the sockets that caused them, so a UDP send to a port with nothing listening blocked until it gave up, and a TCP connection to an unreachable host did the same
- A name ending in `.local` is no longer sent to the name server the router handed out. It leaked on the IPv6 path always, and on every path in the smallest build, which published the names of machines on the local network to whoever runs that server. A reverse lookup of a self-assigned `169.254` address leaked the same way and timed out once per line in `ShowNetStatus` and `netstat`
- A reply to a name lookup is accepted only from the server it was asked of, and only if it carries the question it answers. Either one missing was enough for a forged reply that guessed a sixteen-bit number to be believed and cached
- A cached name is no longer held for sixty-eight years when a server sends a time-to-live with its top bit set
- The address a DHCP server hands out is announced on the network, so other machines reach it at once instead of after their own records expire. If another machine is already using it, that is now reported rather than silently worked around
- A program can ask to be signalled when the machine's address changes, a lease arriving, changing or being lost, rather than polling for it
- Closing a connection is quicker when the last acknowledgement is lost: the far end's repeated goodbye is now answered instead of ignored, where before it retransmitted until its own timer ran out
- A machine that is sent a stream of packets it must refuse no longer answers every one of them, which was a way to make it flood a third party, and answers none at all to a packet sent to a group address
- A transfer no longer stops for good when the far end runs out of room and the message saying it has room again is lost. The stack is meant to keep asking until an answer comes, and in the common case it was not asking at all
- A secure connection is no longer accepted for the wrong machine. The name in a certificate was being read from the wrong field, so a certificate issued for one set of addresses was accepted for another it happened to name
- Certificates signed with MD5 or SHA-1 are refused, as are RSA keys shorter than 1024 bits. This can stop a connection that works today, typically to an old device on the local network; certificates you have placed in DEVS:Internet/certificates yourself are unaffected
- Two certificate authorities that vouch for each other used to make the library loop without end while checking a chain. It now gives up and reports the chain as untrusted
- A server that breaks off a secure connection mid-transfer is reported as an error rather than a normal end of file, so a truncated download no longer looks like a complete one
- Session keys are wiped from memory when a connection closes, and remembered sessions expire after a day on machines with no working clock, where they previously never expired
- A large reply that arrives split into pieces is now put back together instead of vanishing. A machine that could not do this saw big replies simply never arrive, and the program waiting for one timed out with nothing to show for it
- An IPv6 address the router hands out can now be taken back. With one router setting, the address was kept for ever and nothing short of a reboot removed it, so a machine that moved networks kept an address belonging to the old one
- On a network that only speaks IPv6, the machine can now look up names. It previously got an address and could reach numbered addresses but could not resolve a single name
- A name that does not exist is reported as not existing straight away, instead of every name server on the list being asked in turn and the whole retry ladder run out first
- Names spelled with capital letters resolve. Some name servers answer in lower case and the reply was being rejected as if it were for a different name
- A name server answering about one name can no longer slip in an address for a different one, and a reverse lookup checks that the reply is about the address that was asked about
- A name whose address has changed is no longer remembered past the time the server asked for it, even while a program keeps looking it up
- Sharing a folder over the network refuses a request that describes its own length in two contradictory ways, which is how a machine in the middle is made to see a different request than the server does. Some older programs send both and will now be refused
- A file uploaded to the shared folder in pieces is now subject to the same size and time limits as any other, so a handful of very slow uploads can no longer occupy every connection
- Closing a program that was capturing network traffic no longer frees the capture buffer while it is still being read
- A connection request from a network the machine cannot reach, and a stream of packets it must refuse, are both answered at a bounded rate rather than one for one
- Asking to send more than fits in one packet on a raw socket now reports an error instead of reporting success and sending nothing
- A folder shared over WebDAV answers the properties a client actually asked for, instead of all of them every time, and says plainly when it does not keep one that was asked about
- Locking a shared folder now stops anyone else adding or removing files in it, which is what taking the lock was for
- `TCP:` no longer appears as a drive. It was claiming to be a validated disk with a million blocks free, so `Info` listed it and Workbench drew an icon for it. It is a stream, like `CON:` and `PIPE:`, and now says so; `Type TCP:host/port` and everything else that opens it are unchanged
- The commands are about half the size. `ping` was 33,020 bytes and is now 15,860; `host` was 15,728 and is now 9,436. Three things did it: the diagnostics they print are one line instead of a paragraph, they no longer carry a symbol table nothing reads, and six of them were pulling in a C++ memory allocator and the C library's file machinery through a single `atexit()` call none of them needed. Every failure still names its own cause, and the guide has the explanations that used to be printed

## 0.16.9

- `bsdsocket.library` is 16 KB smaller. It was writing diagnostic messages to the serial port on every build, and nothing on an ordinary machine is listening to that port. Reporting a fault now needs a build made for it, which the developer documentation explains
- The smallest 68000 build drops the ARexx host from the `AMITCP` port, taking another 8 KB and an 8 KB stack with it. `WaitForPort AMITCP` still works, so a startup script still waits correctly; what no longer answers is a script that sends commands to the port
- The same build also drops the `TCP:` device, for another 4 KB. Reaching the network as a file, as in `Type TCP:host/port`, needs one of the other builds; everything that opens a socket is unaffected. Altogether the smallest build is 24 KB smaller, 218 KB to 194 KB
- An IPv6 connection through a router with a narrower link than the local one now works. The stack listens to the router's report of how much it can carry and sizes its packets to it, having previously ignored the report and sent packets that could not get through. A report claiming an implausibly small size is refused rather than believed, and a report is only accepted from a machine actually being addressed, so a stranger cannot slow a connection down
- `MTU=` in `DEVS:NetInterfaces` had no effect and now does, downwards from whatever the driver reports
- Ctrl-C stops a name lookup, and a lookup that cannot be answered gives up when its timeout says to. A 30 second timeout was being applied to each attempt in turn rather than to the whole call, so a name server that never replies held the program for over two minutes with one server configured and closer to thirteen with five, and nothing could interrupt it
- Looking up a name while another program is looking one up no longer reports the name as not existing
- A lookup that failed because no server could be reached is now distinguishable from one that failed because the name does not exist
- An address a DHCP server hands out is checked for a machine already using it, and refused if there is one. Bringing the network up takes about three seconds longer as a result, which is the check itself waiting for an answer that should not come
- A server program can now hold the queue of waiting connections it asked for. `listen` accepted a backlog figure and kept one connection regardless, so a second caller arriving before the first was accepted was turned away
- A UDP socket that has named its peer accepts datagrams from that peer only. It was accepting anything sent to its port, from anyone
- `IP_TTL` and `IP_TOS` now reach the wire on TCP and UDP rather than being stored and ignored, and four more options stop reporting a value they never applied
- Multicast options set with a two-byte value are read correctly. A `short` was read as a single byte, so `IP_MULTICAST_TTL` came out as zero and the datagram never left the machine
- Asking for a socket option that belongs to a different kind of socket is refused rather than answered with an invented value
- A connection request sent to a broadcast or multicast address is discarded instead of answered
- An IPv6 router that advertises a prefix as usable but not local now results in a working address, every interface asks for a router rather than only the first, and the stack stops asking on the schedule the standard sets instead of giving up after three tries
- `Info TCP:` no longer prints a name read out of low memory, which appeared as garbage characters

## 0.16.8

- Reading is faster on a 68030, and how much depends on the machine. On an emulated A3000 a 4 MB transfer went from 796 to 1714 KB/s; a 68020 is unchanged and a 68000 is a couple of percent slower, because on those the machine itself is the limit and not the network. Three changes together: the stack now tells the far end which segments arrived after a gap, tells it when a segment arrived twice so it can undo a needless retransmission, and measures the retransmission timeout instead of assuming one second
- A certificate chain is refused unless every issuer in it is marked as a certificate authority. Without that check, anyone holding an ordinary certificate from a trusted root could sign one for any name and it would be accepted
- Three faults found by fuzzing, each reading one byte past the end of a message: an ASN.1 tag, a ServerHello, and a CertificateRequest. All three were fixed on 31 July and had not reached a released build until now
- A second TLS connection opened while the first is busy no longer risks reading the first one's data. Two files this project keeps its own copy of had missed the fix
- `ping fileserver` now tries `fileserver.your.domain` when the short name is not found. The domain a DHCP server supplies was asked for and then discarded, so a machine addressed by DHCP had no default domain at all
- `ShowNetStatus` says where the host name came from, the interface file, DHCP, `ENV:HOSTNAME` or nowhere. A stale `ENV:HOSTNAME` that outranks a newer setting is now visible instead of puzzling
- An interface's `ID=` is used as the host name when nothing else sets one
- `fetch` no longer writes the server's real response into the file as though it were the body when the server sends an interim reply first, and follows a redirect to a relative address instead of trying to resolve it as a host name. It also sends the port in the `Host:` line, so a server on a non-standard port gets the right site
- A `group` file with Mac line endings no longer corrupts memory as it is read
- `IPV6_MULTICAST_HOPS` of 0 keeps the datagram on this machine instead of putting it on the wire, and a raw IPv6 socket can no longer set the checksum offset and get the V6ONLY flag instead
- The user guide is in the archive. `Docs/` has been shipping with nothing in it but the ReadMe since the guide moved in July, and the installer's own final page tells the reader to look there
- The smallest 68000 build is compiled on every CI run. It ships, and until now it was first compiled during the release job

## 0.16.7

- Reading is back to the speed it was at 0.16.4. A change in 0.16.6 stopped the stack sending the duplicate acknowledgment that tells the far end a segment is missing, so every lost segment waited for a timeout instead of being resent immediately. Reads on a 68020 with a real card fell from 395 to 242 KB/s and on a 68000 from 125 to 102; writes were unaffected. The receive window that release also widened went back to what it was, having been measured as worth nothing once the acknowledgments work
- `httpd` serves a drawer over read-write WebDAV, so a drawer on this machine can be written to as well as read from Windows, macOS and Linux, files and drawers can be created, renamed, copied and deleted from the far end's own file manager. Deleting or copying a large tree no longer stops the server answering anyone else while it runs
- A file whose name is longer than the filesystem accepts is refused instead of being silently shortened. On a floppy that cuts at 30 characters, two names differing only after the thirtieth were the same file, and writing the second replaced the first

## 0.16.6, withdrawn

This release was taken down. The read speed below was measured on a test rig that reorders packets, where the change responsible was rewarding the loss recovery it had switched off; on real hardware reads got slower, not faster. 0.16.7 puts it back. The entries are kept as a record of what was claimed.

- Reading is about twice as fast. A 4 MB file off a file server on a 14 MHz 68020 went from 979 to 1953 KB/s. The receive window was exactly 32768 bytes, which cannot hold a 32 KB block of application data plus the header in front of it, so every block arrived in two instalments and each instalment cost a full round-trip wait. It is now 33 whole Ethernet segments. A program holding five or more sockets at once was never affected and does not change
- Out-of-sequence data is acknowledged after it is queued rather than before, so a segment that closes a gap is no longer reported as though the gap were still open. The far end was retransmitting data that had already arrived

## 0.16.5

- A `send()` that takes only part of what it was offered now reports the part it took. A program that resent the remainder was sending some of it twice, which showed on large transfers and not on small ones
- The Developer drawer's `Profile` names functions inside `bsdsocket.library`, so a profile of an ordinary program shows where the stack spent its time rather than one bar for the whole library. A library it cannot read is still named, as before
- `httpd` serves a drawer over HTTP and read-only WebDAV, so this machine can be mounted as a drive from Windows, macOS and Linux with nothing installed at the far end, Finder's Connect to Server, Explorer's Map network drive and the Linux file manager all speak it. `httpd Work:Public 8080`. It answers several clients at once, and it refuses any address that leaves the drawer, including the AmigaOS form `/RAM:` that a check for `..` does not see

## 0.16.4

- TCP transfers are faster. A megabyte over the wire on a 14 MHz 68020 went from 234 to 283 KB/s and over loopback from 610 to 708 KB/s, by taking two costs out of the scheduling underneath the stack rather than out of the protocol: a thread handing work to another thread no longer wakes a third one to do it, and the lock taken around every critical section is no longer a function call. The same work is removed on every processor, but 68020 is where it has been measured. Neither change alters what the stack sends
- A stream read that the already-received data covers no longer takes the lock the stack uses to enter the network kernel. It reached no network state to need it, and a program that reads in small pieces paid for one on every call

## 0.16.3

- DHCP asks for an address under the same client identifier Roadshow uses, so a machine keeps the address and the router reservation it had before the stack was changed. Without it the router treated the same card as a different machine and handed out a different address, which broke every reservation and every note of "the Amiga is at". The request also asks for the domain name and the static route list on a DHCP interface configured in `DEVS:NetInterfaces`, which only an interface configured by hand used to get
- The internet checksum is a quarter faster, and every packet pays it in both directions: 201 to 150 nanoseconds a byte on a 14 MHz 68020, and 24% off it on a 68000. It was the most expensive thing the stack did per byte
- The copy every received frame is handed through is 8 to 13% faster, at all four alignments a card can present it at, and the packet handling built on that copy moves with it

## 0.16.2
- The installer's "no network card driver" message no longer appears before the page it tells the reader to use. It came up first, ended the installation, and advised choosing "Intermediate User", on a page that had not been shown yet and could not be reached
- Keeping an existing interface configuration says that it is being kept as-is and not checked, so a card or driver that has changed since it was written is not silently assumed to still be there
- The installer asks which build to install instead of only detecting one. The processor it finds is still the default and a novice install is unchanged, but a disk being prepared on one machine for another can now be given the right library. The choice names what the smallest build leaves out
- `ShowNetStatus` reports which stack is running and which build of it, and `GetNetStatus VERSION` prints the same for a script. Both report the LIBRARY's version rather than their own: `C:` and `LIBS:` are updated separately, so a machine can have new commands over an older library and the one in memory is the one worth knowing about. Neither starts the network to find out
- Every file says which release it is from. `Version full file C:ping` reads `ping 0.16.2 (1.8.2026) AmiNetXDuo <commit>`, and `bsdsocket.library` answers at all, it carried no version string before, so there was no way to tell an installed copy apart. One number for the whole set instead of a private one per command, the date from the build rather than from whoever last edited the file, and the commit so two builds of the same release can be told apart. The name is in there because Roadshow ships commands called `AddNetInterface`, `ping`, `arp` and `netstat` too
- `STATE=down` on the only interface no longer stops the network library opening. It could not be undone from the machine it happened on: nothing could open the library, so there was no `Online` to bring the interface up with and no `ShowNetStatus` to see it, editing the interface file was the only way out
- `AddNetInterface` on an interface configured down says so, instead of advising a check of the cable
- `NetStackQuery()` and `NetStackControl()` are published, at -0x366 and -0x36c, so a third-party `netstat` or `ifconfig` can be written. They are what `ShowNetStatus`, `netstat` and `arp` are built on and nothing else. `aminetxduo/netstatus.h` joins the Developer drawer, and a caller checks `lib_Revision` against `AMI_NETSTATUS_MIN_REVISION` before calling. Published means fixed: `NetStatusHeader` and every `NETCTRL_*` request structure are part of the interface from here on
- Correction to 0.16.0: that release listed "Closing a TLS connection while another program is using one no longer risks reaching through the closed one". The change is real and stays, but it hardens something a program could not actually reach, both writers of the connection registry already hold `Forbid()` across the whole update, and every lookup is a task asking for its own connection. It should not have been listed as a fault anyone could meet

## 0.16.1

- `AddNetInterface` that cannot bring an interface up gives the machine its memory back. A failure after the card had opened, a PCMCIA card in the slot that will not initialise, a cable that is not there, no DHCP answer, left the network running with nobody using it, and the memory it holds was gone until the machine was switched off. Measured at 580,704 bytes on a machine with 8 MB free, and about 400 KB on a 1 MB one, which is most of what such a machine has
- `AddNetInterface` no longer says an interface came up when it did not. After one failure of the kind above, every later run reported success against the network the failed one had left behind, so a machine with nothing on the wire looked configured
- The network can be started again after a failed attempt. Once one had failed, the count of who was using the stack could never reach zero, so it could not be taken down and could not be restarted; a reboot was the only way out
- A card that is fitted and will not answer is no longer reported as a missing driver. `AddNetInterface` said "There is no cnet.device on this machine" at a machine whose cnet.device had just opened, because it looked for the driver file in four directories instead of asking the card. It now asks, and says that the driver and the unit number are not what to look at
- `AddNetInterface` gives back the library it opened when it finds another TCP/IP stack installed

## 0.16.0

- Two `BeginInterfaceConfig()` calls for different interfaces at the same time no longer risk one of them never being answered, which left the program that made it waiting on its message forever
- Starting a packet capture on a machine where the network had not yet read the clock no longer stops multitasking for the length of a device open, once for every frame captured
- Opening `usergroup.library` while another program is reading `DEVS:passwd` no longer stops multitasking for the length of that disk access
- `recvmsg()` and `sendmsg()` accept a control buffer declared with `CMSG_BUFFER()`. The macro did not give the buffer the alignment it exists to give it, and the library then refused about half of them, `MSG_CTRUNC` with no ancillary data on receive, `EINVAL` on send, depending on where the linker had put it
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
- The raw-framing probe is off unless it is asked for, in a build that does not go through CMake as well as one that does. On a device that ignores `AbortIO()`, Commodore's `a2065.device` among them, it never returns
- Fixed three ways a hostile or broken server could read past the end of a buffer during a TLS handshake, and two more in the code underneath it. A certificate two bytes long, a 38-byte ServerHello and a zero-length CertificateRequest each walked off the end of the record buffer; a signature length was checked against the wrong bound; and every RSA modulus tripped a signed shift. On a machine with no memory protection a read past a buffer is not a crash, it is whatever happened to be next
- `ping`, `ShowNetStatus` and `AddNetRoute` give back the memory they read `DEVS:Internet` into. Each run lost about 12 KB until the next reboot, which on a 1 MB machine is roughly seventy runs
- `connect()` from a socket bound to one of the machine's addresses now leaves from that address instead of being refused
- New `Developer` drawer in the archive: the headers and linkable glue for everything this stack offers that the NDK does not declare, so a program built against it can call `if_nametoindex()`, `if_indextoname()`, `if_nameindex()` and `if_freenameindex()`, and name the IPv6 constants, without writing the vector offsets out by hand
- Expunging the library gives all its memory back. An open, close, expunge and reopen cycle used to lose 12,612 bytes of the machine's free memory every time, so repeatedly starting and stopping the network eventually ran it out
- A browsed service whose address did not arrive with it is now asked for, so a row that said "no address" gives one. Only the rows that need it wait, and the whole listing spends at most two seconds on it
- `ShowNetServices ALL` lists every instance of every type answering, rather than only the types. It costs one more listening window, not one per type
- A socket bound to one of the machine's addresses now sends from it. UDP and raw datagrams leave with the bound address as their source, and so does a TCP connection: on a machine with two interfaces, a `connect()` from an address on one of them leaves from that address rather than from whichever one the routing table preferred
- A destination the bound address cannot reach is refused with `ENETUNREACH`, on `connect()` as well as on a datagram. Such a datagram used to be handed to the stack and dropped inside it with the send already reported as successful
- `sendto()` and `sendmsg()` on a raw socket honour an IPv6 zone, `fe80::1%2` leaves by interface 2, as a UDP socket already did
- IPv4 multicast works: `IP_ADD_MEMBERSHIP`, `IP_DROP_MEMBERSHIP`, `IP_MULTICAST_IF`, `IP_MULTICAST_TTL` and `IP_MULTICAST_LOOP`, so a program that discovers things on the local network, SSDP, UPnP, a ported mDNS, can open the socket it expects instead of getting "Protocol not available"
- `bind()` to a multicast group address is accepted, which is how a program listening for a group is written
- The `68000-minimal` drawer leaves multicast out along with the other optional features, which is 3,888 bytes
- IPv6 multicast works too: `IPV6_JOIN_GROUP`, `IPV6_LEAVE_GROUP`, `IPV6_MULTICAST_IF`, `IPV6_MULTICAST_HOPS` and `IPV6_MULTICAST_LOOP`, with `struct ipv6_mreq` and the option numbers in the new `aminetxduo/in6.h`. A joined group is delivered to the socket that joined it and dropped when it closes. No Multicast Listener Report is sent, there is no MLD in this stack, so a group works on the local segment, which covers the link-local groups mDNS, LLMNR and SSDP use, and a switch that prunes by MLD snooping will not forward a wider one
- `bind()` to an IPv6 group address is accepted, as it already was for IPv4
- A socket bound to a multicast group can send to it. The bound group was treated as the address to send from, which no interface has, so every send from such a socket failed with `EADDRNOTAVAIL`, and binding the group then sending to it is how an SSDP client is written
- `recvmsg()` can now report which interface and local address a datagram arrived on, and its hop limit: `IPV6_RECVPKTINFO`, `IPV6_RECVHOPLIMIT` and, for IPv4, `IP_PKTINFO` and `IP_RECVDSTADDR`. A server on a machine with more than one address could not previously tell which of them a query was sent to, so it could only answer from whichever the routing table preferred
- `sendmsg()` can name the source address and outgoing interface for one datagram, and `setsockopt(IPV6_PKTINFO)` sets a standing one, so a server can answer on the interface a query came in on. An interface or address the machine does not have is refused rather than quietly replaced
- Raw ICMPv6 sockets take an `ICMP6_FILTER`, so a program watching for one kind of ICMPv6 message is no longer handed every neighbour solicitation on the network as well
- `sendmsg()` can set the hop limit of one datagram with an `IPV6_HOPLIMIT` object, which is what a traceroute over UDP is made of
- `IP_TTL` and `IPV6_UNICAST_HOPS` reach the wire on a UDP socket. They were stored and read back and never applied, so every UDP datagram left with the stack default whatever the program asked for
- `sendmsg()` on a raw socket takes the same source address and interface a UDP one does
- The loopback interface has a name and a number, `lo0`, from `if_nametoindex()`, `if_indextoname()` and `if_nameindex()`. A datagram over `::1` or `127.0.0.1` reports it like any other arrival instead of reporting nothing, and a send can name it
- New header `aminetxduo/cmsg.h` with the structures and `CMSG_*` macros the above needs. The NDK's own `CMSG_NXTHDR` cannot be compiled, it uses an `ALIGN()` no NDK header defines, and its `CMSG_FIRSTHDR` returns a pointer where it should return `NULL`; both are replaced, and `CMSG_LEN` and `CMSG_SPACE` added
- `ssh` asks for 8 KB of stack instead of 256 KB, and gives it back. Its deepest measured use is 5,008 bytes; the old figure was a guess made for a program this never built. And every invocation used to lose the whole 256 KB of the machine's free memory until the next reboot
- `ssh` closes `bsdsocket.library` when it finishes, and stops the console reader it started. Neither happened before: both are registered with `atexit()`, and on this compiler's startup code `exit()` ends the program without running anything registered that way. So every `ssh` left the network library with one more user than it had, which is enough to stop it ever being unloaded, and an interactive session left a reader process running and two of the program's 32 signal bits gone until the machine is switched off. An `ssh` that connects and finishes now costs the machine nothing at all, measured; it used to cost 4,224 bytes

## 0.15.1

- New `Developer` drawer in the archive: the headers and compiler glue for what bsdsocket.library has that the NDK does not declare. `if_nametoindex()`, `if_indextoname()`, `if_nameindex()` and `if_freenameindex()`; and the AF_INET6 names, `IPPROTO_IPV6`, `PF_INET6`, `INET6_ADDRSTRLEN`, `IPV6_V6ONLY`, `IPV6_UNICAST_HOPS`, `IPV6_TCLASS`, `IN6ADDR_ANY_INIT`, `IN6ADDR_LOOPBACK_INIT`, the `IN6_IS_ADDR_*` macros, `struct sockaddr_storage` and `AI_ADDRCONFIG`. Put its `include` on the compiler's include path, `#include <proto/aminetxduo.h>`, and open `bsdsocket.library` as usual; there is no second library and no link library. `Developer/examples/` has two working programs and `Developer/ReadMe` has the rest, including the warning that this NDK's `struct sockaddr_in6` keeps its family byte at a different offset from `struct sockaddr_in`, so the two cannot be cast through `struct sockaddr *`
- Two programs adding an interface at the same moment can no longer be given the same one: the slot was picked and then the device opened, which takes long enough for the second to pick it again
- `STATE=down` in `DEVS:NetInterfaces` is honoured. It was read from the file and then ignored, so an interface configured down came up anyway; `Online` brings it up as usual
- `GetRouteInfo()` reports the interface each route belongs to. Static routes and the default gateway reported none at all, and the rest counted from 0 where the convention is to count from 1, so a program matching a route to an interface was off by one
- `ShowNetServices` no longer says the list is what answered just now. It is what this machine has heard recently, and something listed may since have gone

## 0.15.0

- New command `ShowNetServices`: what else on the local network is offering something. With nothing after it, the kinds of service answering; naming one, `ShowNetServices _http._tcp`, lists the machines behind it with their addresses, ports and, with `TXT`, their advertised settings. Printers, NAS boxes and media players turn up without being configured anywhere. The list is what answered in a few seconds and says so, since nothing on a `.local` network can say when the answers have stopped
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
- `SBTC_HAVE_LOCAL_DATABASE_API` answers `TRUE`, which it always should have, all nine of the network, protocol and service database calls are implemented
- Added a `Libs/68000-minimal` drawer: the same library with every optional feature compiled out, for the smallest machines
- Upgrading the commands without the library, or without rebooting so the new library is the one in memory, now says so. The reporting commands used to describe that half-installed pair as something else, the stack refusing to report on itself, or a library built without service discovery

## 0.14.3

- Fixed closing the last `bsdsocket.library` handle never returning, so a program that opens the library, uses it and exits no longer hangs
- `AddNetInterface` now says when the network could not start for want of memory, instead of sending you to check the cable

## 0.14.2

- `netstat -h` now reports what the network owns, allocations, sockets and packet buffers, each with a high-water mark, so a suspected leak can be confirmed or refuted without a debugger
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
