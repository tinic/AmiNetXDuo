# Ship decisions handed to the release instance

2026-08-25.  The transfer-performance campaign is closed: both our stack and
AmiTCP_NG sit on the identical 1.31 Mbit/s steady plateau over the same
anxnet.device on the real A1200+3c589, which names the plateau as the
hardware/PIO ceiling and satisfies the match-or-beat rule
(docs/RECEIVE_BUDGET.md, the two calibration sections).  Everything the
campaign proved is on main; every flag it added preserves the shipped
default.  What is left is a set of RELEASE decisions -- defaults, presets,
packaging -- and none of them is taken here.  Each entry below is the
evidence and a recommendation; the decision belongs to the release.

## (a) TX_LAZY_COLLECT: default ON, and for which tier

`-DAMINETXDUO_TX_LAZY_COLLECT` parks the TX completion signal while sends
flow and lets the next send's reap collect, with a one-tick TX_TIMER as the
quiet-link safety net (<= 20 ms collection bound; the 11 s-bug duty the
per-completion signal existed for stays covered).  Flag is on main, default
OFF.

Evidence: hardware A/B on the A1200, +5.7-6.6% (three runs 1.174/1.195/1.197
vs control ~1.115-1.124 Mbit/s), the post leg falling ~2.9 -> ~1.5 ms/ACK --
exactly the predicted wake/preempt glue.  Three emulator gates passed on the
lazy build (poolshare bridged, ifdhcp including RELEASE = the lone send on a
quiet link that exercises the safety net, run-iperf), and every physical
build since the lazy landing has carried the flag ON through the campaign's
whole run ladder without a loss or a hang.  One reading note: the `ack`
WALL leg rises to ~13 ms under lazy BY DESIGN (collection piggybacks the
next send); it is latency of an already-replied completion, not CPU, and
must not be read as a regression.

Recommendation: default ON for the small (2 MB chip) tier at minimum -- that
is the machine the +6% was measured on and the campaign soaked.  ON for all
tiers is defensible (the mechanism is not tier-specific and the safety net
is bounded), but no big-memory machine has had the A/B, so shipping it
everywhere rests on reasoning where the small tier rests on record.

## (b) The small-tier build preset

The campaign measured three flags a 2 MB chip-only machine would want as a
named preset rather than as folklore:

- `AMINETXDUO_MDNS=OFF`: about 34 KB off the library (the campaign's
  matched pair: 400112 -> 365184 bytes, same configuration otherwise), and
  the whole mDNS CPU cost gone -- measured ~1% of rate in ambient LAN
  traffic and ~10% under a response storm as pure arithmetic, EVEN AFTER
  the fork's per-record yield fix bounded the old ACK-starving holds.  The
  cost is losing the responder: no `amiga-1200.local`, no service browse.
- `AMINETXDUO_TCP_TIMESTAMP=OFF`: settle leg 3.67 -> 3.09 ms, rate +~1%,
  reconfirmed across many later runs.  Costs RFC 1323 timestamps (PAWS,
  better RTT estimates) on a machine whose windows never justify them.
- `AMINETXDUO_IPV6=OFF`: about 69 KB off the library.  This one is a real
  tradeoff, not a free cut: the campaign's own history is the argument for
  keeping v6 -- when a probe build broke IPv4 receive on the real machine,
  IPv6 stayed end-to-end alive and WAS the rescue channel that let the rig
  be repaired remotely (docs/RECEIVE_BUDGET.md era; the v6-only deploy
  path).  A 2 MB machine run headless benefits from v6 exactly when things
  go wrong.
- Plus (a) ON, per its record.

Recommendation: ship a named small-tier preset with MDNS=OFF, TS=OFF and
LAZY=ON, and keep IPV6 ON by default even there, offering IPV6=OFF as the
documented last 69 KB for machines that truly cannot afford it.  The
preset is a convenience wrapper over existing flags -- no new code paths.

## (c) The installer selecting the tier by AvailMem

