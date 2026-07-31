# Changelog

User-visible changes, newest first. Internal work is in the git log.

## Unreleased

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
