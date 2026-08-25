# What the matrix covers, and what it cannot

Facts, not plans. Which axes of "the machine a user actually has" are asserted
somewhere, by what, and — honestly — which ones still cannot be reached
without real hardware.

It exists because on 2026-08-25 three defects were found by hand, on a real
machine, in one evening, and every gate in this tree had walked past all three.
They were not subtle. They were invisible for the same reason: **no arm had
ever varied the axis.** A gate cannot hold an opinion about a configuration it
has never booted.

All three are fixed. The arms below were written against the **contract** while
they were still broken, went red on them, and went green on the fixes without an
assertion changing — which is the only order in which an arm proves anything
afterwards.

| The defect | Why nothing caught it |
|---|---|
| Delay loops that count bus reads (`n = us * 4` in `src/netdev/netdev_pcmcia.c` `pc_settle()`, with siblings in `el3.c`, `ne2000.c`, `netdev_cmds.c`, `netdev_isapnp.c`) collapse on an accelerator | No arm had ever run a CPU other than the A1200's 68EC020. Not one, on any board, at any tier |
| A third file in `DEVS:NetInterfaces/` is dropped silently (`AMI_CFG_MAX_INTERFACES` is 2; `src/config/config_file.c:163`, behind an `AMI_WARN` that compiles out of shipping builds), and which two survive is directory-scan order rather than alphabetical | No arm had ever staged three interface files. The cap had never been reached by a test, so the branch had never executed in CI |
| `Check the debug log for what failed` — advice a shipping build cannot be followed, because `AMINETXDUO_LOG` is off in every drawer that ships | Several selftests assert the wording of a **success**. None asserted the wording of a **failure**, which is the half a user reads |

## The axes, and what asserts them

| Axis | Values covered | Asserted by | State |
|---|---|---|---|
| Board | 9 cards | `tests/tools/run-cardsweep.sh`, `cards.sh` | green |
| CPU model | 68020, 68030, 68040, 68060 | `tests/tools/run-cpuspeed.sh` | green |
| CPU rate | stock (~14 MHz), `cpu_speed=max`, `cpu_multiplier=64` (~209 MHz) | `tests/tools/run-cpuspeed.sh` | green |
| Memory size | 0 MB, 8 MB, 32 MB, 128 MB | `tests/tools/run-bigmem.sh` | green |
| Interface count | 1, 2, 3, 4, 8 definitions | `tests/tools/run-multidef.sh` | green (landed red) |
| Failure wording | missing device, wrong unit, unusable address, attach cap, no memory | `tests/tools/run-bringupfail.sh`, `bringupfail-verdict.sh` | green (landed red) |

Everything above runs from one stage:

```
tools/ci.sh matrix
```

It needs a Kickstart and nothing else — SLIRP is enough, because every arm asks
whether the machine came up and moved a packet, and SLIRP's gateway answers
ICMP. No bridge, no peer, no licensed Workbench. Twelve boots for the CPU arm,
four for memory, three for interface count, one for refusals; about five
minutes in total.

### What the green arms measured, 2026-08-25

Twelve CPU arms, all green: `a2065` and `ne2000_pcmcia` × 68020/68030/68040/68060
× stock, `cpu_speed=max`, `cpu_multiplier=64`. Each one reached `online`, took a
DHCP lease and got an ICMP echo back from the gateway.

The memory arm, which is the one that had never run at scale:

| arm | machine | Fast | Zorro III | pool |
|---|---|---|---|---|
| `chip-only` | A1200 68020 | 0 MB | — | 57 packets |
| `lab-a1200` | A1200 68020 | 8 MB | — | 378 packets |
| `accel-32m` | A3000 68030 | 8 MB | 32 MB | 513 packets |
| `accel-128m` | A3000 68030 | 8 MB | 128 MB | 513 packets |

The clamp holds: quadrupling 32 MB to 128 MB does not move the pool. That
equality *between* arms is the assertion, not a constant — a constant cannot
tell arithmetic that saturated from arithmetic that overflowed into the
minimum clamp on its way out. (`AMI_POOL_MAX_PACKETS` is 512; NetX Duo's own
total is one above the number asked for, hence 513.)

