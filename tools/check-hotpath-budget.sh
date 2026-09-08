#!/usr/bin/env bash
#
# The per-frame receive path must not grow.
#
#   tools/check-hotpath-budget.sh [build-dir]
#
# WHY COUNTING AND NOT MEASURING.  Removing n68k_rx_verify_sum whole measured
# +3.40% (p=0.0019, 12 rounds an arm) for ~118 instructions on its fused fast
# path, which prices ONE INSTRUCTION AT ~0.029% OF RECEIVE.  The rig resolves
# about 0.9% at twenty boots an arm.
#
# So a change that adds twenty instructions to a per-frame function costs
# ~0.6% and NO A/B ON THIS RIG WILL EVER SEE IT, and several of them together
# are a percent of throughput nobody watched leave.  Every function below was
# read line by line during the 2026-09-07/08 audit and found already tight; a
# counted ceiling is what keeps them that way, because it is the only
# instrument with the resolution.
#
# It counts the WHOLE function, not the fast path: every branch is reachable,
# a cold path that doubles is still a defect, and picking the fast path out of
# assembly needs a judgement this cannot make.
#
# RAISING A BUDGET IS A DECISION, NOT MAINTENANCE.  Each one is ~0.029% of
# receive per instruction.  Put the reason in the commit.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

BUILD="${1:-build/cm}"
CCJSON="$BUILD/compile_commands.json"

if [ ! -r "$CCJSON" ]; then
    echo "hotpath_budget=skipped reason=no_compile_commands path=$CCJSON"
    exit 0
fi

# SHIPPING-SHAPED ARMS ONLY, and the numbers below are why.
#
# The counts are per ARM, not per tree: _n68k_rx_verify_sum is 220 on the
# `default` cross arm and 214 on the profiler arm, because the options change
# what is compiled.  A ceiling read against the wrong arm is comparing two
# different programs, and it fails or passes for reasons that have nothing to
# do with the change under review.
#
# check-hot-calls.sh already refuses instrumented arms for the same reason and
# this did not, which is a hole: it accepted build/wp2 and answered 214/230
# with a straight face.  The budgets were taken on `default`, so that is the
# only shape they mean anything against.
CACHE="$BUILD/CMakeCache.txt"
if [ -r "$CACHE" ]; then
    for opt in PROFILER PROFILER_NOINLINE RXPROBE NXCENSUS SCHEDCOUNT \
               SANA2_PROBE_RAW NETDEV_TIME ALLOCCENSUS NX_COUNTERS; do
        if grep -q "^AMINETXDUO_$opt:BOOL=ON" "$CACHE"; then
            echo "hotpath_budget=skipped reason=instrumented opt=$opt"
            exit 0
        fi
    done
    if grep -q "^AMINETXDUO_LTO:BOOL=OFF" "$CACHE"; then
        echo "hotpath_budget=skipped reason=lto_off"
        exit 0
    fi
fi

python3 - "$CCJSON" <<'PY'
import json, re, subprocess, sys, tempfile, os

ccjson = sys.argv[1]

