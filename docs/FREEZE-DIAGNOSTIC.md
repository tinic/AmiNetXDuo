# Reading the freeze counters

A total lockup writes nothing: no Enforcer hit, no MungWall hit, no log line.
So the stack keeps a running count of the two things that could produce one,
and `netstat -h` prints them.

`netstat -h` opens no library, allocates nothing and takes no lock. It is safe
to run while the copy is hammering the machine, and it answers even when the
rest of the stack has stopped answering.

## What to run

In a second shell, before starting the copy:

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

## What to send back

`DH0:health.log`, or just the last few blocks of it, and one `netstat -h` taken
before the copy started so there is a baseline. Say roughly how long the run
had been going.

The counters restart when the stack does, so a block taken after the reboot
says nothing about the freeze.

## What each number means

```
scheduler:
	22482 ticks in 449661 ms, 0 clipped, 0 lost
	worst stall 0 ms, service 0 us at the time
	baton: 148213 transitions, 4 at once at the peak
	baton: 0 table full, 0 moved, state max 0
	mark at 0x0027ADC4
```

**ticks in ms** — the stack's 50 Hz clock. Ticks divided by seconds should come
out at 50 whatever else is happening. The ms figure is time since the stack
started, so it also dates each block in the log.

**clipped, lost** — times the clock fell so far behind that the arrears were
thrown away, and how many ticks that cost. Above zero means the machine was
held by something.

**worst stall, service** — the longest gap ever seen between two clock ticks,
and how long the clock's own work took on the wakeup before it. These two
together say which of the two possible faults it was:

* stall in the hundreds of milliseconds, service in the hundreds of
  microseconds: a task at priority 20 was not dispatched for that long.
  Something else held the machine.
* stall and service close together: the clock overran its own period. That one
  is ours.

**transitions** — times a stack thread stepped aside to wait on the network
card. The rate matters, not the total: it rises with the number of packets, not
with the number of bytes, which is why a copy of many small files is harder on
the machine than a bulk transfer of the same size.

**at once at the peak** — the most threads that were waiting on the card at the
same moment. The table holds 16.

**table full** — must be zero. Above zero, a thread waited on the card while
holding the lock the whole stack runs under, and everything else queued behind
an event that may never come.

**moved** — must be zero. Above zero, the stack suspended a thread and left
itself with nothing to dispatch.

**state max** — 0 or 1 is normal.

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
there: nothing has started the network yet, or the stack that is up is not this
one.
