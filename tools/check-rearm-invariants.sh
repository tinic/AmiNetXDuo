#!/usr/bin/env bash
#
# The re-armed receive request's HOISTED fields must stay hoistable.
#
#   tools/check-rearm-invariants.sh
#
# WHY THIS EXISTS.  ami_sana2_rx_post_slot() (src/sana2/sana2_rx.c) used to
# write NINE fields of the IOSana2Req once a frame, and _ami_sana2_rx_post_slot
# carries 2.6 per cent of the real-path receive profile for a body that copies
# no packet data at all.  Four of the nine were re-establishing values that
# were already correct, so they moved to where the slot is built.
#
# THAT IS ONLY TRUE WHILE NOTHING WRITES THEM DURING THE ROUND TRIP.  It was
# established by reading every assignment in src/netdev on 2026-09-07: the one
# request field the device assigns is ln_Type, which Exec's ReplyMsg() clobbers
# and which post_slot therefore still restores.  A device that later starts
# writing io_Command, ios2_Data or mn_ReplyPort would break the receive path in
# a way no host test sees, because the host tier's BeginIO() is a no-op stub
# (tests/sana2/host/test_sana2_rx_host.c:49) and never writes anything back.
#
# So this gate reads the DEVICE side, which is the side that would change.  If
# a write appears here it is not necessarily wrong -- it means the hoist is no
# longer safe and post_slot has to take that field back.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

rc=0

# The fields ami_sana2_rx_post_slot() no longer writes per frame.
HOISTED='io_Command|ios2_Data|mn_ReplyPort'

# An assignment to one of them, discounting == and >= and friends.  ios2_Data
# is matched on the word boundary so ios2_DataLength -- which the device DOES
# write, and which post_slot still resets -- does not trip it.
hits=$(grep -rnE "(\.|->)(${HOISTED})\b[[:space:]]*=[^=]" \
           src/netdev/*.c src/netdev/*.h 2>/dev/null) || true

if [ -n "$hits" ]; then
    echo "check-rearm-invariants: the device writes a field the receive re-arm"
    echo "  hoisted out of the per-frame path (sana2_rx.c, ami_sana2_rx_post_slot)."
    echo "  Either drop the write, or put that field back in the re-arm:"
    echo "$hits" | sed 's/^/    /'
    rc=1
fi

# The other half of the same claim: post_slot must still restore what the round
# trip really does disturb.  ln_Type is the one the device assigns.
for f in ln_Type io_Flags io_Error ios2_WireError ios2_PacketType ios2_DataLength; do
    if ! grep -qE "slot->req\..*\b${f}\b[[:space:]]*=" src/sana2/sana2_rx.c; then
        echo "check-rearm-invariants: ami_sana2_rx_post_slot no longer restores ${f}."
        echo "  The device or Exec writes it during the round trip."
        rc=1
    fi
done

[ "$rc" = 0 ] && echo "check-rearm-invariants: ok"
exit "$rc"
