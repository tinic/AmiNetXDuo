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
| Diagnostic volume | a configuration carrying keywords this stack ignores by design, beside one real fault | `tests/tools/run-multidef.sh -r compat` | green (landed red) |
| Interface slots | which of 4 definitions gets one of 2 slots, and whose name it answers to | `tests/tools/run-ifslots.sh` | green (landed red) |
| Failure wording | missing device, wrong unit, unusable address, attach cap, no memory | `tests/tools/run-bringupfail.sh`, `bringupfail-verdict.sh` | green (landed red) |

Everything above runs from one stage:

```
tools/ci.sh matrix
```

It needs a Kickstart and nothing else — SLIRP is enough, because every arm asks
whether the machine came up and moved a packet, and SLIRP's gateway answers
ICMP. No bridge, no peer, no licensed Workbench. Twelve boots for the CPU arm,
four for memory, three for interface count, one for refusals, two for interface
slots; about seven minutes in total.

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

**The pool is not the only clamp the big arms reach, and it is the only one
they read.** The TCP receive window and the UDP socket queue are both shares of
the pool with ceilings of their own, and both ceilings are unreachable below
about 13.6 MB free — so the 32 MB arm is the first run in this project's
history where `BSD_TCP_WINDOW_CEILING` and `BSD_UDP_QUEUE_CEILING` are the
binding terms rather than arithmetic nothing evaluates. Nothing then opens a
socket at that memory. `ami_bsd_tcp_window()` and `bsd_udp_queue_max()` have no
host test either, and `tests/netstack/host/test_tcp_rxflood_host.c` hardcodes
the lab A1200's 368 packets and 72,128-byte window, so even the host model is
the small-memory regime. The precondition is now covered and the consequence is
not; the row is in `docs/BACKLOG.md`.

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

#### the `compat` round — the same defect from the other end

Everything above is about a machine being **too quiet** about a file the user
wrote. `run-multidef.sh -r compat` is the same fault inverted, reported by the
user from their own machine: `DEVS:NetInterfaces/genet` carried four Roadshow
keywords this stack reads and deliberately ignores — `IPREQUESTS`,
`WRITEREQUESTS`, `COPYMODE`, `MULTICAST` — and **every** command that loads the
configuration printed six lines about each of them, under the heading
`Problems in the configuration:`, ending with the sentence *"The line is
harmless and can stay"*. `netstat -i` was 33 lines, 21 of them that lecture,
before the table. The same block prefixed an unrelated `ShowNetStatus DHCP`
error, so a question about one interface was answered with an essay about four
keywords and then *"there is no interface called DHCP"*.

The round stages that file beside a `badaddr` definition — a real fault — and
asserts the division of labour in one transcript:

| | |
|---|---|
| ordinary commands | `netstat`, `ShowNetStatus`, `AddNetInterface` say **nothing** about a keyword ignored by design. Not a shorter essay: nothing |
| `CheckNetConfig` | names every one of them, with its line and its reason, under a heading of its own, and does not count them towards the return code |
| genuine faults | **unchanged**. `netstat -i` and `AddNetInterface` still print the bad `ADDRESS`, with the file and the line |

The last row is what stops the fix from being *"print less"*: without a real
fault in the same drawer, the arm would pass on a tree that had simply stopped
reporting configuration problems altogether.

The mechanism is a third severity, `AMI_CFG_PROBLEM_NOTE`
(`include/aminetxduo/config.h`), and a category rather than a list of keywords:
`src/tools/tool_diag.c`'s reporter — the one every ordinary command installs —
drops notes, and `src/tools/checknetconfig.c`'s keeps them. Adding a keyword to
the inert list needs no change anywhere else.

The round also reads the `netstat -i` table itself: a definition that exists and
is not attached is now named there (`Defined but not attached: …`), which is the
visibility clause above asked of the other command a user looks at.

### `run-ifslots.sh` — the visible half was fixed and the usable half was not

`run-multidef.sh` above asks whether every definition is **visible**. This one
asks whether the one a user **names** can be brought **up**, which is the half
that cost the user the rest of their evening after the first was fixed.

The contract:

| | |
|---|---|
| the slot | goes to the interface that was **named**, whatever its place in the drawer |
| the cap | still real. Once both slots are held by interfaces somebody named, a third is refused, by name, with what to type |
| the name | a live interface answers to **its own**. Never to the name of the description that happens to share its array subscript |

Two defects, both pre-existing and both proven so on `bf8959cd`:

**The slot went by position.** `ami_ns_open_devices()` claimed both `NX_IP`
slots walking the sorted description list from the head, so on a drawer of
`aeth0`, `beth1`, `meth2`, `zeth3` the command `AddNetInterface zeth3` failed
`ENOSPC (28)` on a machine where nothing else was online. The alphabet decided
which card the machine was allowed to use.

