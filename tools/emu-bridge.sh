#!/usr/bin/env bash
#
# Is the guest on the BRIDGE, or on amiberry's user-mode NAT?
#
#   . tools/emu-bridge.sh
#   emu_bridge_assert "$SERIAL" "$RUNLOG"     # fails the run on SLIRP
#   emu_bridge_addr   "$SERIAL"               # the address, or empty
#
#   tools/emu-bridge.sh <log>...              # same check, standalone
#
# WHY THIS EXISTS.  amiberry's `sana2=true' is a bare bool and uaenet.device
# has no backend option, so a config that names no bridge gets USER-MODE NAT
# and says nothing about it.  A bridge that fails to attach falls back the same
# way.  The guest boots, DHCPs, pings, mounts and transfers -- everything
# works, and every throughput number is the NAT's.
#
# MEASURED 2026-09-13, the run that caused this file.  Same stack, same boot,
# 32 MB SMB2 read: 18 s on SLIRP against 3 s bridged, and iperf 38.7 Mbit/s
# against 133.4.  A whole investigation was spent explaining the 18 s -- a
# packet capture taken on the host's own interface reads the OUTSIDE of the
# NAT, so the guest appears to sit idle for 34 ms per read and a plausible
# story about the SMB client's think time follows from it.  None of it was
# real.
#
# THE TELL IS THE ADDRESS.  SLIRP hands out 10.0.2.15 with router 10.0.2.2 and
# name server 10.0.2.3, every time.
#
# FAILS CLOSED.  A log with no address at all is a FAILURE, not a pass: a gate
# that cannot see what it is gating must say so.  Set AMINETXDUO_ALLOW_SLIRP=1
# to run on the NAT deliberately -- correctness tests do not care, only
# throughput does.
#
# SPDX-License-Identifier: MIT

# The address the guest actually got.  Both spellings: ours prints
# "eth0: online, address 192.168.1.140", Roadshow prints
# "Interface "eth0" configured, address = 192.168.1.169, network mask ...".
emu_bridge_addr() {
    local f
    for f in "$@"; do
        [ -f "$f" ] || continue
        sed -n -e 's/.*online, address \([0-9][0-9.]*\).*/\1/p' \
               -e 's/.*configured, address = \([0-9][0-9.]*\).*/\1/p' \
               -e 's/.*Local host address *= *\([0-9][0-9.]*\).*/\1/p' \
            "$f" 2>/dev/null | head -1
    done | head -1
}

# 0 when bridged, 2 when on the NAT or when no address could be found.
emu_bridge_assert() {
    local addr
    addr=$(emu_bridge_addr "$@")

    case "$addr" in
        10.0.2.*)
            if [ "${AMINETXDUO_ALLOW_SLIRP:-0}" = 1 ]; then
                echo "!! guest is on SLIRP ($addr); AMINETXDUO_ALLOW_SLIRP=1," \
                     "so continuing. THROUGHPUT FROM THIS RUN IS NOT A" \
                     "MEASUREMENT." >&2
                return 0
            fi
            echo "!! THE GUEST IS ON AMIBERRY'S USER-MODE NAT, NOT THE BRIDGE." >&2
            echo "   address $addr (SLIRP hands out 10.0.2.15/10.0.2.2/10.0.2.3)." >&2
            echo "   Every throughput number from this run is the NAT's: measured" >&2
            echo "   6x slow on SMB2 and 3.4x on raw TCP, 2026-09-13." >&2
            echo "   uaenet.device with only 'sana2=true' has NO backend and always" >&2
            echo "   lands here. Boot an emulated card instead -- in" >&2
            echo "   install/test/run-smbmount.sh that is AMINETXDUO_SMB_BOARD=<card>," >&2
            echo "   which writes the board's 'mac=<mac>,<iface>' line; the trailing" >&2
            echo "   interface IS the bridge." >&2
            echo "   AMINETXDUO_ALLOW_SLIRP=1 runs on the NAT on purpose." >&2
            return 2 ;;
        "")
            echo "!! no guest address in: $*" >&2
            echo "   This gate cannot tell a bridge from the NAT without one, and" >&2
            echo "   a gate that cannot see must fail rather than pass." >&2
            return 2 ;;
        *)
            echo "==> bridged, guest address $addr"
            return 0 ;;
    esac
}

# Standalone: tools/emu-bridge.sh <log>...
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    [ $# -gt 0 ] || { echo "usage: $0 <log>..." >&2; exit 2; }
    emu_bridge_assert "$@"
fi
