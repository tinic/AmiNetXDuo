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