**The name went by subscript.** With `aeth0` naming a card that is not in the
machine and `zeth3` naming the one that is, the machine came up on `zeth3`'s
card with `zeth3`'s lease and reported it as `aeth0` — in `netstat -i` and in
`ShowNetStatus`, the latter adding a note that the interface file had been
changed after the network started, about a file nobody had touched — while
`zeth3` was listed as offline. The compaction that moves a surviving
description down had already broken the position match the tools relied on.

Six claims and a guard, over three boots:

| claim | |
|---|---|
| 1 | a machine with four definitions boots |
| 2 | `ShowNetStatus` lists all four, as `defined` |
| 3 | a definition that is **not first** in directory order can be brought up, takes a lease and reaches the gateway |
| 4 | a third simultaneous attach is refused, naming both holders |
| 4b | a slot is never taken for an interface that cannot open its device: a mistyped `DEVICE=` costs a message and nothing else |
| 5 | the slot frees on `RemoveNetInterface`, and five add/remove cycles do not leak |
| 6 | a live interface reports its own name, and the bogus file-changed note is gone |

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

## The final gate of the defect-fix phase — 2026-08-25, `1805861d`

Ten commits landed on 2026-08-25, from six agents, and until this run no single
pass had ever covered the finished tree. Each of them was gated against the tip
that existed while it was being written, which proves the commit and not the
result of putting all ten together. A tree that has only ever been tested in
pieces has been tested in pieces.

So: one fresh clone of `1805861d`, its own build directory, on the machine that
has the Kickstarts and `a2065.device` in it — every gate below ran against the
same tree, in the same session, and nothing was rebuilt between them.

| | gate | result |
|---|---|---|
| 1 | `tools/ci.sh host` | **green** — 99/99 ctest, 99 registered against `HOST_TESTS_EXPECTED`, and all twelve `*-verdict-selftest.sh` graders reporting 0 wrong |
| 2 | `tools/ci.sh matrix` | **green** — every arm; the detail is below |
| 3 | `tests/tools/run-ifdhcp.sh`, SLIRP | **green** — PASS, 21 s against a 105 s ceiling |
| 3 | `tests/tools/run-nettools.sh` | **green** — `nettools_result=PASS` |
| 3 | `tests/tools/run-cardsweep.sh` | **green** — `cards=9 pass=9 fail=0 fail_assert=0 skip=0 carried_both_ways=9 wall_s=369` |
| 4 | bridged poolshare, `ne2000_pcmcia` | **green** — `peer_bytes=6356992 peer_lost=0 peer_outoforder=0`, `zero_windows=0`, 140 packets the fewest ever free of 207 |
| 4 | bridged poolshare, `a2065` | **green** — `peer_bytes=8642560 peer_lost=0 peer_outoforder=0`, `zero_windows=0` |
| 5 | `tests/netdev/run-netdelay.sh` at 68020 | **green** — 6 checks, 0 failures |
| 5 | `tests/netdev/run-netdelay.sh` at 68060 | **green** — 6 checks, 0 failures |
| 6 | `tests/tools/run-payverify.sh`, unshaped | **green** — 64/64 connections content-verified, v4 and v6, 11 856 direct fills |
| 6 | the same under `-E 'loss 1%'` | **green** — 26/26, retransmissions up to 17 on one socket, guest overruns 0, qdisc removed and verified |
| 7 | `tools/ci.sh cross`, all 14 configurations | 13 green. `cpu68060` **red, and pre-existing** — proven below |

One red in the whole run, and it is the one that was already known. Nothing
today's ten commits touched is in it.

### The matrix stage, arm by arm

This was the stage with no history to compare against — `tools/ci.sh matrix`
landed on 2026-08-25 as well, so this is the first time it has been run against
a tree it was not written on.

| arm | boots | result |
|---|---|---|
| `check-no-log-advice.sh` | 0 | `log_advice_commands_checked=39 log_advice_offences=0` |
| `run-cpuspeed.sh` | 12 | `cpuspeed_arms=12 cpuspeed_failed=0` |
| `run-bigmem.sh` | 4 | `bigmem_arms=4 bigmem_failed=0` |
| `run-multidef.sh` | 3 | PASS — every definition visible, every refusal carrying a reason |
| `run-bringupfail.sh` | 1 | PASS — every refusal naming its operation and its code |
| `run-ifslots.sh` | 3 | PASS — 29 checks, 0 failures, claims 1–6 and guard 4b |

