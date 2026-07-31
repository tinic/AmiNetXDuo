# Reading the health counters

Two faults leave no evidence behind. A total lockup writes nothing -- no
Enforcer hit, no MungWall hit, no log line -- because the machine stops before
anything can be written. A slow leak writes nothing either: `AvailMem` falls,
but it falls for every program on the machine and cannot say whose.

So the stack keeps a running count of both, and `netstat -h` prints them.

`netstat -h` opens no library, allocates nothing and takes no lock. It is safe
to run while the copy is hammering the machine, and it answers even when the
rest of the stack has stopped answering.

## What to run

In a second shell, before starting whatever provokes the fault:

```
1> netstat -h
```

To leave a trail, put this in a file and run it as a script (`Execute`):

```
lab loop
C:netstat -h >>DH0:health.log
C:Wait 5
skip back loop
```

Point it at a real disk, not `RAM:`. The last block before a freeze may still
be sitting in a filesystem buffer and be lost, so the useful evidence is the
trend across the run, not only the final block.

For a suspected leak `Wait 30` is enough, and the run wants to be long: a leak
is a slope, and two blocks five seconds apart do not have one.

## What to send back

`DH0:health.log`, or just the last few blocks of it, and one `netstat -h` taken
before the run started so there is a baseline. Say roughly how long the run had
been going, and what the machine was doing.

The counters restart when the stack does, so a block taken after a reboot says
nothing about what happened before it.

## What a healthy machine looks like

A freshly started stack with one interface up and nothing using it -- an A1200,
68020, 10 MB, address from DHCP:

```
memory:
	21 allocations outstanding, 21 at the peak, 0 refused
	0 sockets open, 0 at the peak, 1 programs have it open
	219 of 256 packets free, 219 fewest ever, 1568 bytes each
	0 found the pool empty, 0 waited, 0 released twice
	9408160 bytes of system memory free, 7297264 in the largest block

scheduler:
	158 ticks in 3173 ms, 0 clipped, 0 lost
	0 over budget, 0 ticks deferred
	timer wheel 0 ticks late, worst 1
	worst stall 0 ms, service 0 us at the time
	baton: 12 transitions, 4 at once at the peak
	baton: 0 table full, 0 moved, state max 0
	baton: 0 shared interrupt states
	mark at 0x0026A96C
```

The same machine after ninety seconds of a workload that opened and closed
about two hundred sockets, and then stopped:

```
memory:
	21 allocations outstanding, 92 at the peak, 0 refused
	0 sockets open, 70 at the peak, 1 programs have it open
	220 of 256 packets free, 215 fewest ever, 1568 bytes each
	0 found the pool empty, 0 waited, 0 released twice
```

That is what "no leak" reads like: both peaks moved a long way and both live
counts came back to exactly where they started. A machine that has done work
and returned to its own baseline is the thing to compare against.

## The memory block

**allocations outstanding** -- blocks the stack has taken and not given back.
It is ours alone, which is the whole point: it does not move when another
program allocates. Tens on an idle stack, rising and falling with the number of
open sockets. **A number that only ever rises is the report to make.** The
figure beside it is the most there have ever been at once; one reading cannot
say whether a number is climbing, and the peak is what makes one reading worth
sending.

**refused** -- allocations that came back empty. Above zero means the machine
ran out of memory, which is a different fault from a leak and usually has one
behind it.

**sockets open** -- `AmiSocket` structures the library owns. Not the same as the
sockets a program has open: a closed TCP connection is held until the protocol
has finished with it, so this lags a program's own count by up to a minute and
then comes back. §37.5 of the research notes was 776 of these, and nothing on
the machine could say so at the time. Anything in the hundreds on a machine
with a handful of connections is that fault.

**programs have it open** -- how many programs hold `bsdsocket.library`. The
denominator for the line above: forty sockets across ten programs is a busy
machine, forty across one is worth a look. `netstat -s -h` counts itself and so
reads one higher than `netstat -h` does; that is not a discrepancy.

**packets free** -- the network's own fixed pool of packet buffers. It does not
grow, so it starves rather than leaks, and that is a different fault with a
different fix. **fewest ever** is the closest it has come to running out. On a
healthy machine it stays within a few packets of the total; a workload that
takes it to single figures is running the pool at its limit.

**found the pool empty, waited** -- what happened when it did run out. Both must
be zero. Above zero, something asked for a packet and there was none: the stack
either dropped what it was doing or suspended waiting, and both reach a user as
the network stalling.

**released twice** -- must be zero. Above zero is a defect in the stack, not a
capacity problem.

**system memory free** -- `AvailMem`, the machine's own figure, printed here so
it can be read next to ours. Falling while our numbers stay flat means the leak
is not the network's.

The pool figures on `netstat -h` are as of the last thing the stack did, since
reading them for real would mean entering the stack this command exists to stay
out of. `netstat -s -h` and `ShowNetStatus MEMORY` do enter it and take a fresh
reading, so a packet or two of disagreement between them is expected and is not
a fault.

## The scheduler block

**ticks in ms** -- the stack's 50 Hz clock. Ticks divided by seconds should come
out at 50 whatever else is happening. The ms figure is time since the stack
started, so it also dates each block in the log.

**clipped, lost** -- times the clock fell so far behind that the arrears were
thrown away, and how many ticks that cost. Above zero means the machine was
held by something.

**worst stall, service** -- the longest gap ever seen between two clock ticks,
and how long the clock's own work took on the wakeup before it. These two
together say which of the two possible faults it was:

* stall in the hundreds of milliseconds, service in the hundreds of
  microseconds: a task at priority 20 was not dispatched for that long.
  Something else held the machine.
* stall and service close together: the clock overran its own period. That one
  is ours.

**transitions** -- times a stack thread stepped aside to wait on the network
card. The rate matters, not the total: it rises with the number of packets, not
with the number of bytes, which is why a copy of many small files is harder on
the machine than a bulk transfer of the same size.

**at once at the peak** -- the most threads that were waiting on the card at the
same moment. The table holds 16.

**table full** -- must be zero. Above zero, a thread waited on the card while
holding the lock the whole stack runs under, and everything else queued behind
an event that may never come.

**moved** -- must be zero. Above zero, the stack suspended a thread and left
itself with nothing to dispatch.

**state max** -- 0 or 1 is normal.

Either of the two "must be zero" lines being non-zero after a freeze is the
freeze, not a statistic.

## With a debugger

`mark at` is the address of an `AmiHealthMark`, published as the public
semaphore `AmiNetXDuo.Health` for as long as the stack is up. It holds pointers
to the live counters, so a debugger on the frozen machine reads what the stack
had at the moment it stopped. Layout in `include/aminetxduo/health.h`; the
magic longword is `'ANXH'`, for finding it by scanning.

## If it says no stack is running

`netstat -h` reports "no AmiNetXDuo stack is running" when the mark is not
there: nothing has started the network yet, the stack that is up is not this
one, or it is a version of this one that keeps a different set of counters.
Commands and library are installed together for that reason.
