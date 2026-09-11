#!/usr/bin/env bash
#
# No path in the shipping libraries may outgrow the stack it runs on.
#
#   tools/check-stack-frames.sh [build-dir]
#
# THE NUMBERS ARE FLOORS.  A call through a function pointer in a struct is not
# a symbol in the assembly; the resolver ladder is supplied by hand in
# LADDER_EDGES below, and anything else missing counts as zero.
#
# The build is its own: -fstack-usage and -save-temps=obj are not in the
# shipping configuration.  A directory that already has .su/.s files is reused
# ONLY while every source is older than it; anything newer and it is rebuilt,
# because data that predates the code describes code that is not there.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build/stackframes}"
JOBS="${AMINETXDUO_CI_JOBS:-$( (command -v nproc >/dev/null && nproc) || echo 4 )}"

# The resolver ladder calls its ask function through AmiNetAskFn, so the graph
# has a hole exactly where the deepest paths are.  These close it.
LADDER_EDGES=(
    --edge ami_net_ask_until=ami_ns_ask_name
    --edge ami_net_ask_until=ami_ns_ask_addr
    --edge ami_net_ask_until=ami_ns_ask_name6
    --edge ami_net_ask_until=ami_ns_ask_mdns
    --edge ami_ns_resolve_once=ami_net_ask_until
    --edge ami_ns_resolve6_once=ami_net_ask_until
    --edge netstack_resolve_reverse_until=ami_net_ask_until
    --edge netstack_resolve_until=ami_ns_resolve_once
)

# A thread entry point is dispatched, never called.  ami_sana2_rx_start() puts
# the reader's in a tx_thread_create() argument, which is a `lea` in the
# assembly and indistinguishable from a call, so every root that reaches
# ami_sana2_driver_entry -- every resolver root does, through the send a DNS
# query makes -- walks the whole receive path as though its own send had
# started the reader.  It measures ami_sana2_driver_entry at 728 bytes today,
# 540 of which is that.
#
# It changes no verdict while the reader is shallow: bsd_getaddrinfo's deepest
# path is 2016 bytes through mDNS and the entropy pool, which is further than
# the receive path reaches.  It stops being harmless the moment anything moves
# work on to the reader, and then it fails a budget for a call that does not
# exist.
# bsd_netstack_boot_main is a PROCESS, not a call.  bsd_netstack_bringup() and
# bsd_netstack_attach() name it in a CreateNewProc tag and then Wait(), so the
# assembly has an edge where the runtime has a 64 KB stack of its own
# (BSD_STARTUP_STACK).  Measuring through it charges the caller for work that
# never touches the caller's stack -- and hides the thing worth measuring,
# which is how much of a Shell's 4096 the vector itself takes before it hands
# the job over.
# tcp_ctrl_main and tcp_session_main are PROCESSES too, launched with their
# own NP_StackSize (TCP_CTRL_STACK, TCP_SESSION_STACK in
# src/bsdsocket/tcp_handler.c).  bsd_lib_open() starts the handler and returns;
# measuring through them charged the opener 908 bytes of tcp_session_open()
# that its stack never sees.  They want budgets of their own, against the
# stacks they are actually given -- not against a Shell's.
NOT_A_CALL=( --cut ami_sana2_rx_thread --cut bsd_netstack_boot_main
             --cut tcp_ctrl_main --cut tcp_session_main )