The memory arm had `AMINETXDUO_KICKSTART_A3000` and used it, so the two arms
that need an A3000 to map their memory ran rather than skipping — the whole
point of that variable is that a skipped 32 MB arm and a green one look alike:

| arm | machine | Fast | Zorro III | pool | pool verdict |
|---|---|---|---|---|---|
| `chip-only` | A1200 68020 | 0 MB | — | 57 | band |
| `lab-a1200` | A1200 68020 | 8 MB | — | 378 | band |
| `accel-32m` | A3000 68030 | 8 MB | 32 MB | 513 | saturated |
| `accel-128m` | A3000 68030 | 8 MB | 128 MB | 513 | saturated |

The clamp still holds across the quadrupling, which is the assertion — 513 in
both rows and not 513 as a constant.

### The two timing arms are the fix, measured

`run-netdelay.sh` is the only gate whose numbers mean something on their own,
because it is the arm the raster-beam work was written for. Both CPUs, same
tree, same boot:

| | spins per line | lines per field | line costed at | 300 ms from the clock | the same hold counted in bus reads |
|---|---|---|---|---|---|
| 68020 | 17 | 313 | 63 µs | **311 ms** | 900 ms |
| 68060 | 29 | 313 | 63 µs | **306 ms** | 859 ms |

A wait asked for 300 ms gets 311 and 306. The counted loop it replaced gets 900
and 859 on the same two machines, which is the shape of a measure of the CPU
rather than of time — and the reason the never-shorter guarantee is the way
round it is.

### The one red: `cpu68060`, pre-existing and unchanged

`tools/ci.sh cross` builds 14 configurations. Thirteen are clean. `cpu68060`
fails, and it fails in `cmake/check-pcrel-branches.cmake`:

```
crypto68k_test: a 32-bit PC-relative branch does not land on a function.
```

**It is not today's.** Two proofs, one structural and one empirical.

*Structural.* Between `f729d014` — the tip before the defect-fix phase began —
and `1805861d`, `git diff` reports **no change at all** to
`cmake/check-pcrel-branches.cmake`, `src/crypto68k/`, `tests/crypto68k/`,
`src/tls/`, `src/tlslib/`, `tests/tls/` or the rest of `cmake/`. The entire
surface this gate inspects is byte-identical across all ten commits.

*Empirical.* The same configuration was built on a pristine `f729d014`
worktree, with `-k` on both sides so the build does not stop at the first
refusal and the whole list is visible. The two lists are identical — same
twelve targets, same ten distinct refusals, in the same set:

```
src/tlslib/tls.library
tests/crypto68k/crypto68k_25519_test   tests/crypto68k/crypto68k_bench
tests/crypto68k/crypto68k_bulk         tests/crypto68k/crypto68k_ec_bench
tests/crypto68k/crypto68k_ec_test      tests/crypto68k/crypto68k_test
tests/tls/tls_bench                    tests/tls/tls_decompose
tests/tls/tls_handshake                tests/tls/tls_https
tests/tls/tls_interop
```

**Two corrections to how this red has been described, both from that
enumeration.** It has been carried as "seven test images in `tests/crypto68k`
and `tls_bench`". It is twelve targets, not seven; and `src/tlslib/tls.library`
is in the list, which is **a shipped image and not a test image**. A user who
selects `-DAMINETXDUO_CPU=68060` — a documented value of a cache variable with
a `STRINGS` list, not an internal knob — cannot build `tls.library` at all.
That is a larger defect than the note it inherited, it is pre-existing on
`f729d014` exactly as it is on `1805861d`, and it is not fixed here: the brief
for this run was to gate the tree and to fix only a proven regression.

The reason it was under-described is worth keeping. `tools/ci.sh` builds in
parallel and without `-k`, so `gmake` stops shortly after the first refusal and
reports whichever targets happened to be in flight. The failure is real either
way and the arm is correctly red, but the *list* that run prints is an artifact
of scheduling. Enumerating it needs `-k`.

### What this run does not cover

Nothing here changes the honest gaps above — the PiStorm/Gayle rate half, the
el3 FIFO under a real card, and two cards in one machine are as uncovered as
they were, and the A1200 is out of service, so every arm above is emulated. The
claim this section supports is narrower and is only about coverage in time: on
2026-08-25 the tree at `1805861d` was gated once, as a unit, by everything in
this document that can be run without that hardware.

And it is `1805861d` and not whatever `main` says today. `e2acb0a3` landed
while this run was in progress; it changes `docs/BACKLOG.md`,
`docs/TEST-MATRIX.md` and one row of `tests/HARNESSES`, and **no source file at
all**, so every result above still describes the code that is in the tree. The
next commit that touches `src/` is the one that makes this section history.