Getting the memory into the guest at all took a measurement rather than an
assumption: **only an A3000 maps it.** An A1200 is `address_space_24 = true` and
asking for `address_space_24=false` does not fix it; an A4000 ignores it too.
Both of those runs are *green* and both would have been a test of 32 MB that
never had 32 MB in it — so the arm skips loudly without
`AMINETXDUO_KICKSTART_A3000` rather than quietly proving nothing.

The two graders run with no emulator at all and are picked up automatically by
the `host` stage's `tests/*/*-verdict-selftest.sh` loop:

```
tests/tools/bringup-verdict-selftest.sh        9 fixtures
tests/tools/bringupfail-verdict-selftest.sh   17 fixtures
tests/tools/check-no-log-advice.sh            strings, needs a cross build
```

## The two that landed red

Both were written against the **contract**, not against what the tree did on the
day. Both went red on the defects they were built for, both went green when
those were fixed — **without an assertion changing**. That order is the point:
an arm added at the same time as its fix proves nothing afterwards.

If a fifth arm is added here, write it the same way round.

### `run-multidef.sh` — definitions were truncated

The contract:

| | |
|---|---|
| definitions | **unlimited**. Every file in `DEVS:NetInterfaces/` is a thing the user said |
| attachment | **capped** is fine. Two cards is a real resource limit; two *definitions* is not |
| refusal | **explicit**. A defined interface that could not be attached is listed as defined-and-not-attached, with the reason, by the command a user runs to see their interfaces |

What it found. `ShowNetStatus` printed its table from `cfg->interface_count`
(`src/tools/shownetstatus.c:635`), which was capped at 2:

| round | definitions staged | listed, before | listed, after |
|---|---|---|---|
| 3 | 3 | 2 | 3 |
| 4 | 4 | 2 | 4 |
| 8 | 8 | 2 | 7 |

And **which** two survive is not alphabetical, whatever the comment above
`insert_interface()` suggests. The array is kept sorted, but the *drop* is by
arrival: the first two the directory scan hands over are kept, and only those
two are then sorted. On a round of eight named `eth0`..`eth7` the machine kept
`eth2` and `eth4` — filesystem order, which a user cannot see, cannot predict
and cannot change except by accident. That is worse than alphabetical, not
better; an alphabetical rule can at least be explained to somebody.

**7 of 8, not 8 of 8, and that is correct.** `eth5` is the file with no `DEVICE`
line. It names no card, so it defines no interface, and the machine says exactly
that by name: `the file 'eth5' cannot be used, so that interface does not exist`.
The arm exempts it from the table clause and holds it to the named-somewhere
clause instead.

That exemption was itself found by running the arm — it had been a false red,
and would have shipped as a permanent one. Which is the argument for keeping an
arm past the defect that prompted it.

### `run-bringupfail.sh` — refusals carried no code, and pointed at a log

The contract is two clauses:

1. **The first line names the failing operation and its code.** First, because
   a user reads one line before deciding what to do. The operation, because "it
   did not work" is not a diagnosis and "opening a2065.device unit 0" is. The
   code, because it is the only part a user can carry to a search or a bug
   report unchanged.
2. **Nothing anywhere sends the user to a log.** `AMINETXDUO_LOG` is off in
   every shipping drawer, so the log cannot exist.

Both halves have a second acceptable form, because one path in this tree
already does *better* than the house style rather than worse. A configuration
fault prints no verb and no number — the path names what failed as precisely as
a verb would, and the line number is a locator the user can act on more directly
than an error code. Both are accepted. What is never accepted is a refusal
carrying neither.

What it found, 2026-08-25, before the fix:

| cause | first line of the refusal, then | verdict |
|---|---|---|
| missing device | `AddNetInterface: nodev was not added to the running network` | **no code** |
| wrong unit | `AddNetInterface: badunit was not added to the running network` | **no code** |
| unusable address | `DEVS:NetInterfaces/badaddr, line 4:` | passed — path and line |
| attach cap | `AddNetInterface: cap2 was not added to the running network` | **no code** |
| no memory | fixture only, see above | — |
| any of them | log sentence in `AddNetInterface`, `CheckNetConfig`, `Online` | **dead-end advice** |

All green now.

The defect was always narrower than "names nothing", and the arm was built to
say so rather than overstate it. The *second* line usually named the reason well
(`There is no nosuchcard.device on this machine.`), and the address case above
is the best message on the whole bring-up path — no prefix, no verb, no number,
and it points at the file and the line. The grader had to learn to accept that
shape; before it did, it reported the best message in the tree as silence, which
would have been the harness inventing a defect.