The stack already scales its packet pool from AvailMem at start; the
question is whether the INSTALLER should pick the small-tier preset the
same way (e.g. total memory <= 2 MB, or no Fast RAM at all -> small tier).

Evidence: nothing measured argues against it -- the small-tier flags were
all measured on exactly the machine AvailMem would select them for.  The
risks are operational: an installer that silently picks a no-mDNS build
surprises the user whose `.local` name vanishes, and a memory upgrade later
does not re-run the installer.

Recommendation: have the installer DETECT and PROPOSE (default the choice by
AvailMem, say what the small tier gives up in one sentence), never silently
select.  The detection is one AvailMem call; the sentence is the work.

## (d) LTO and the physical boot-lock -- OPEN

A clean LTO Release build of main (08fe2a21 era) boot-locked the physical
A1200; a non-LTO build of the same tree ran.  The campaign never
root-caused it -- an unproven LTO miscompile somewhere in the image -- and
every physical deployment since has been non-LTO by rule, while LTO remains
the cross-build default and every emulated configuration runs it happily.

This is the one entry with an unresolved fact at its centre.  The release
cannot close it by policy: either the shipped image is LTO (smaller,
faster, and the shape that locked a real machine once), or it is non-LTO
(the shape every physical hour of the campaign actually ran).

Recommendation: ship non-LTO until the 08fe2a21 lock-up is reproduced and
named, and keep non-LTO as the documented diagnostic build shape
regardless.  The size cost is real but the campaign bought all its physical
evidence on non-LTO images; shipping the untested-on-hardware shape to save
KB inverts the burden of proof.

### What was tested off-hardware, 2026-08-25

The A1200 is out of reach this cycle, so this is what could be established
without it.  It does not close (d).  It narrows what is left to ask the
machine, and it names one real relocation-class LTO defect that is NOT in
the shipped set.

METHOD.  A fresh clone of main at ffed8d1d, built twice from one tree with
one flag between them: `-DCMAKE_BUILD_TYPE=Release` against
`cmake/toolchain-m68k-amigaos.cmake`, `AMINETXDUO_LTO` ON (the cross
default) and OFF.  Every hunk image in both builds was then walked block by
block with `tools/hunkdiff.py`, which this work adds; `lto-repro/run.sh` was
run against the pinned toolchain; and the LTO library was cold-booted
repeatedly under Amiberry.  08fe2a21 itself was checked out into a worktree
and rebuilt with LTO, so the era that locked the machine was examined and
not only its descendant.

THE FOUR SHIPPED IMAGES ARE STRUCTURALLY SOUND UNDER LTO.  Same three
hunks, same block sequence, same relocation form, nothing present in one
image and absent in the other:

    image                LTO bytes / hunks / relocs   non-LTO
    bsdsocket.library      356,324 / 3 / 6,281        398,628 / 3 / 8,211
    anxnet.device           32,572 / 3 /   407         35,312 / 3 /   597
    tls.library            189,440 / 3 / 2,628        243,380 / 3 / 2,861
    usergroup.library        7,172 / 3 /   208          7,684 / 3 /   259

Both arms are CODE/RELOC32/END, DATA[/RELOC32]/END, BSS/END and nothing
else: no HUNK_DEBUG, no HUNK_SYMBOL, no HUNK_DREL32 and no
HUNK_RELOC32SHORT in either, no memory-attribute flags, no resident name
list, no bytes past the last HUNK_END.  No loadable hunk carries
relocations in one arm and none in the other.  The counts differ the way
inlining makes them differ, not the way a dropped table does, and the
parser was checked against BFD's own reader
(`m68k-amigaos-objdump -r`), which agrees exactly on all eight images.

RELOCATION COMPLETENESS WAS CHECKED PER SITE, not per image.  Every
absolute-long operand in the disassembly was located in the raw instruction
bytes and looked up in the relocation table: 5,206 sites in the LTO
bsdsocket.library, 27 in anxnet.device, 5 in usergroup.library, ZERO
unrelocated in each.  tls.library has two, at the same two addresses in
both arms, and both are constant tables the disassembler read as code.

