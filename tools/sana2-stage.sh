#!/usr/bin/env bash
#
# Board -> SANA-II driver, and staging that driver into a test's DEVS: tree.
# Sourced by tests/netstack/run-winuae.sh and tests/conformance/run-winuae.sh
# so the two agree about which driver a board wants and where it lives.
#
#   sana2_driver_for <board>        echoes the driver file name
#   sana2_stage <board> <devsdir>   stages the driver and rewrites DEVICE=
#
# The driver name is not the board key.  Individual Computers ship
# x-surf-100.device, Hydra Systems ship hydra.device, ASDG ship eb920.device.
#
# Third-party drivers are staged into DEVS:Networks, which is where they are
# really installed, and DEVICE= is left as the bare name so the run exercises
# the fallback in ami_sana2_open_device(), docs/RESEARCH.md 44.9.  The A2065
# keeps DEVS: itself, which is where our own tests have always put it.
#
# Environment:
#   AMINETXDUO_SANA2_DRIVER       path to the driver binary to stage
#   AMINETXDUO_SANA2_DRIVER_NAME  the name it is staged under
#   AMINETXDUO_SANA2_DEVICE       what goes in DEVICE=, for proving whether a
#                                 bare name reaches DEVS:Networks
#   AMINETXDUO_SANA2_DIR          subdirectory of DEVS: to stage into
#   AMINETXDUO_SANA2_CARD         CARD= to write into the interface file, for
#                                 a driver that covers a family of boards
#   AMINETXDUO_SANA2_VENDOR       set to anything: sana2_select() picks the
#                                 vendor driver for every card
#
# SPDX-License-Identifier: MIT

sana2_driver_for() {
    case "$1" in
        a2065)                 echo a2065.device ;;
        ariadne)               echo ariadne.device ;;
        ariadne2)              echo ariadne_ii.device ;;
        hydra)                 echo hydra.device ;;
        eb920)                 echo eb920.device ;;
        xsurf)                 echo x-surf.device ;;
        xsurf100z2|xsurf100z3) echo x-surf-100.device ;;
        ne2000_pcmcia)         echo cnet.device ;;
        *)                     echo "$1.device" ;;
    esac
}

# ------------------------------------------------- our driver, per card ----
#
# THE ONE PLACE.  anxnet.device covers the NE2000/DP8390 family and nothing
# else, so a sweep gates on it for the boards it supports and on the vendor
# driver for the rest.  This table is what decides which is which, and adding
# a row is the whole change:
#
#   hydra   ->  hydra      when NETDEV_CHIP_ED has a core.  src/netdev/
#   eb920   ->  lanrover   netdev_cards.c has both rows already and
#                          netdev_nic_ops_for() returns NULL for the chip, so
#                          the open fails today; that is why they are not here.
#
# Never covered, and not waiting on anything:
#   a2065, ariadne          Am7990 LANCE, a different chip family
#   ne2000_pcmcia           fixed at 0xA20300 with no autoconfig record, so
#                           there is nothing for the ConfigDev walk to find
#
# The value is the CARD= name, which is netdev_cards.c's row name and is also
# what include/aminetxduo/anxnet.h lists for the config parser.  Echoes
# nothing when anxnet.device does not cover the board.
anxnet_card_for() {
    case "$1" in
        xsurf100z2|xsurf100z3) echo xsurf100 ;;
        ariadne2)              echo ariadne2 ;;
        xsurf)                 echo xsurf ;;
        hydra)                 echo hydra ;;
        eb920)                 echo lanrover ;;
        a2065)                 echo a2065 ;;
        ariadne)               echo ariadne ;;
        ne2000_pcmcia)         echo pcmcia ;;
        *)                     echo "" ;;
    esac
}

# Where the built anxnet.device is, given a build directory.  Echoes the path
# or nothing.
anxnet_binary() { # [builddir]
    if [ -n "${AMINETXDUO_ANXNET:-}" ] && [ -f "$AMINETXDUO_ANXNET" ]; then
        echo "$AMINETXDUO_ANXNET"
        return 0
    fi
    if [ -n "${1:-}" ] && [ -f "$1/src/netdev/anxnet.device" ]; then
        echo "$1/src/netdev/anxnet.device"
        return 0
    fi
    [ -f "$HOME/amiga-assets/devs/anxnet.device" ] &&
        echo "$HOME/amiga-assets/devs/anxnet.device"
    return 0
}

# Which driver a sweep should gate this board on.  Sets, always:
#
#   SANA2_SEL_DRIVER   the driver file name
#   SANA2_SEL_PATH     where its binary is, empty when it is not on this host
#   SANA2_SEL_CARD     the CARD= name, empty for a vendor driver
#   SANA2_SEL_SOURCE   anxnet | vendor
#
# A board anxnet.device covers whose binary is missing does NOT fall back to
# the vendor driver: the sweep would then report a green card for a driver it
# was not asked to measure.  It comes back with SANA2_SEL_PATH empty and
# SANA2_SEL_SOURCE=anxnet, which every caller already reports as a skip.
# shellcheck disable=SC2034  # SANA2_SEL_* are read by the callers, not here
sana2_select() { # board [builddir]
    SANA2_SEL_CARD=""
    SANA2_SEL_SOURCE=vendor
    SANA2_SEL_DRIVER=$(sana2_driver_for "$1")

    if [ -z "${AMINETXDUO_SANA2_VENDOR:-}" ]; then
        SANA2_SEL_CARD=$(anxnet_card_for "$1")
        if [ -n "$SANA2_SEL_CARD" ]; then
            SANA2_SEL_SOURCE=anxnet
            SANA2_SEL_DRIVER=anxnet.device
            SANA2_SEL_PATH=$(anxnet_binary "${2:-}")
            return 0
        fi
    fi

    SANA2_SEL_PATH=$(sana2_local_driver "$SANA2_SEL_DRIVER")
}

