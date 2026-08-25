#!/usr/bin/env bash
#
# Prove tests/tools/bringupfail-verdict.sh can fail.
#
#   tests/tools/bringupfail-verdict-selftest.sh
#
# The live arm (tests/tools/run-bringupfail.sh) needs a ROM, a driver and five
# boots, and two of its five causes are awkward to produce on demand -- a
# machine genuinely out of memory, and an attach cap reached on a machine with
# one card.  This drives the grader against a transcript per cause instead,
# in both the shape the contract asks for and the shape that is there today,
# so the difference between them is a recorded fact rather than an argument.
#
# THE FIXTURE THAT MATTERS is `real_addif_configure`.  It is not invented: it
# is the sentence in src/tools/addnetinterface.c today, and `strings` finds it
# in a shipping AddNetInterface.  It is the reason this file exists.
#
# Needs nothing; under a second.
#
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
. "$ROOT/tests/tools/bringupfail-verdict.sh"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# ------------------------------------------------ what the contract asks for --

# MISSING DEVICE.  Operation named, code carried, hint that a user can act on.
cat > "$T/good-nodevice" <<'EOF'
AddNetInterface: could not open nosuchcard.device unit 0 (error 32)

  eth0 asks for nosuchcard.device, and there is no such driver on this
  machine.  Drivers belong in DEVS:Networks/.  Check the DEVICE line in
  DEVS:NetInterfaces/eth0, or run NetSetup to write the file again.
EOF

# WRONG UNIT.
cat > "$T/good-badunit" <<'EOF'
AddNetInterface: could not open a2065.device unit 9 (error 32)

  a2065.device is installed and unit 9 did not open.  Almost every card is
  unit 0: change the UNIT line in DEVS:NetInterfaces/eth0 to 0.
EOF

# UNUSABLE ADDRESS.
cat > "$T/good-badaddress" <<'EOF'
AddNetInterface: could not configure eth0 with address 300.1.1.1 (error 22)

  300.1.1.1 is not an address: every part must be 0 to 255.  Change the
  ADDRESS line in DEVS:NetInterfaces/eth0, or set CONFIGURE = DHCP to have
  one handed out.
EOF

# ATTACH CAP REACHED.
cat > "$T/good-cap" <<'EOF'
AddNetInterface: could not add eth2 to the running network (error 28)

  This stack holds 2 interfaces and both are in use by eth0 and eth1.
  eth2 stays defined and is not attached.  Take one of the others offline
  with Offline, or remove its file from DEVS:NetInterfaces/.
EOF

# OUT OF MEMORY.
cat > "$T/good-nomem" <<'EOF'
AddNetInterface: could not allocate the packet pool (error 103)

  103 is "out of memory".  187392 bytes are free and the stack needs about
  450K.  Close something, or add Fast RAM to this machine.
EOF

# ----------------------------------------------- what is there today, or was --

# THE REAL ONE.  src/tools/addnetinterface.c, found by `strings` in a shipping
# build on 2026-08-25.  It fails BOTH clauses: no code on the first line, and
# it sends the reader to a log a shipping build cannot write.
cat > "$T/real_addif_configure" <<'EOF'
AddNetInterface: eth0 did not come online

  a2065.device unit 0 opens on its own, so the card and the driver are fine
  and something later in the bring-up refused.  Check the interface file for
  a wrong ADDRESS or CONFIGURE line.  Check the debug log for what failed
  after the device opened.
EOF

# The log sentence alone, with an otherwise perfect first line: proves clause 2
# is graded over the WHOLE transcript and not just the first line.
cat > "$T/logadvice-in-hint" <<'EOF'
AddNetInterface: could not open a2065.device unit 0 (error 32)

  The driver did not open.  See the debug log for the SANA-II return code.
EOF

# First line with an operation and no code.
cat > "$T/nocode" <<'EOF'
AddNetInterface: could not open a2065.device unit 0

  Check the DEVICE line in DEVS:NetInterfaces/eth0.
EOF

# First line with a code and no operation: "it failed, here is a number".
cat > "$T/nooperation" <<'EOF'
AddNetInterface: eth0 failed (error 32)

  Something went wrong bringing up eth0.
EOF

# A bring-up that failed and printed nothing a user is addressed by.  Silence
# is a separate finding from bad wording and is named separately.
cat > "$T/silent" <<'EOF'
AmiNetXDuo 0.24.0

EOF

# The words that are NOT the noun.  A check that flagged these would be turned
# off inside a week, so it must not, and this fixture is what says so.
cat > "$T/innocent-words" <<'EOF'
AddNetInterface: could not open a2065.device unit 0 (error 32)

  The login name in the dialog is not a logical device.  Catalogue the
  DEVICE line in DEVS:NetInterfaces/eth0.