THE ROMTAG CHAIN SURVIVES LTO IN ALL FOUR, followed in the image rather
than trusted from `nm`: RTC_MATCHWORD found, and rt_MatchTag, rt_EndSkip,
rt_Name, rt_IdString and rt_Init all carrying RELOC32; the RTF_AUTOINIT
table's vector-table and init-function pointers relocated, its NULL
structure pointer correctly not; and every entry of every LVO vector table
relocated -- 150 in bsdsocket.library, 43 in usergroup.library, 14 in
tls.library, 6 in anxnet.device.  The offset-0 entry guard and the `$VER:`
string survive in both arms of both shipped binaries.  The
`-Wl,-u,_<x>_romtag` each target carries is doing exactly the job its
comment claims.

08fe2a21 REBUILT WITH LTO LOOKS THE SAME: bsdsocket.library 354,108 / 3
hunks / 6,049 relocations, anxnet.device 32,572 / 3 / 407, tls.library and
usergroup.library identical to the main figures.  The instrumented shape of
that era was checked too, since the reported artefact was around 354 KB and
carried probes: 08fe2a21 with `-DAMINETXDUO_RXPROBE=ON` under LTO comes out
355,428 / 3 hunks / 6,052 relocations, and the device is byte-identical to
the clean arm.  Whatever locked that machine, the shipped images of that
build are not missing their relocations.

WHAT LTO DOES BREAK HERE, TODAY: TEN TEST IMAGES LOAD UNRELOCATED.  The
sweep found them in the main LTO build and in the 08fe2a21 LTO build and in
neither non-LTO build -- `tests/tls/{tls_handshake,tls_https,tls_interop,
tls_decompose,tls_bench}` and all five `tests/crypto68k/` programs -- each
at 10 or 12 loadable hunks and ZERO relocation entries, against 3 hunks and
thousands in the non-LTO arm.  (`tests/atf/AtfTcpSocket` is an eleventh
image of the same shape and is NOT an LTO effect: 12 hunks and 0
relocations in both arms, so it has been unloadable all along and belongs
in its own row.)  This is 9fb69360's failure exactly: LoadSeg relocates
nothing, and the program jumps into low memory before its first line of
output.  Confirmed in a guest, one flag apart, same source:

    crypto68k_25519_test, LTO      10 hunks, 0 relocs -- TIMEOUT at 180 s,
                                   "NOT ONE BYTE reached the serial port",
                                   zero-byte stdout, no done file
    crypto68k_25519_test, non-LTO   3 hunks, 518 relocs -- PASS, 16,636
                                   checks, 0 failures, exit 0

So Amiberry DOES reproduce this class.  It is not one of the things only
hardware can see, which is part of why the shipped library is unlikely to
have been carrying it.

The mechanism is the LTRANS-late libcall that `src/common/CMakeLists.txt`
already documents at length, arriving by a second route.  Under `-flto` the
64-bit helpers are synthesised during LTRANS, after
`libaminetxduo_m68k_rt.a` has been scanned, so
`libgcc.a(_muldi3.o,_udivdi3.o,_umoddi3.o)` are pulled in instead -- and
those three carry DWARF.  ld's amiga backend gives every `.debug_*` section
its own LOADABLE data hunk, and an image that has one comes out with no
HUNK_RELOC32 at all.  The non-LTO link of the same program pulls
`ami_udivdi3.c.obj`, pulls no libgcc member, has zero `.debug` sections and
loads correctly.

One flag decides it, and the four shipped targets already pass it.  Same
objects, same `-flto`, `--gc-sections` the only difference:

    tls_handshake without --gc-sections   12 hunks,     0 relocations
    tls_handshake with    --gc-sections    3 hunks, 5,399 relocations

