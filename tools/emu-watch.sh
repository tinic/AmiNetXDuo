#!/usr/bin/env bash
#
# PROGRESS FOR A GUEST THAT IS STILL RUNNING.
#
# Both emulator runners used to poll DH0:.done once a second and print nothing
# else until the timeout expired.  A guest that died twenty seconds in and a
# guest that was still working looked identical for up to twelve minutes, and
# the only output was `status 124` at the end -- which says the deadline
# passed and nothing about where the guest stopped.  Two 720 s waits were
# spent on that on 2026-09-11 before the boot was even attributed.
#
# DH0: is a directory on this host, so everything the guest writes is visible
# WHILE it runs.  That is already how install/test/run-workbench.sh reads the
# guest's address out of ShowNetStatus mid-boot.  This turns the same fact
# into the thing a stuck run needs: a running account of what the guest last
# did, and an early stop when it stops doing anything.
#
# A stall is not automatically a failure -- a guest waiting 30 s for a DHCP
# lease is quiet and healthy -- so the window has to be wider than the longest
# legitimate silence.  What it buys is that a DEAD guest costs the window
# rather than the timeout, and says what it last managed to do.
#
# SPDX-License-Identifier: MIT

# emu_watch_init <hd> <serial>
emu_watch_init() {
    EMU_WATCH_HD="$1"
    EMU_WATCH_SERIAL="$2"
    EMU_WATCH_FP=""
    EMU_WATCH_NOTE="nothing yet"
    EMU_WATCH_AT=0
    EMU_WATCH_QUIET=0
}

# The newest thing under DH0:, and how much serial has arrived.  Names the
# file rather than only counting, because "wrote usercheck.txt" localises a
# hang and "something changed" does not.
emu_watch_fingerprint() {
    local newest="" sersize=0
    [ -n "${EMU_WATCH_SERIAL:-}" ] && [ -f "$EMU_WATCH_SERIAL" ] &&
        sersize=$(wc -c < "$EMU_WATCH_SERIAL" 2>/dev/null || echo 0)
    if [ -n "${EMU_WATCH_HD:-}" ] && [ -d "$EMU_WATCH_HD" ]; then
        newest=$(find "$EMU_WATCH_HD" -type f -printf '%T@ %s %p\n' 2>/dev/null |
                 sort -n | tail -1)
    fi
    printf '%s|%s' "$sersize" "$newest"
}

# emu_watch_poll <elapsed>
# 0 when something moved since the last poll, 1 when nothing did.  Sets
# EMU_WATCH_NOTE to what moved and EMU_WATCH_QUIET to the seconds of silence.
emu_watch_poll() {
    local elapsed="$1" fp
    fp=$(emu_watch_fingerprint)

    if [ "$fp" != "$EMU_WATCH_FP" ]; then
        local sersize="${fp%%|*}" rest="${fp#*|}" path
        path=$(printf '%s' "$rest" | cut -d' ' -f3-)
        if [ -n "$path" ]; then
            EMU_WATCH_NOTE="wrote ${path#"$EMU_WATCH_HD"/}"
        else
            EMU_WATCH_NOTE="serial ${sersize} bytes"
        fi
        EMU_WATCH_FP="$fp"
        EMU_WATCH_AT="$elapsed"
        EMU_WATCH_QUIET=0
        return 0
    fi

    EMU_WATCH_QUIET=$((elapsed - EMU_WATCH_AT))
    return 1
}

# emu_watch_stalled <elapsed> <window>
# True once the guest has done nothing observable for <window> seconds.
emu_watch_stalled() {
    local elapsed="$1" window="$2"
    [ "$window" -gt 0 ] || return 1
    [ "$((elapsed - EMU_WATCH_AT))" -ge "$window" ]
}

# emu_watch_say_stall <elapsed>
emu_watch_say_stall() {
    local elapsed="$1"
    echo "!! the guest has done nothing for $((elapsed - EMU_WATCH_AT))s." >&2
    echo "!!   last activity at ${EMU_WATCH_AT}s: $EMU_WATCH_NOTE" >&2
    echo "!! Stopping here instead of waiting out the timeout.  Raise" >&2
    echo "!! AMINETXDUO_STALL_SECS if this guest is legitimately quiet" >&2
    echo "!! for longer than that." >&2
}
