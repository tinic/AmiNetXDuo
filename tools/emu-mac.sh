#!/usr/bin/env bash
#
# One MAC address per run tag, for every harness that puts a guest on the
# bridge.
#
#   . tools/emu-mac.sh
#   MAC=$(emu_mac_for_tag "$TAG")
#
# The fifth byte is never 0x00 and never 0x0d, which is what keeps a derived
# address clear of every harness that pins its own: tools/demo.sh 00:77 and the
# tests/tools runs on 00:xx are fifth-byte 0x00, run-dnsguard.sh the only 0x0d.
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