`src/tools/`, `tools/profiler/`, the three libraries and the device all
link `--gc-sections`; `tests/` does not.  That is why the release set is
clean and the test set is not -- by a property of a link line, checked by
nothing.

THE REDUCED REPRODUCER, EVERY ARM (`lto-repro/run.sh`, pinned toolchain,
gcc 16.2.0b, ld 2.39):

    arm=nolto_nogc    bytes=964 entry=ok  vertag=ok   romtag=ok   vectable=ok
    arm=nolto_gc      bytes=964 entry=ok  vertag=ok   romtag=ok   vectable=ok
    arm=lto_nogc      bytes=544 entry=ok  vertag=ok   romtag=ok   vectable=ok
    arm=lto_gc        bytes=480 entry=BAD vertag=GONE romtag=ok   vectable=ok
    arm=lto_gc_keep   bytes=436 entry=BAD vertag=GONE romtag=GONE vectable=GONE
    arm=lto_nogc_keep bytes=952 entry=ok  vertag=ok   romtag=ok   vectable=ok
    arm=exe_nolto     bytes=66000
    arm=exe_lto       bytes=61552

Read with the hunk walker rather than with `nm`, those arms say something
sharper than they print.  `lto_nogc` has 72 bytes of code and no
relocations and `lto_gc` has 32, against 324 bytes and 19 relocations
non-LTO: the library was REMOVED, and `romtag=ok` on those lines is a FALSE
NEGATIVE -- the names are still in HUNK_SYMBOL, pointing at an image that
no longer contains them.  `nm` cannot answer this question; size and
relocation count can, and run.sh would be worth teaching that.

What the arms demonstrate is the trap CMakeLists.txt already describes,
not a new one: none of them passes `-Wl,-u,_min_romtag`, nothing references
a romtag, so the whole-program view deletes the chain and the link still
exits 0.  `__attribute__((used))` alone holds it without `--gc-sections`
(`lto_nogc_keep`, 952 bytes) and does NOT hold it with (`lto_gc_keep`,
436).  Adding the keep the tree actually ships restores the image
completely -- two arms run on top of run.sh's six:

    arm=u_nogc lto=1 gc=0 keep_u=yes bytes=952 entry=ok vertag=ok  2 hunks, 18 relocs
    arm=u_gc   lto=1 gc=1 keep_u=yes bytes=952 entry=ok vertag=ok  2 hunks, 18 relocs

against non-LTO's 964 bytes, 2 hunks, 19 relocations.  The guarded LTO
library and the non-LTO library are the same shape.

EMULATED BOOT STRESS, playhouse3: own clone, own build directories, own
`AMINETXDUO_RUN_TAG`s, and every arm matched by a non-LTO control run in
the same sitting.  Sixteen cold boots of the LTO bsdsocket.library, eleven
of them over the LTO anxnet.device as well, and no boot-time hang in any of
them:

    a2065, bridged, LTO x5      every boot clean in ~55 s, our device
                                opened (`sana2_staged ...
                                driver=anxnet.device source=anxnet
                                card=a2065`), a real lease from the LAN
                                router each time (.147/.175/.131/.181/.138),
                                66-127 frames in and 21-22 out, 26 ok.
                                Non-LTO control identical (.128, 70 in,
                                22 out, 26 ok).  The nine assertion
                                failures every run shares are run-ifdhcp's
                                SLIRP literals under a bridge, already a
                                backlog row, and are not LTO's
    ne2000_pcmcia, SLIRP, LTO x5  PASS, 35 ok, 0 fail, every boot, our
                                device on `card pcmcia`, 10.0.2.15 taken
                                each time.  Non-LTO control PASS, 35 ok
    a2065, SLIRP, LTO x5        PASS, 35 ok, every boot; this pair ran the
                                VENDOR a2065.device and so speaks for the
                                library only.  Non-LTO control PASS

And one bridged transfer to go with the boots: run-poolshare on the a2065
with our LTO device, peer on a third machine -- 9,232,384 bytes in 15 s,
4.92 Mbit/s, 2,254 packets, lost=0, out-of-order=0, zero_windows=2.