# The reader's own edges, and deliberately NOT in LADDER_EDGES.  NetX Duo
# dispatches the transport receivers and the link driver through function
# pointers in NX_IP, so without these the reader measures as a leaf that hashes
# an arrival time.
#
# They belong to that one root.  Three of them point at ami_sana2_driver_entry,
# which every resolver root reaches through a DNS query's send, so measuring
# any other root with them walks the whole receive path as if the send had
# started the reader -- 2792 bytes against a budget of 2432 for
# bsd_getaddrinfo.  Every other root gets NOT_A_CALL instead.
RX_EDGES=(
    --edge ami_sana2_rx_thread=ami_sana2_rx_deliver
    --edge ami_sana2_rx_deliver=ami_sana2_rx_dispatch
    --edge ami_sana2_rx_dispatch=_nx_ip_packet_receive
    --edge ami_sana2_rx_dispatch=_nx_arp_packet_receive
    --edge _nx_ip_packet_receive=_nx_tcp_packet_receive
    --edge _nx_ip_packet_receive=_nx_udp_packet_receive
    --edge _nx_ip_packet_receive=_nx_icmp_packet_receive
    --edge _nx_ip_packet_receive=_nx_igmp_packet_receive
    --edge _nx_ip_packet_send=ami_sana2_driver_entry
    --edge _nx_ipv6_packet_send=ami_sana2_driver_entry
    --edge _nx_arp_packet_send=ami_sana2_driver_entry
)

# WHAT IS CHECKED, and what each number is for.
#
#   binary  root symbol                     budget  measured  what it bounds
#
# bsdsocket.library runs on a Shell's 4096 with the calling program's own
# frames already in it.  stack_test measures 2840 touched for the same calls,
# so the static figure runs about 800 bytes light; a budget of 2432 against
# 2016 measured leaves the guest number under 3700 with the eighth of a Shell
# stack stack_test insists on still spare.
#
# The absorb has its own row because it is the one that regressed unobserved,
# and because it is bounded by construction now: its buffers are in an
# AllocMem block, so anything that puts one back on the stack shows up here as
# hundreds of bytes and not as tens.
#
# tls.library is not on a Shell stack -- every program that opens it must
# StackSwap first, and src/tools/fetch.c asks for 64 KB -- so its budget is
# about noticing growth, not about survival.
#
# Neither is the SANA-II reader: AMI_SANA2_RX_STACK_SIZE is 8192.  Its static
# budget remains 3072 so growth in code we own cannot consume the margin held
# for a third-party device driver's BeginIO() frames.  They run on whichever
# stack calls them and none is a symbol on this path.  Runtime probes cover
# three drivers, but cannot establish a smaller safe floor for every external
# driver.
# THE VECTORS A SHELL COMMAND ENTERS THROUGH had no row at all until 0.27, and
# that is how NETCTRL_INTERFACE_ADD came to want 3484 bytes of a Shell's 4096:
# every root here was a resolver, a reader or a TLS path, so the gate said
# clean while AddNetInterface corrupted the stack under it about half the time.
# A budget that does not cover the way in is not a budget for the way in.
#
# bsd_lib_open carries the bring-up fallback, which runs on the caller's stack
# when no signal or process can be had -- 2212 measured.  bsd_NetStackControl
# is 1868 now that the attach runs on the library's own stack; the margin is
# the same eighth of a Shell stack the resolver rows keep.
BUDGETS=(
    "bsdsocket:bsd_lib_open:2432"
    "bsdsocket:bsd_NetStackControl:2432"
    "bsdsocket:ami_sana2_rx_thread:3072"
    "bsdsocket:bsd_getaddrinfo:2432"
    "bsdsocket:bsd_getnameinfo:2432"
    "bsdsocket:bsd_gethostbyname:2432"
    "bsdsocket:bsd_gethostbyaddr:2432"
    "bsdsocket:ami_ns_dns_absorb_pending:768"
    "tlslib:ami_crypto_method_rsa_operation:3456"
    "tlslib:_nx_crypto_huge_number_modulus:2688"
)

say() { echo "$@"; }

# ------------------------------------------------------------------ build ---