# tu : [(function, budget)].  Counts on the `default` cross arm, 2026-09-08.
TABLE = {
    "n68k_rx_verify.c": [("_n68k_rx_verify_sum",     230)],   # 220
    "sana2_rx.c":       [("_ami_sana2_rx_thread",    385),    # 366, carries the
                         ("_ami_sana2_rx_deliver",    95),    #  inlined drain
                         ("_ami_sana2_rx_post_slot",  70)],   #  loop and
                                                             #  rx_complete
    "netdev_device.c":  [("_netdev_rx",              168),    # 155
                         ("_netdev_tx_direct",       108),    # 98, the ACK path
                         ("_netdev_hand_over",        68)],   # 60

    # THE TCP CORE IS VENDORED, WHICH IS WHY IT IS HERE AND NOT WHY IT IS NOT.
    # These four run once per received segment and are 9.2% of the profile
    # between them -- the largest block on the path after the copies, and the
    # only one nothing was watching.  A NetX version bump is a normal thing to
    # do here (d1253f11 bumped it for the window-update knob) and it can add
    # instructions to the hottest per-segment function in the stack with
    # nobody the wiser, because the rig cannot see 0.6% and these are not our
    # files to read line by line every release.
    #
    # A bump that trips this is NOT a defect -- it is a number to look at and
    # then raise on purpose.
    "nx_tcp_socket_state_data_check.c":  [("__nx_tcp_socket_state_data_check",  467)],  # 429
    "nx_tcp_packet_process.c":           [("__nx_tcp_packet_process",           546)],  # 502
    "nx_tcp_socket_packet_process.c":    [("__nx_tcp_socket_packet_process",    515)],  # 474
    "nx_tcp_socket_state_ack_check.c":   [("__nx_tcp_socket_state_ack_check",   604)],  # 556
}

try:
    cc = json.load(open(ccjson))
except Exception:
    print("hotpath_budget=skipped reason=unreadable_compile_commands")
    sys.exit(0)

# A label at column one starting with an underscore ends a body on this
# target; local labels are L1, L2 and do not.  There is NO .size and NO .type
# in this assembler's output, so a directive-only rule runs to end of file for
# every static function -- it counted four functions as one (180 against 62).
END_LABEL = re.compile(r"^_[A-Za-z]")
END_DIRECTIVE = re.compile(r"^[ \t]*\.(size|globl|type)")
INSTRUCTION = re.compile(r"^[ \t]+[a-z]")

def count(lines, fn):
    seen = False
    n = 0
    for line in lines:
        if line == fn + ":":
            seen = True
            continue
        if seen:
            if END_LABEL.match(line) or END_DIRECTIVE.match(line):
                break
            if INSTRUCTION.match(line):
                n += 1
    return (n if seen else None)

rc = 0
rows = []
tmp = tempfile.mkdtemp()
for tu, wanted in TABLE.items():
    hits = [x for x in cc if x["file"].endswith("/" + tu) or x["file"].endswith(tu)]
    if not hits:
        print("hotpath_budget=skipped reason=tu_not_in_build tu=%s" % tu)
        sys.exit(0)
    e = hits[0]
    out = os.path.join(tmp, tu + ".s")
    cmd = re.sub(r"-o\s+\S+", "-o " + out, e["command"]).replace(" -c ", " -S ")
    # -flto makes -S emit GIMPLE, and the symbols are then not there at all.
    cmd = re.sub(r"\s-flto(=\S+)?", " ", cmd) + " -fno-lto"
    r = subprocess.run(cmd, shell=True, cwd=e["directory"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("hotpath_budget=FAIL reason=compile_failed tu=%s" % tu)
        print(r.stderr[:400])
        sys.exit(1)
    lines = open(out).read().split("\n")
    for fn, budget in wanted:
        n = count(lines, fn)
        if n is None:
            print("hotpath_budget=FAIL reason=symbol_not_found sym=%s tu=%s"
                  % (fn, tu))
            print("  A name this gate cannot find is a name it cannot guard:")
            print("  the count would be zero and the budget would pass.")
            sys.exit(1)
        rows.append((fn, n, budget))
        if n > budget:
            over = n - budget
            print("hotpath_budget=FAIL sym=%s instructions=%d budget=%d"
                  % (fn, n, budget))
            print("  %s grew %d past its ceiling, about %.2f%% of receive,"
                  % (fn, over, over * 0.029))
            print("  which is BELOW what the rig can measure -- which is why")
            print("  it is counted instead.  Raise the budget only with a")
            print("  reason in the commit.")
            rc = 1

if rc == 0:
    print("hotpath_budget=PASS " +
          " ".join("%s=%d/%d" % (f.lstrip("_"), n, b) for f, n, b in rows))
sys.exit(rc)
PY