The bridged pcmcia combination specifically could not be run: another agent
held the bridge with its own ne2000_pcmcia guests throughout, and Amiberry
ignores `mac=` for that board, so two bridged pcmcia guests carry one MAC.
The SLIRP pcmcia arm above covers the board and the driver; what it does
not cover is that board over a real segment, which is a rig-availability
gap and not a result.

READING NOTE ON THE STAGING, because it decides what these runs prove.
`run-ifdhcp` stages whatever `AMINETXDUO_SANA2_DRIVER` points at and never
consults `sana2_select`, so an arm that does not set the SANA2 variables
boots the VENDOR driver for the board.  The first pass of these arms did
exactly that -- `driver=a2065.device source=vendor card=none` -- and was
rerun with `AMINETXDUO_SANA2_DRIVER=<build>/src/netdev/anxnet.device`,
`_DRIVER_NAME` and `_DEVICE` = `anxnet.device`, `_DIR=Networks` and
`_CARD=<card>`, the set `tests/tools/run-netcapture.sh:249` uses.  Teaching
`run-ifdhcp` to call `sana2_select` the way the sweeps do would close the
trap; until then an arm that does not print `source=anxnet` is measuring
the library alone.

Two rig facts worth carrying, neither a stack fact: 192.168.1.240 is
contested on this LAN -- on it the poolshare peer got RST then timeouts and
the arm read "the guest never accepted" while the guest sat in `iperf -s`,
and .243 already cost this campaign two runs the same way.  And
`run-poolshare.sh` derives its own tag `poolshare16`, so two agents running
it at once share a serial port AND a guest address; pass `-a` and check
`pgrep -af amiberry` first.

VERDICT: BOUNDED, WITH ONE LIVE DEFECT NAMED.  Every structural check that
can be made off-hardware passes on the LTO shipping images, at main and at
08fe2a21 both; the emulator boots them repeatedly without a hang; and the
one relocation-class defect LTO does introduce in this tree lands on test
binaries that no deployment carries.  The 08fe2a21 lock-up is therefore NOT
explained by a missing relocation table in the library or the device, and it
is not reproduced here.  It stays an unexplained fact about one physical
boot, and only the machine can settle it.

Two things that are decisions rather than findings, and are left where they
belong: whether `tests/` should link `--gc-sections` or the tree should
gate on `tools/hunkdiff.py --check` (which fails exactly those eleven
images -- the ten LTO ones and AtfTcpSocket -- and passes every other
image in both builds); and (d) itself.

### The physical test for (d), when the machine comes back

One session, in this order.  Nothing here needs a second visit.