## What cannot be covered without real hardware

This section is the point of the document. An arm that is believed to cover
something it does not is worse than no arm, because it converts an open
question into a closed one.

### PiStorm / Gayle timing — **not covered, and no emulator knob reaches it**

This is the most important entry in the document, because it is the one axis
where a green arm must not be read as a covered defect.

`tests/tools/run-cpuspeed.sh` does **not** reproduce `pc_settle()`. That was
measured while the `n = us * 4` loop was still in the tree, unfixed, and every
arm was green — so the twelve green arms below did **not** catch the defect that
prompted them, and could not have. Timing the window around `netstack_startup()` — which contains the
PCMCIA claim, and therefore `pc_settle(300000)` — off the stamped serial log,
on `ne2000_pcmcia` at 68030:

| `cpu_multiplier` | nominal | window |
|---|---|---|
| 4 | 13 MHz (stock A1200) | 452 ms |
| 16 | 52 MHz | 390 ms |
| 64 | 209 MHz | 574 ms |
| 192 | 628 MHz | 1714 ms |

The loop does not get **shorter** as the CPU gets faster. Past about 52 MHz it
gets **longer**. Two independent reasons, both structural:

* An emulated bus read costs the **host** a roughly fixed amount of work, so
  `us * 4` reads take host-time proportional to the count and the emulated
  clock barely enters into it. On a PiStorm the ratio is the entire defect —
  the CPU is tens of times faster and the Gayle bus is not — and under
  emulation that ratio cannot be created, because the bus is not a slower thing
  that the CPU outruns.
* The emulated card has **no settle time to violate**. A software model answers
  a register write immediately, so a delay that is too short has nothing to be
  too short *for*.

So the CPU arm covers the **model** half of the class — code that behaves
differently at 68030/68040/68060, caches, alignment, anything gated on the
model, and any path that hangs, expires or races when the processor changes
underneath it — and not the **rate** half. The rate half needs an accelerated
Amiga. It is worth booting anyway: before it, both halves had a coverage of
zero.

### The el3 FIFO under a real card — **not covered**

`src/netdev/el3.c` has a `pc_settle()` sibling and a FIFO whose depth and
drain behaviour are the card's, not the driver's. Amiberry emulates no
3c589/EtherLink III at all, so there is no arm to write: the code is reachable
only on a machine with the card in it. `tests/tools/cards.sh` covers the nine
boards the emulator does model, and this is not one of them.

### Two cards in one machine — **not covered, and it is the emulator**

Amiberry holds one network board of each family in file-scope statics, so only
the board named by `-N` is ever instantiated; a second one written into the
config never appears, silently. What that leaves untested in `anxnet.device` is
unit numbering across more than one board, and `CARD=` picking the right one of
several. The note is on `tests/tools/cards.sh`, where the table lives.

This matters to `run-multidef.sh`: it stages many definitions of the **one**
card the machine has, so its attach-cap arm is "more definitions than cards",
never "two working cards and a third definition".

### A genuinely starved machine — **covered by fixture, not live**

`run-bringupfail.sh` does not drive the out-of-memory cause live. Starving an
emulated A1200 until the pool allocation fails, without also starving it until
the Shell cannot load the command, is a narrow window and a flaky arm. The
cause is covered by a fixture in `bringupfail-verdict-selftest.sh` and is
reported in the table as `fixture` rather than being quietly absent.

## Adding an axis

The pattern the four arms share, and the reason each file is split the way it
is:

* the **grader** goes in a sourceable file with no emulator dependency
  (`bringup-verdict.sh`, `bringupfail-verdict.sh`);
* its **selftest** drives it against transcripts for runs nobody can produce on
  demand, and is picked up by `tools/ci.sh host` automatically by name;
* the **harness** boots the thing and applies the grader;
* the row goes in `tests/HARNESSES`, which `tools/check-harnesses.sh` checks in
  both directions.

The split is not ceremony. It is the only thing that can tell an arm that
passes from an arm that cannot fail, and every arm here found at least one
assertion of its own that could not fail while it was being written — a grep
for a reason word that matched the command's own progress echo, a name that was
present only because the harness had asked for it, a check-count floor that a
one-packet pool clears.
