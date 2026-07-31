# Changelog

User-visible changes, newest first. Internal work is in the git log.

## Unreleased

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