1.  Build both arms from one tree, one flag apart, and keep both:

        cmake -S . -B build/lto \
              -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
              -DCMAKE_BUILD_TYPE=Release
        cmake -S . -B build/nolto \
              -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
              -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_LTO=OFF
        python3 tools/hunkdiff.py \
              build/lto/src/bsdsocket/bsdsocket.library \
              build/nolto/src/bsdsocket/bsdsocket.library
        python3 tools/hunkdiff.py \
              build/lto/src/netdev/anxnet.device \
              build/nolto/src/netdev/anxnet.device

    The differ must report no findings before anything is copied, and

        python3 tools/hunkdiff.py --check \
              build/lto/src/bsdsocket/bsdsocket.library \
              build/lto/src/netdev/anxnet.device \
              build/lto/src/tools/*

    must report `check=ok` for everything the deploy carries -- it says
    `check=skip` for the maps and the HTML in the same directory, so the
    glob is safe to use.  If either complains, stop: the answer is in the
    image and the machine is not needed.

2.  Park a known-good pair on DH0: FIRST, from the Shell, not from the
    host: `copy LIBS:bsdsocket.library DH0:bsdsocket.library.known` and
    `copy DEVS:Networks/anxnet.device DH0:anxnet.device.known`.  Recovery
    depends on these existing before the LTO pair is installed.

3.  Install the LTO pair -- `build/lto/src/bsdsocket/bsdsocket.library` to
    `LIBS:`, `build/lto/src/netdev/anxnet.device` to `DEVS:Networks/` --
    and reboot.  Nothing else changes: same startup-sequence, same C:
    commands, same card, same everything the non-LTO ladder ran on.

4.  Watch the BOOT, not the network.  Record which of these happens:

    - the machine reaches the Shell and the stack comes up in the usual
      ~35 s: LTO is not the lock, and step 6 is the confirmation;
    - the machine stops with a Guru: WRITE THE NUMBER DOWN.  8000 0003 is
      an address error and 8000 0004 an illegal instruction; either one
      with a PC of $ffffffff or in low memory is the unrelocated-image
      signature, which would mean step 1 missed it and the differ needs
      that case added;
    - the machine hangs with no Guru, screen up, drive quiet: that is the
      reported symptom.  Note whether it hangs BEFORE or AFTER the
      startup-sequence line that starts the stack -- before means the file
      could not even be loaded, after means it ran.

5.  Recovery, in increasing cost: reset; if it locks again, boot with both
    mouse buttons held and pick the no-startup boot, then
    `copy DH0:bsdsocket.library.known LIBS:bsdsocket.library` and
    `copy DH0:anxnet.device.known DEVS:Networks/anxnet.device`; if the
    machine will not boot at all, the CF card comes out and the pair is
    replaced from a host.  The .known copies from step 2 are what make the
    middle option work, which is why they are step 2.

6.  If it boots: five more cold boots, power off between them, and on the
    fifth run `netstat -s` and take a 10-second `iperf -s` receive from a
    host on the LAN.  The original report was one boot; five clean ones and
    a transfer is what turns "it ran" into evidence.  If any of the five
    locks, the answer is "intermittent", which is a different and more
    useful fact than "LTO is broken".

7.  Either way the outcome belongs here and in the backlog row, with the
    build that produced it named by commit.

## (e) GREEN_REALM stays OFF

`-DAMINETXDUO_GREEN_REALM` (the option-4 green-thread port) is on main via
the campaign's landing merge, default OFF, and OFF is the shipping answer.
The close-out (docs/GREEN-REALM.md cycle 4) is explicit: with the free-baton
fast path the realm is rate-NEUTRAL on hardware (+0.35%, inside the +-2%
band), architecturally superior (handoffs/frame 0.00, the mDNS storm hold
bound TIGHTER than the baton port's), and not worth shipping for speed.  It
is kept as the foundation: any future lever that needs realm structure
builds on it instead of re-deriving it.

Recommendation: no release action beyond confirming the default-off build
is what ships (verified at the landing merge: default config compiles, host
tier 98/98, bridged poolshare smoke clean).  Do not advertise the flag.

## (f) The calibration record as the reassurance line

The campaign's final measurement is the sentence a release note can carry:
**on the same machine, the same card and the same anxnet.device, this stack
matches or beats both Roadshow and AmiTCP_NG.**  Emulated three-way, one
rig, one sitting: ours 2778 > Roadshow demo 2757 > AmiTCP_NG 2142 kbit/s;
a clean-Release Roadshow arm on its own rig read +15-19% in our favour.
Physical: AmiTCP_NG 4.1.5 over our device on the real A1200+3c589 reached
1.158-1.160 Mbit/s with the same 1.31 Mbit/s steady plateau as ours -- the
ceiling is the machine's, and no foreign stack extracts more from it
(docs/RECEIVE_BUDGET.md, both calibration sections, method caveats
included there).

Scope discipline: the line holds for the measured machine and card.  The
X-Surf-100 report (their 906 KB/s vs our 412) is different hardware, a
different driver and an open backlog row -- the reassurance line must not
be stretched over it.

Recommendation: use the line, scoped exactly as above, and link the
calibration sections rather than restating numbers that carry method
caveats.
