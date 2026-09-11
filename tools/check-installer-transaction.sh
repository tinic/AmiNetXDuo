#!/usr/bin/env bash
#
# THE INSTALLER MUST NOT MOVE A LIVE FILE ASIDE BEFORE IT HAS THE NEW ONE.
#
# A library that is open cannot be written over, so the installer moves the
# live one aside and copies the replacement in.  Done in that order it leaves a
# window with no LIBS:bsdsocket.library at all, and anything that ends the copy
# inside that window -- a full disk, a media error, or the user answering no to
# the copy's own (confirm) requester -- leaves the machine without the library
# its network IS.  It is not a disk-full-only hazard: a declined requester is
# one click.
#
# So the order is fixed, per replaced file:
#
#   1 copy      the new file in beside the live one, under a .new name
#   2 guard     (if (exists <X>_NEW ...)) -- nothing below runs without it
#   3 rename    the live file to .old
#   4 activate  the .new file to the live name
#
# This proves the ORDER, on every commit, without an emulator.
#
# A dynamic round was tried first and thrown away: it planted a library, took
# the new one out of the archive so the copy could not succeed, and checked
# the planted one survived.  It PASSED against the old order too -- the
# Installer gives up on an incomplete archive long before it reaches the
# library section, so the renames never ran and the file survived for a reason
# that had nothing to do with the fix.  A check that passes either way is
# worse than no check.  Making it discriminate needs a failure AT the copy (a
# full volume, a write-protected LIBS:), which is not cheap to arrange inside
# the emulator; if someone builds that, it belongs beside this, not instead.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT="$ROOT/install/Install-AmiNetXDuo"

[ -f "$SCRIPT" ] || { echo "installer_txn=skipped reason=no_script"; exit 2; }

python3 - "$SCRIPT" <<'PY'
import re, sys

path = sys.argv[1]
lines = open(path, errors='ignore').read().split('\n')

# var, the file name the copy lands under
WATCH = [
    ("D_BSD", "bsdsocket.library"),
    ("D_UG",  "usergroup.library"),
    ("D_TLS", "tls.library"),
    ("D_ANX", "anxnet.device"),
]

def all_lines(pattern):
    rx = re.compile(pattern)
    return [i for i, l in enumerate(lines, 1) if rx.search(l)]

def first(pattern):
    hits = all_lines(pattern)
    return hits[0] if hits else 0

def last_before(pattern, limit):
    """The nearest match ABOVE `limit`.

    There are two `(if (exists <X>_NEW` in the script: the one that clears a
    leftover .new before the copy, and the one that guards the swap.  Taking
    the first would compare the wrong one and fail a correct script."""
    hits = [h for h in all_lines(pattern) if h < limit]
    return hits[-1] if hits else 0

bad = 0
for var, name in WATCH:
    copy     = first(r'\(newname\s+"%s\.new"\)' % re.escape(name))
    rename   = first(r'\(rename\s+%s\s+%s_OLD\b' % (re.escape(var), re.escape(var)))
    activate = first(r'\(rename\s+%s_NEW\s+%s\b' % (re.escape(var), re.escape(var)))
    guard    = last_before(r'\(if\s+\(exists\s+%s_NEW\b' % re.escape(var),
                           rename or len(lines))

    print("installer_txn file=%-18s copy=%-4s guard=%-4s rename=%-4s activate=%s"
          % (name, copy or "-", guard or "-", rename or "-", activate or "-"))

    if not copy or not guard or not rename or not activate:
        print("  !! %s: the staged-then-swapped shape is not there" % name)
        bad = 1
        continue
    if not (copy < guard < rename < activate):
        print("  !! %s: out of order.  The live file must not be renamed "
              "until the new one is on the disk and the guard has seen it."
              % name)
        bad = 1

print("installer_txn=%s" % ("FAIL" if bad else "PASS"))
sys.exit(1 if bad else 0)
PY
