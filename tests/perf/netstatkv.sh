#!/usr/bin/env bash
# Turn `netstat -s` prose into key=value, so a harness never greps prose.
#
#   . tests/perf/netstatkv.sh
#   netstat_kv <file> [prefix]        one key=value per line on stdout
#
# The receive-budget legs only carry counts in a library built with
# -DAMINETXDUO_RXPROBE=ON.  Any other build answers the selector and prints
# "not instrumented", which comes out here as probe=0 and no leg keys, so a
# caller can tell "the leg is zero" from "the leg was never measured".
# SPDX-License-Identifier: MIT

netstat_kv() { # file [prefix]
    awk -v pfx="${2:-}" '
    /^[[:space:]]+[0-9]+ of [0-9]+ packets free/ {
        print pfx "pool_free="       $1
        print pfx "pool_total="      $3
        print pfx "pool_low="        $6
        print pfx "pool_bytes_each=" $9
        next
    }
    /^[[:space:]]+[0-9]+ found the pool empty/ {
        print pfx "pool_empty="  $1
        print pfx "pool_waited=" $6
        print pfx "pool_double=" $8
        next
    }
    /^[[:space:]]+[0-9]+ bytes of system memory free/ {
        print pfx "mem_free="    $1
        print pfx "mem_largest=" $7
        next
    }
    /^[[:space:]]+direct: [0-9]+ completed on the IP thread/ {
        print pfx "rx_direct="   $2
        print pfx "rx_fallback=" $8
        next
    }
    /^receive budget:/ { budget = 1; next }
    /^[[:space:]]+not instrumented/ { if (budget) print pfx "probe=0"; next }
    /^[^[:space:]]/    { budget = 0 }
    budget && /^[[:space:]]+[a-z]+,.*: no samples$/ {
        leg = $1; sub(",", "", leg)
        print pfx "probe=1"
        print pfx "leg_" leg "_samples=0"
        next
    }
    budget && /^[[:space:]]+[a-z]+,.*: [0-9]+ samples, mean [0-9]+ us, max [0-9]+ us$/ {
        leg = $1; sub(",", "", leg)
        # "<name> : N samples, mean M us, max X us" -- the label is prose of
        # unknown width, so every field is counted back from the end.
        print pfx "probe=1"
        print pfx "leg_" leg "_samples=" $(NF-7)
        print pfx "leg_" leg "_mean_us=" $(NF-4)
        print pfx "leg_" leg "_max_us="  $(NF-1)
        next
    }
    ' "$1" | awk '!seen[$0]++ || $0 !~ /probe=/'
}

# tools.txt holds one "===== <command> =====" block per staged command.  Print
# the Nth block whose header matches a pattern, 1-based.
netstat_kv_block() { # file pattern n
    awk -v pat="$2" -v want="${3:-1}" '
    /^===== / { inblk = (index($0, pat) > 0); if (inblk) n++; }
    inblk && n == want { print }
    ' "$1"
}
