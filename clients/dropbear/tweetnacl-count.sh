#!/usr/bin/env bash
#
# Count the field multiplications in one SSH handshake, on the build host.
#
#   clients/dropbear/tweetnacl-count.sh
#
# WHY THIS IS NOT AN EMULATOR RUN
#
#   A timing run has to take the machine alone and the queue is deep, so a
#   slot is worth spending only on something a 68020 can answer and a Mac
#   cannot.  An operation count is not that: 2^255-19 arithmetic
#   executes the same multiplies everywhere.  The guest supplies milliseconds
#   per primitive, this supplies multiplies per primitive, and the quotient --
#   the cost of one field multiply on this part, is what every proposal to
#   make SSH faster has to be argued against.
#
# HOW IT REACHES A `static` FUNCTION WITHOUT PATCHING THE SUBMODULE
#
#   third_party/dropbear is unpatched and clients/dropbear/build.sh refuses to
#   build if it is not.  So the two definitions that matter are renamed in a
#   DERIVED COPY under build/, with counting macros of the original names put
#   directly after them.  TweetNaCl is written bottom-up, every use of M() and
#   S() is below their definitions, so every use in the file goes through the
#   counter, including S()'s own call to M().
#
#   The copy is regenerated on every run and the sed is checked for having hit
#   something, so this cannot silently drift from the pinned tag.
#
# WHAT IT LINKS AGAINST
#
#   build/dropbear-host, the native Dropbear that clients/dropbear/
#   sshd-testserver.sh already builds for dropbearkey.  Its config.h and its
#   libtomcrypt (for SHA-512) are the same source at the same tag as the
#   Amiga's, so the counts are of the code the Amiga runs.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DB="$ROOT/third_party/dropbear"
HOST="$ROOT/build/dropbear-host"
OUT="$ROOT/build/tweetnacl-count"

[ -f "$HOST/config.h" ] || {
    echo "!! no native Dropbear at build/dropbear-host." >&2
    echo "   clients/dropbear/sshd-testserver.sh start builds it." >&2
    exit 2
}

mkdir -p "$OUT"
SRC="$OUT/curve25519_counted.c"

# The rename.  Both definitions are `sv NAME(...)` at the start of a line --
# `sv` is TweetNaCl's own `static void` shorthand, and nothing else in the
# file matches, which is what makes a two-line sed safe here.
sed -e 's/^sv M(gf o,const gf a,const gf b)$/sv tn_M_real(gf o,const gf a,const gf b)/' \
    -e 's/^sv S(gf o,const gf a)$/sv tn_S_real(gf o,const gf a)/' \
    "$DB/src/curve25519.c" > "$SRC"

grep -q '^sv tn_M_real' "$SRC" || { echo "!! M() not found, did curve25519.c change?" >&2; exit 1; }
grep -q '^sv tn_S_real' "$SRC" || { echo "!! S() not found, did curve25519.c change?" >&2; exit 1; }

# Insert the counters immediately after S()'s body.  S() is defined right below
# M() and is the last of the two, so one insertion point covers both, and it
# is above every remaining use in the file.
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()

# The two counters go at the top, above every use.  `typedef i64 gf[16];` is
# TweetNaCl's field type and the first line after which they are legal.
anchor = 'typedef i64 gf[16];'
if anchor not in s:
    sys.exit('gf typedef not found, did curve25519.c change?')
s = s.replace(anchor, anchor + '\nunsigned long tn_count_M, tn_count_S;', 1)

# S()'s body was written before the macros exist, so its own call to M() is the
# real one and would go uncounted.  A squaring IS a multiply and the total has
# to say so, so it is counted by hand here.
body = 'sv tn_S_real(gf o,const gf a)\n{\n  M(o,a,a);\n}'
if body not in s:
    sys.exit('S() body not as expected, did curve25519.c change?')
s = s.replace(body,
              'sv tn_S_real(gf o,const gf a)\n{\n  tn_count_M++;\n'
              '  tn_M_real(o,a,a);\n}\n'
              '/* ---- inserted by clients/dropbear/tweetnacl-count.sh ---- */\n'
              '#define M(o,a,b) (tn_count_M++, tn_M_real((o),(a),(b)))\n'
              '#define S(o,a)   (tn_count_S++, tn_S_real((o),(a)))\n'
              '/* --------------------------------------------------------- */\n',
              1)
open(p, 'w').write(s)
PY
grep -q '#define M(o,a,b)' "$SRC" || { echo "!! counter insertion failed" >&2; exit 1; }

CC="${CC:-cc}"
INC="-I$HOST -I$DB/src -I$DB/libtomcrypt/src/headers -I$DB/libtommath"

echo "==> building the counter"
$CC -O1 -w $INC -DLOCALOPTIONS_H_EXISTS -DDROPBEAR_CLIENT \
    -o "$OUT/tweetnacl-count" \
    "$SRC" "$ROOT/clients/dropbear/tweetnacl-count.c" \
    "$HOST/libtomcrypt/libtomcrypt.a" "$HOST/libtommath/libtommath.a" \
    2>&1 | grep -v '^$' || true

[ -x "$OUT/tweetnacl-count" ] || { echo "!! build failed" >&2; exit 1; }

echo
exec "$OUT/tweetnacl-count"
