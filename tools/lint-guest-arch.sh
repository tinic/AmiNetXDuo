#!/usr/bin/env bash
#
# Refuse a hardcoded -m680x0 in a script that builds a GUEST binary for a
# machine it also chooses.
#
# THE BUG THIS EXISTS FOR, three times over.  A harness compiles a small helper
# for the emulated machine, hardcodes -m68020, and is then pointed at an A600.
# The helper stops on an illegal instruction inside its own C constructor
# before one line of the thing under test runs: no serial, no stdout.txt, the
# machine idling at 50 fps.  It reads as "the stack does not work on a 68000"
# and costs a day.
#
#   clients/dropbear/run-dbclient.sh  fixed, RUNNER_ARCH
#   tools/amiberry-run.sh           fixed, ENVSETUP_ARCH
#   tests/compare/run-compare.sh    NOT fixed until 2026-08-07, and it is the
#                                   harness every measurement in this tree runs
#                                   through.  CheckRunner carried `tst.l a0`.
#
# The pattern is always the same and is in amiberry-run.sh:
#
#     case "${CPU:-}${MODEL:-}" in
#         *68000*|*A500*|*A600*|*A2000*) ARCH="-m68000" ;;
#         *)                             ARCH="-m68020" ;;
#     esac
#
# A script with no MODEL and no CPU is fixed-machine and exempt; name it below
# with the reason.
#
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 2

# Fixed-machine harnesses, with why.
exempt() {
    case "$1" in
        # Enforcer needs a real MMU, so this is A1200/68030 and can never be a
        # 68000 (tools/enforcer-run.sh:17).
        tools/enforcer-run.sh) return 0 ;;
        # The floor for a ported client is documented as 68020; overridable
        # with AMIGA_CLIENT_ARCH.
        clients/amiga-client.sh) return 0 ;;
        # Installs a release archive on a real Workbench 3.1 image; the drive
        # level is the variable, not the CPU, and it builds -m68000 already.
        install/test/run-workbench.sh) return 0 ;;
        # Never passes -m to tools/amiberry-run.sh, so it gets that script's
        # default machine and the 020 build matches it.
        install/test/run-all.sh) return 0 ;;
        # Feeds the ported clients, where 68020 is the documented floor and
        # AMIGA_CLIENT_ARCH is the override (clients/amiga-client.sh).
        tests/clients/build.sh) return 0 ;;
    esac
    return 1
}

rc=0
found=0

while IFS= read -r f; do
    exempt "$f" && continue

    # Scripts that choose a machine, AND scripts that only BUILD for one -- the
    # model those binaries meet is chosen by whoever runs them, so a build
    # script cannot be cleared by looking at itself.  tests/soak/build-fitz-soak.sh
    # and tests/endurance/build.sh were both invisible to the first version of
    # this check for exactly that reason, and both broke an A600 run.
    grep -qE '^[[:space:]]*(MODEL|CPU)=|-m[[:space:]]*MODEL|\bMODEL\b' "$f" ||
        grep -qE 'build\.sh$|build-.*\.sh$' <<<"$f" ||
        case "$f" in */build*.sh) ;; *) continue ;; esac

    # A literal -m680x0 on a compiler line, not in a comment or a case arm.
    hits=$(grep -nE '^[^#]*\$\{?[A-Za-z_]*GCC[A-Za-z_]*\}?[^#]*-m68[0-9]{3}' "$f" || true)
    [ -n "$hits" ] || continue

    found=1
    rc=1
    echo "$f"
    printf '%s\n' "$hits" | sed 's/^/    /'
done < <(git ls-files '*.sh' 2>/dev/null)

if [ "$found" = "0" ]; then
    echo "guest-arch: no hardcoded -m680x0 in a model-aware script"
    exit 0
fi

cat <<'EOF'

A guest binary must be built for the machine the script was asked to run, not
for the newest one.  Derive it:

    case "${CPU:-}${MODEL:-}" in
        *68000*|*A500*|*A600*|*A2000*) ARCH="-m68000" ;;
        *)                             ARCH="-m68020" ;;
    esac

and key any cached output on the architecture so a binary built for one machine
is never reused on another.  If the script is genuinely fixed-machine, add it to
exempt() above WITH the reason.
EOF
exit "$rc"
