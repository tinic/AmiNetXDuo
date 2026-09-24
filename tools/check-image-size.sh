#!/usr/bin/env bash
#
# The resident images may not grow without somebody typing the new number.
#
#   tools/check-image-size.sh <build-dir>
#   AMINETXDUO_IMAGE_ARM=default tools/check-image-size.sh <build-dir>
#
# bsdsocket.library and anxnet.device are open for the life of the machine, so
# their size is RAM and not disk.  0.26.0 shipped 27,948 bytes of serial-log
# sentences inside bsdsocket.library -- 7.2 per cent of it -- and every gate in
# this directory passed, because none of them asked how big anything got.  The
# argument for those bytes had been made and measured; what nobody did was
# decide to spend them.
#
# ONLY THE THREE CONFIGURATIONS THAT SHIP ARE BUDGETED, and they are the three
# tools/check-shipping-config.sh maps a drawer onto: `default` is the full
# drawer, with `minimal` and `micro` named directly.  Every other arm is
# coverage -- nolto
# is 44 KB larger because that is what LTO is worth, census carries its side
# table, log carries the sentences -- and holding coverage to a shipping budget
# would only teach whoever hits it to raise the number.  The arm is the build
# directory's name, or AMINETXDUO_IMAGE_ARM.
#
# A budget is not a limit somebody guessed at.  It is the size the image was
# when it was last looked at, plus room to move, so growth arrives as a diff to
# this file with a reason beside it rather than as a fact found after a
# release.  Raising one is fine.  Raising one without saying why in the commit
# message is the thing this exists to stop.
#
# BSS is measured but not budgeted: an AmigaOS hunk records it as a length and
# no bytes, so it costs the image nothing and the machine the whole thing.  It
# is reported so a jump in it is visible at all.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build/ci/default}"
ARM="${AMINETXDUO_IMAGE_ARM:-$(basename "$BUILD")}"

# arm : image path under the build dir : budget in bytes
#
# Set 2026-08-28 at 0.26.1, headroom to the next round 2 KB.  bsdsocket.library
# is 361,428 in the full drawer and 233,928 in the minimal one, both with
# AMINETXDUO_LOG off, which is where they belong.
BUDGETS=(
    # 355,508 with ANXD_CMD_RX_BATCH (the batch reader: post, drain, the
    # settle/hand-up split) -- A1200 iperf RX 399 -> 805 Mbit/s, 2026-09-20.
    # 357,160 with SBTC_ERROR_HOOK called from the outer
    # bsd_nx_leave() and the hook re-read off the base before each
    # deferred call (b1706f18, f5576e57), 2026-09-22.
    "default:src/bsdsocket/bsdsocket.library:359000"
    # 41,412 after stateless receive-checksum verification was added to the
    # EL3 and word/long NE2000 direct paths, 2026-09-15.  43,620 with
    # ANXD_CMD_RX_BATCH in the shell (claim, completion, staging copy, the
    # pass-end flush, the command), which the classic cores refuse and so
    # never pay for at run time, 2026-09-20.
    # 44,860 after the 68030 cache guards were scoped to active units
    # (a3292227): the per-unit push/invalidate record, its lookup and the
    # device-side arm/disarm.  A guard that flushed for every unit on a
    # machine with one card paid for cards that were not there, 2026-09-22.
    "default:src/netdev/anxnet.device:46000"
    # 21,724 at the split; 23,032 with the original receive offload (IPv4 and
    # IPv6 verification plus the former driver-side CONTINUES mark); 24,440 in
    # the former batched-reply experiment, plus the held pass and reset
    # counters -- 142 -> 270 Mbit/s between them, 2026-09-15.  26,828 with
    # the idle poller (its task, the clock through /soc, three counters):
    # Fitz read 26.5 -> 31.6 MB/s, iperf in 832 -> 910, 2026-09-17.  27,428
    # with the batch shell, the pass budget and the RBUF overflow counter
    # after the batched reply and the driver-side GRO left: 399 -> 805
    # Mbit/s against main 027a62c8, 2026-09-20.
    # 28,740 after the GIC top half was reduced to acknowledge/mask/signal and
    # the ring, callbacks, PHY and watchdog moved into a schedulable service
    # task.  The extra ownership and lifecycle serialization buy bounded
    # interrupt latency rather than another data-path feature, 2026-09-20.
    "default:src/netdev/anxgenet.device:29000"
    "default:src/wifipi/anxwifipi.device:56000"
    # +832 bytes for Roadshow's native users/groups ReadArgs syntax, strict
    # /N validation and bounded member-vector sizing: existing UID/GID maps
    # survive installing this usergroup.library without trusting malformed DBs.
    "default:src/usergroup/usergroup.library:10000"
    "default:src/tlslib/tls.library:198000"
    # 225,876 -> 226,216 on 2026-09-17: the write queue behind the SANA-II
    # ring and its launcher (a round trip in flight for every card: A1200
    # sends 180 -> 275 Mbit/s on the LAN, 12 -> 198 over 26 ms), the 1 MB
    # long-path window, and DEVS: redirected only to what exists.
    # -> 226,788 the same day: the transport checksum written by the card
    # (negotiated TX checksum: the record, pseudo-header sum at launch and
    # per-write metadata callback), A1200 sends 337 -> 462 Mbit/s with the page push it
    # made possible.  -> 227,148: RFC 6675 loss recovery in the fork (the
    # lost accounting, HighRxt, the walk on every duplicate): one drop in
    # ten thousand 63 -> 537 Mbit/s out on the A1200.  -> 227,512: the DNS
    # cache dropped with the server that filled it (nxd_dns.c
    # _nx_dns_cache_drop), so a name does not resolve after NetShutdown.
    # -> 227,696: the SACK tail fix retransmits the second lost tail segment
    # when the first repair is ACKed, instead of waiting for the RTO with no
    # further duplicate ACK possible.
    # -> 227,672: FILTER=EVERYTHING read (three values compared, the
    # promiscuous open flag carried to OpenDevice).  -> 227,804:
    # gethostname() qualifies a dotless name with the domain in force.
    # -> 227,572: the MTU-based datagram caps and bsd_route_mtu() gone, the
    # stack fragments what BSD fragments.  -> 227,916: the versioned transmit
    # metadata callback keeps private state out of SANA-II's io_Flags.
    # 229,560 with the batch reader, 2026-09-20 (see the default row).
    # 231,324 with the SBTC_ERROR_HOOK move, 2026-09-22.
    # 233,024 with the retained SANA-II interface (a device that keeps
    # requests after close is held, not dropped), 2026-09-24.
    "minimal:src/bsdsocket/bsdsocket.library:234000"
    "minimal:src/netdev/anxnet.device:46000"
    "minimal:src/netdev/anxgenet.device:29000"
    "minimal:src/wifipi/anxwifipi.device:56000"
    "minimal:src/usergroup/usergroup.library:10000"
    # First budgeted as a shipping profile at 0.28.9: 181,012 bytes.  Raised
    # to 197,632 in beta2: the private status/control implementation is 16 KB
    # and cannot be removed because AddNetInterface, Online and Offline use it.
    # Headroom remains only to the next KiB boundary.
    # +184 bytes in beta5 for hiding and restoring the bare AMITCP port around
    # vendor X-Surf OpenDevice; without it the driver selects its
    # incompatible private AmiTCP copy path and sends no packets.
    # 198,048 measured with the X-Surf-only name check, which leaves AMITCP
    # visible to every other SANA-II driver (2026-09-23).
    # 199,224 with the retained SANA-II interface, 2026-09-24.
    "micro:src/bsdsocket/bsdsocket.library:200000"
    "micro:src/netdev/anxnet.device:46000"
    "micro:src/netdev/anxgenet.device:29000"
    "micro:src/wifipi/anxwifipi.device:56000"
    "micro:src/usergroup/usergroup.library:10000"
)

