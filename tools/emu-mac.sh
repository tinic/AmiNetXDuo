#!/usr/bin/env bash
#
# One MAC address per run tag, for every harness that puts a guest on the
# bridge.
#
#   . tools/emu-mac.sh
#   MAC=$(emu_mac_for_tag "$TAG")
#
# WHY IT IS DERIVED AND NOT PINNED.  A harness with a fixed address puts every
# one of its runs on the LAN under the same hardware address, beside the demo
# instance and beside other checkouts' guests.  Two machines at two addresses
# sharing one hardware address is not a collision the emulator reports: the
# frames arrive, a peer's neighbour cache keeps whichever answered last, and
# what fails is a later assertion somewhere else -- an arp table with the wrong
# owner in it, a peer that reaches the other machine's httpd.
#
# WHY THIS SHAPE.  Only the last three bytes survive the A2065's LANCE, so the
# hash fills the fifth and sixth and the fourth stays 0x49: the address is
# still recognisably one of ours in a capture, and there are 65024 of them
# rather than one.
#
# The fifth byte is never 0x00 and never 0x0d, which is what keeps a derived
# address clear of every harness that pins its own: tools/demo.sh 00:77 and the
# tests/tools runs on 00:xx are all fifth-byte 0x00, and run-dnsguard.sh is the
# only 0x0d.  So a derived address cannot collide with a pinned one, only with
# another derived one, and that needs a hash collision on the tag.
#
# SPDX-License-Identifier: MIT

# $1 = the run tag.  Prints one MAC address.
emu_mac_for_tag() {
    local tag="$1" hash fifth

    hash=$(printf '%s' "$tag" | cksum | cut -d' ' -f1)
    fifth=$(( (hash / 256) % 254 + 1 ))
    if [ "$fifth" -eq 13 ]; then
        fifth=255
    fi
    printf '02:41:4d:49:%02x:%02x\n' "$fifth" "$((hash % 256))"
}
