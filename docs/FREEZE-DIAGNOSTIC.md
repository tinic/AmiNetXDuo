# Reading the health counters

A total lockup writes nothing — no Enforcer hit, no MungWall hit, no log line —
and a slow leak writes nothing either, because `AvailMem` falls for every
program on the machine and cannot say whose. So the stack counts both, and
`netstat -h` prints them.

`netstat -h` opens no library, allocates nothing and takes no lock, so it
answers while the rest of the stack has stopped answering. `netstat -s -h` and
`ShowNetStatus MEMORY` do enter the stack, so a packet or two of disagreement
between them and `netstat -h` is expected. The pool figures under `netstat -h`
are as of the last thing the stack did.

A trail is `netstat -h >>DH0:health.log` on a `Wait 5` loop from an `Execute`
script, on a real disk, started before the fault. The evidence is the trend, not
the final block: the last block may still be in a filesystem buffer. A leak is a
slope — use `Wait 30` and run it long. The counters restart when the stack does.

## The memory block

| Counter | Meaning | Bad |
|---|---|---|
| `N allocations outstanding, N at the peak` | blocks the stack has taken and not given back. Ours alone: it does not move when another program allocates. Tens on an idle stack, rising and falling with open sockets. The peak is what makes a single reading worth sending | a number that only ever rises |
| `N refused` | allocations that came back empty | above zero: the machine ran out of memory, a different fault from a leak and usually with one behind it |
| `N sockets open, N at the peak` | `AmiSocket` structures the library owns. Not the sockets a program has open — a closed TCP connection is held until the protocol has finished with it, so this lags by up to a minute and then comes back | hundreds, on a machine with a handful of connections |
| `N programs have it open` | holders of `bsdsocket.library`, the denominator for the line above. Forty sockets across ten programs is a busy machine; forty across one is worth a look. `netstat -s -h` counts itself | — |
| `N of M packets free, N fewest ever` | the network's own fixed pool. It does not grow, so it starves rather than leaks. Healthy is within a few packets of the total | single figures: the pool is at its limit |
| `N found the pool empty, N waited` | what happened when it ran out — dropped, or suspended waiting. Both reach a user as the network stalling | either above zero |
| `N released twice` | — | above zero is a defect in the stack, not a capacity problem |
| `N bytes of system memory free` | `AvailMem`, printed here so it can be read beside ours. Falling while our numbers stay flat means the leak is not the network's | — |

No leak looks like a machine that has done work and come back to its own
baseline: both peaks move a long way, both live counts return to where they
started.

## The scheduler block

| Counter | Meaning | Bad |
|---|---|---|
| `N ticks in N ms` | the stack's 50 Hz clock. Ticks divided by seconds should be 50 whatever else is happening. The ms figure is time since the stack started, so it also dates each block in a log | a rate under 50 |
| `N clipped, N lost` | times the clock fell so far behind that the arrears were thrown away, and how many ticks that cost | above zero: something held the machine |
| `timer wheel N ticks late, worst N` | how far behind the ThreadX timer wheel ran | a worst well above 1 |
| `worst stall N ms, service N us` | the longest gap between two clock ticks, and how long the clock's own work took on the wakeup before it | stall in the hundreds of ms with service in the hundreds of us: a priority-20 task went undispatched that long, so something else held the machine. Stall and service close together: the clock overran its own period, and that one is ours |
| `baton: N transitions, N at once at the peak` | times a stack thread stepped aside to wait on the network card, and the most waiting at once. The table holds 16. The rate matters, not the total: it rises with packets, not bytes, which is why many small files are harder on the machine than one bulk transfer of the same size | — |
| `baton: N table full` | — | above zero: a thread waited on the card while holding the lock the whole stack runs under, and everything else queued behind an event that may never come |
| `baton: N moved` | — | above zero: the stack suspended a thread and left itself with nothing to dispatch |
| `baton: state max N` | 0 or 1 is normal | — |

Either must-be-zero line above being non-zero after a freeze is the freeze, not
a statistic.

## With a debugger

`mark at 0x…` is the address of an `AmiHealthMark`, published as the public
semaphore `AmiNetXDuo.Health` while the stack is up. It holds pointers to the
live counters, so a debugger on a frozen machine reads what the stack had at the
moment it stopped. Layout is `include/aminetxduo/health.h`; the magic longword
is `'ANXH'` (`health.h:54`), for finding it by scanning.

`netstat -h` reports no stack running when the mark is absent: nothing has
started the network, the stack that is up is not this one, or it is a version of
this one that keeps a different set of counters. Commands and library are
installed together for that reason.
