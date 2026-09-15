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
#   1 clear     any .new left by an interrupted earlier run
#   2 reject    the run if that fixed staging name still exists
#   3 copy      the new file in beside the live one, under the clean name
#   4 guard     (if (exists <X>_NEW ...)) -- nothing below runs without it
#   5 rename    the live file to .old
#   6 activate  the .new file to the live name
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

# (live var, .new var, .old var, the newname the copy lands under, label)
#
# The three libraries are spelled out in the script with D_<X>, D_<X>_NEW and
# D_<X>_OLD.  The two SANA-II drivers go through ONE procedure,
# P_install_device, whose variables are dev_dest / dev_new / dev_old and whose
# newname is (cat dev_file ".new"); the shape is checked once in the
# procedure body and the calls below check that each driver goes through it.
WATCH = [
    ("D_BSD", "D_BSD_NEW", "D_BSD_OLD", r'"bsdsocket\.library\.new"', "bsdsocket.library"),
    ("D_UG",  "D_UG_NEW",  "D_UG_OLD",  r'"usergroup\.library\.new"', "usergroup.library"),
    ("D_TLS", "D_TLS_NEW", "D_TLS_OLD", r'"tls\.library\.new"',       "tls.library"),
    ("dev_dest", "dev_new", "dev_old",  r'\(cat dev_file "\.new"\)',  "P_install_device"),
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

def first_between(pattern, start, limit):
    hits = [h for h in all_lines(pattern) if start < h < limit]
    return hits[0] if hits else 0

bad = 0
for var, new_var, old_var, newname_rx, name in WATCH:
    copy     = first(r'\(newname\s+%s\)' % newname_rx)
    rename   = first(r'\(rename\s+%s\s+%s\b' % (re.escape(var), re.escape(old_var)))
    activate = first(r'\(rename\s+%s\s+%s\b' % (re.escape(new_var), re.escape(var)))
    pre      = [h for h in all_lines(r'\(if\s+\(exists\s+%s\b' % re.escape(new_var))
                if h < copy]
    clear    = pre[0] if pre else 0
    reject   = pre[-1] if len(pre) >= 2 else 0
    abort    = first_between(r'\(abort\b', reject, copy) if reject else 0
    guard    = last_before(r'\(if\s+\(exists\s+%s\b' % re.escape(new_var),
                           rename or len(lines))

    print("installer_txn file=%-18s clear=%-4s reject=%-4s copy=%-4s "
          "guard=%-4s rename=%-4s activate=%s"
          % (name, clear or "-", reject or "-", copy or "-", guard or "-",
             rename or "-", activate or "-"))

    if not clear or not reject or not abort or not copy or not guard or not rename or not activate:
        print("  !! %s: the staged-then-swapped shape is not there" % name)
        bad = 1
        continue
    if not (clear < reject < abort < copy < guard < rename < activate):
        print("  !! %s: out of order.  The live file must not be renamed "
              "until the staging name was cleared, the new file is on the "
              "disk, and the guard has seen it."
              % name)
        bad = 1

# Every driver image the archive carries goes through the procedure: a
# (set dev_file "<name>") followed by a (P_install_device) call, inside the
# DO_ANXNET yes.  A device added to dist/make-dist.sh's DEVICES without a
# call here would ship and never install, which is the silence this whole
# gate exists for.
for name in ("anxnet.device", "anxgenet.device"):
    setline = first(r'\(set\s+dev_file\s+"%s"\)' % re.escape(name))
    call    = first_between(r'\(P_install_device\)', setline, setline + 12) if setline else 0
    print("installer_txn file=%-18s via=P_install_device set=%-4s call=%s"
          % (name, setline or "-", call or "-"))
    if not setline or not call:
        print("  !! %s: not installed through P_install_device" % name)
        bad = 1
print("installer_txn=%s" % ("FAIL" if bad else "PASS"))
sys.exit(1 if bad else 0)
PY