budgeted=0
for row in "${BUDGETS[@]}"; do
    [ "${row%%:*}" = "$ARM" ] && budgeted=1
done

if [ "$budgeted" = 0 ]; then
    echo "image_size=skipped arm=$ARM reason=not_a_shipping_configuration"
    exit 0
fi

if [ ! -d "$BUILD" ]; then
    echo "image_size=skipped reason=no_build dir=$BUILD"
    exit 0
fi

SIZE="${AMIGA_TOOLCHAIN_ROOT:-}/bin/m68k-amigaos-size"
rc=0
seen=0

for row in "${BUDGETS[@]}"; do
    arm="${row%%:*}"
    [ "$arm" = "$ARM" ] || continue
    rest="${row#*:}"
    rel="${rest%:*}"
    budget="${rest##*:}"
    img="$BUILD/$rel"
    name="$(basename "$rel")"

    [ -f "$img" ] || continue
    seen=$((seen + 1))

    bytes=$(stat -c %s "$img" 2>/dev/null || stat -f %z "$img")
    bss=""
    [ -x "$SIZE" ] && bss=$("$SIZE" "$img" 2>/dev/null | tail -1 | awk '{print $3}')

    if [ "$bytes" -gt "$budget" ]; then
        echo "image_size=OVER arm=$arm image=$name bytes=$bytes budget=$budget over=$((bytes - budget))"
        echo "  $name is resident for the life of the machine.  Either take the"
        echo "  bytes back out, or raise its budget in tools/check-image-size.sh"
        echo "  in the same commit and say in the message what bought them."
        rc=1
    else
        echo "image_size=ok arm=$arm image=$name bytes=$bytes budget=$budget spare=$((budget - bytes))${bss:+ bss=$bss}"
    fi
done

if [ "$seen" = 0 ]; then
    echo "image_size=skipped arm=$ARM reason=no_images dir=$BUILD"
    exit 0
fi

[ "$rc" = 0 ] && echo "image_size=PASS arm=$ARM images=$seen"
exit "$rc"