# Both halves, or the graph has frame sizes and no edges and every root looks
# like a leaf -- which would pass every budget while measuring nothing.
#
# AND THE DATA HAS TO BE NEWER THAN THE CODE IT DESCRIBES.  Reusing whatever
# .su files happened to be in the directory made this gate measure a tree that
# was no longer there: on 2026-09-11 a release run failed bsd_NetStackControl
# at 2868 bytes against files built the previous evening, hours after the path
# had been moved onto its own stack and measured at 1852 in a fresh directory.
# A stale PASS is worse than that stale FAIL and reads identically.
need_build=1
if [ -d "$BUILD" ] &&
   [ -n "$(find "$BUILD" -name '*.su' -print -quit 2>/dev/null)" ] &&
   [ -n "$(find "$BUILD" -name '*.ltrans*.s' -print -quit 2>/dev/null)" ]; then
    need_build=0

    _stale=$(find "$ROOT/src" "$ROOT/include" "$ROOT/port" \
                  -name '*.c' -o -name '*.h' 2>/dev/null |
             while read -r _src; do
                 [ "$_src" -nt "$BUILD" ] && { echo "$_src"; break; }
             done)
    if [ -n "$_stale" ]; then
        say "stack_frames=rebuilding reason=source_newer_than_data" \
            "first=${_stale#"$ROOT"/}"
        rm -rf "${BUILD:?}"
        need_build=1
    fi
fi

if [ "$need_build" = 1 ]; then
    # shellcheck disable=SC1091
    . "$ROOT/tools/amiga-toolchain.sh" >/dev/null 2>&1 || true
    if [ -z "${AMIGA_TOOLCHAIN_ROOT:-}" ] ||
       [ ! -x "${AMIGA_TOOLCHAIN_ROOT}/bin/m68k-amigaos-gcc" ]; then
        say "stack_frames=skipped reason=no_toolchain"
        exit 2
    fi

    if ! cmake -S "$ROOT" -B "$BUILD" \
            -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_FLAGS="-fstack-usage -save-temps=obj" \
            > "$BUILD-configure.log" 2>&1; then
        say "stack_frames=skipped reason=configure_failed log=$BUILD-configure.log"
        exit 2
    fi

    if ! cmake --build "$BUILD" --parallel "$JOBS" \
            --target bsdsocket_library tls_library \
            > "$BUILD-build.log" 2>&1; then
        say "stack_frames=skipped reason=build_failed log=$BUILD-build.log"
        exit 2
    fi
fi

DEPTH="$ROOT/tools/stack-depth.py"
PYTHON="$(command -v python3 || command -v python || true)"
if [ -z "$PYTHON" ] || [ ! -f "$DEPTH" ]; then
    say "stack_frames=skipped reason=no_python"
    exit 2
fi

# ---------------------------------------------------------------- measure ---

rc=0
checked=0
for row in "${BUDGETS[@]}"; do
    IFS=: read -r lib sym budget <<< "$row"

    dir="$BUILD/src/$lib"
    if [ ! -d "$dir" ]; then
        say "stack_frames=skipped reason=no_objects binary=$lib"
        exit 2
    fi

    case "$sym" in
        ami_sana2_rx_thread) EXTRA=("${RX_EDGES[@]}") ;;
        *)                   EXTRA=("${NOT_A_CALL[@]}") ;;
    esac

    out=$("$PYTHON" "$DEPTH" --quiet "${LADDER_EDGES[@]}" "${EXTRA[@]}" \
          "$dir" "$sym" 2>/dev/null)
    got=${out##*worst_case_bytes=}
    got=${got%%[!0-9]*}

    if [ -z "$got" ] || [ "$got" = 0 ]; then
        # A root that resolves to nothing is a broken check, not a pass: it is
        # how a renamed symbol would quietly stop being measured.
        say "stack_frames=FAILED binary=$lib root=$sym reason=symbol_not_found"
        rc=1
        continue
    fi

    checked=$((checked + 1))
    if [ "$got" -gt "$budget" ]; then
        say "stack_frames=FAILED binary=$lib root=$sym bytes=$got budget=$budget"
        say "  the deepest path:"
        "$PYTHON" "$DEPTH" "${LADDER_EDGES[@]}" "${EXTRA[@]}" \
            "$dir" "$sym" 2>/dev/null |
            sed -n '2,$p' | sed 's/^/  /'
        rc=1
    else
        say "stack_frames_bytes binary=$lib root=$sym bytes=$got budget=$budget"
    fi
done

if [ "$rc" = 0 ]; then
    say "stack_frames=clean roots=$checked"
fi
exit "$rc"