# Where a hand-placed driver lives.  Most of these cannot be fetched, the
# licences in docs/RESEARCH.md 77 permit two of eight, so the rest arrive by
# somebody putting the file there, and that store is the supported way to do
# it.  Echoes the path, or nothing.
sana2_local_driver() {
    for _d in ${AMINETXDUO_SANA2_STORE:-} "$HOME/amiga-assets/devs"; do
        [ -f "$_d/$1" ] && { echo "$_d/$1"; return 0; }
    done
    return 0
}

# The three variables every caller reads afterwards, worked out from the board
# and the environment.  Set by both halves below so either can be used alone.
#
#   _drv_name  the file the driver is staged as
#   _dir       the subdirectory of DEVS: it goes in, empty for DEVS: itself
#   _device    what goes in DEVICE=
# shellcheck disable=SC2034  # SANA2_* are read by the callers, not here
_sana2_names() {
    _drv_name=${AMINETXDUO_SANA2_DRIVER_NAME:-$(sana2_driver_for "$1")}
    _drv_path=${AMINETXDUO_SANA2_DRIVER:-}
    if [ "$1" = a2065 ]; then
        _dir=${AMINETXDUO_SANA2_DIR-}
    else
        _dir=${AMINETXDUO_SANA2_DIR-Networks}
    fi
    _device=${AMINETXDUO_SANA2_DEVICE:-$_drv_name}

    SANA2_DRIVER=$_drv_name
    SANA2_DEVICE=$_device
    SANA2_DIR=$_dir
    SANA2_CARD=${AMINETXDUO_SANA2_CARD:-}
}

# THE DRIVER FILE ONLY, into $2, which is a DEVS: tree.  Split out of
# sana2_stage because install/test/run-workbench.sh needs exactly this half and
# cannot have the other: the interface file it would rewrite does not exist
# yet, the Installer is what writes it, and creating one first would put the
# install down the "keep the existing configuration" branch instead of the card
# question.
#
# Echoes a warning and returns 0 when there is no driver to stage: the card is
# then in the machine with nothing able to open it, which is a result.
sana2_stage_driver() {
    _sana2_names "$1"
    _devs=$2

    if [ -n "$_drv_path" ] && [ -f "$_drv_path" ]; then
        if [ -n "$_dir" ]; then
            mkdir -p "$_devs/$_dir"
            cp "$_drv_path" "$_devs/$_dir/$_drv_name"
        else
            cp "$_drv_path" "$_devs/$_drv_name"
        fi
    elif [ "$1" != a2065 ]; then
        echo "!! no $_drv_name staged: set AMINETXDUO_SANA2_DRIVER=<path> to one." >&2
        echo "!! The card will be in the machine and nothing will be able to" >&2
        echo "!! open it, which is what this run will show." >&2
    fi

    # Said out loud, once, by the one function every caller goes through.
    # Which driver a run booted decides what the run proves, and a caller that
    # leaves it to the reader ships a green result for a driver nobody asked
    # about.  source= is read off the file name rather than off a variable so
    # it cannot disagree with what was actually copied.
    case "$_drv_name" in
        anxnet.device) _src=anxnet ;;
        *)             _src=vendor ;;
    esac
    printf 'sana2_staged board=%s driver=%s source=%s device=%s card=%s dir=%s path=%s\n' \
           "$1" "$_drv_name" "$_src" "$_device" \
           "${SANA2_CARD:-none}" "DEVS:${_dir:+$_dir/}" \
           "${_drv_path:-none}"
}

# THE INTERFACE FILE ONLY: point $2/NetInterfaces/eth0 at the driver for $1.
sana2_stage_interface() {
    _sana2_names "$1"
    _devs=$2

    sed "s|^DEVICE=.*|DEVICE=$_device|" "$_devs/NetInterfaces/eth0" \
        > "$_devs/NetInterfaces/eth0.new"
    mv "$_devs/NetInterfaces/eth0.new" "$_devs/NetInterfaces/eth0"

    # CARD= when a driver covers a family of boards.  Rewritten rather than
    # appended so a stage run twice does not leave two lines, and dropped
    # entirely when nothing asked for one: a vendor driver would report it as
    # an unknown keyword.
    grep -v '^CARD=' "$_devs/NetInterfaces/eth0" > "$_devs/NetInterfaces/eth0.new"
    [ -z "${AMINETXDUO_SANA2_CARD:-}" ] ||
        echo "CARD=$AMINETXDUO_SANA2_CARD" >> "$_devs/NetInterfaces/eth0.new"
    mv "$_devs/NetInterfaces/eth0.new" "$_devs/NetInterfaces/eth0"
}

# Stage the driver for $1 into $2 and point DEVS:NetInterfaces/eth0 at it.
sana2_stage() {
    sana2_stage_interface "$1" "$2"
    sana2_stage_driver "$1" "$2"
}
