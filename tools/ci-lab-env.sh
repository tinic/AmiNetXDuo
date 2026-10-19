#!/usr/bin/env bash
#
# The lab environment for a self-hosted Emulator job (#56).
#
#   tools/ci-lab-env.sh
#
# Copies what ~/amiga-assets/env.sh defines into $GITHUB_ENV, for the steps
# after this one.  @reboot sourced env.sh and keepalive.sh did not, so oommsg
# ran or skipped by accident.  Only names that are empty here are filled: a
# repository variable, or the listener's own value, wins.  A step-level `env:`
# still beats this.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

f="$HOME/amiga-assets/env.sh"
if [ ! -f "$f" ]; then
    if [ "${AMINETXDUO_LAB_RIG:-}" = 1 ]; then
        echo "::error::$f is missing on the lab rig"; exit 1
    fi
    echo "no $f, nothing to load"; exit 0
fi
# Only what env.sh itself defines: clear the inherited names first.
# NUL-separated so a value may hold spaces, brackets or newlines; names are
# logged, values never are.
bash -c 'for v in $(compgen -v AMINETXDUO_); do unset "$v"; done
         . "$1" >/dev/null
         for v in $(compgen -v AMINETXDUO_); do
           printf "%s\0%s\0" "$v" "${!v}"
         done' _ "$f" |
while IFS= read -r -d '' name && IFS= read -r -d '' value; do
    case "$name" in
        *TOKEN*|*PASS*|*SECRET*|*KEY*|*CRED*)
            echo "$name: not exported (credential-shaped name)"; continue ;;
    esac
    if [ -n "${!name:-}" ]; then
        echo "$name: kept the job's value"
        continue
    fi
    # GitHub's multi-line form, with a delimiter the value cannot hold.
    eof="EOF_$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')"
    case "$value" in *"$eof"*) echo "::error::$name: delimiter clash"; exit 1 ;; esac
    printf '%s<<%s\n%s\n%s\n' "$name" "$eof" "$value" "$eof" >> "$GITHUB_ENV"
    echo "$name: from env.sh"
done