EOF

# PROGRESS ECHOES BEFORE THE REFUSAL, which is what the real command does.
# The first two lines are addressed by the INTERFACE's name and are a tool
# doing its job; the refusal is the line prefixed with the COMMAND's name.
# Grading the literal first line of the OUTPUT reported this as naming no
# operation, which would put a red beside a message that is fine.
cat > "$T/progress-then-refusal" <<'EOF'
nodev: nosuchcard.device unit 0
nodev: starting the network...
AddNetInterface: could not open nosuchcard.device unit 0 (error 32)
  There is no nosuchcard.device on this machine.
EOF

# The same shape, and the refusal itself is the one that is there today: it
# names the operation and carries no code.  This is the live finding, in a
# fixture, so it is graded without a ROM.
cat > "$T/real_shape_nocode" <<'EOF'
nodev: nosuchcard.device unit 0
nodev: starting the network...
AddNetInterface: nodev was not added to the running network
  There is no nosuchcard.device on this machine.
EOF

# THE CONFIGURATION-FAULT SHAPE, which has no command prefix and no error
# number and is nonetheless the best message on this path.  Taken verbatim from
# a live run, 2026-08-25.  Grading it as silence would be this file inventing a
# defect; the path names what failed and the line number is the locator.
cat > "$T/configfault" <<'EOF'

Problems in the configuration:
  DEVS:NetInterfaces/badaddr, line 4:
      ADDRESS cannot be '300.1.1.1'
      An address is four numbers from 0 to 255 with dots between them, for
      example 192.168.1.10. Write ADDRESS=DHCP to have the address handed out
      automatically.
EOF

# The same block with the locator taken out: a path and no line, no number
# anywhere.  This is what the shape looks like when it stops being useful, and
# it must go red or the clause above means nothing.
cat > "$T/configfault-noline" <<'EOF'

Problems in the configuration:
  DEVS:NetInterfaces/badaddr:
      the interface has no address
EOF

: > "$T/empty"

n=0; bad=0
case_() { # description expected-rc expected-result transcript cause
    local what="$1" want="$2" wantr="$3" report="$4" cause="${5:-x}"
    local out rc gotr
    out=$(bringupfail_verdict "$report" "$cause" 2>&1); rc=$?
    gotr=$(printf '%s\n' "$out" | sed -n 's/^bringupfail_result=//p' | tail -1)
    n=$((n + 1))
    if [ "$rc" = "$want" ] && [ "$gotr" = "$wantr" ]; then
        printf 'ok   %-44s -> %s %s\n' "$what" "$rc" "$gotr"
    else
        printf 'FAIL %-44s -> %s %s, wanted %s %s\n' \
               "$what" "$rc" "$gotr" "$want" "$wantr"
        bad=$((bad + 1))
    fi
    printf '%s\n' "$out" | sed 's/^/       | /'
}

echo "-- the five causes, worded the way the contract asks --"
case_ "missing device"          0 PASS "$T/good-nodevice"   missing_device
case_ "wrong unit"              0 PASS "$T/good-badunit"    wrong_unit
case_ "unusable address"        0 PASS "$T/good-badaddress" unusable_address
case_ "attach cap reached"      0 PASS "$T/good-cap"        attach_cap
case_ "no memory"               0 PASS "$T/good-nomem"      no_memory

echo
echo "-- and the ways it goes wrong --"
case_ "THE SHIPPED ONE: log advice, no code" 1 FAIL "$T/real_addif_configure" shipped
case_ "log advice in the hint block"         1 FAIL "$T/logadvice-in-hint"    hint
case_ "operation named, no code"             1 FAIL "$T/nocode"               nocode
case_ "code given, no operation named"       1 FAIL "$T/nooperation"          nooperation
case_ "failed and said nothing"              1 FAIL "$T/silent"               silent
case_ "an EMPTY transcript"                  1 FAIL "$T/empty"                empty
case_ "no transcript at all"                 1 FAIL "$T/nope"                 missing

echo
echo "-- the refusal is not the first line of the output --"
case_ "progress echoes, then a good refusal"  0 PASS "$T/progress-then-refusal" progress
case_ "THE LIVE SHAPE: named, no code"        1 FAIL "$T/real_shape_nocode"     liveshape

echo
echo "-- a configuration fault has no prefix and no number --"
case_ "path and line, no verb and no code"    0 PASS "$T/configfault"        configfault
case_ "path, and no locator at all"           1 FAIL "$T/configfault-noline" noline

echo
echo "-- and the words that must NOT trip it --"
case_ "login, dialog, logical, catalogue"    0 PASS "$T/innocent-words"       innocent

echo
echo "bringupfail-verdict-selftest: $n cases, $bad wrong"
[ "$bad" -eq 0 ]
