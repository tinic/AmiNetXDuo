# Changelog

User-visible changes, newest first. Internal work is in the git log.

## Unreleased

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
