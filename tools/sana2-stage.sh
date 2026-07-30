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
# the fallback in ami_sana2_open_device() -- docs/RESEARCH.md 44.9.  The A2065
# keeps DEVS: itself, which is where our own tests have always put it.
#
# Environment:
#   AMINETXDUO_SANA2_DRIVER       path to the driver binary to stage
#   AMINETXDUO_SANA2_DRIVER_NAME  the name it is staged under
#   AMINETXDUO_SANA2_DEVICE       what goes in DEVICE=, for proving whether a
#                                 bare name reaches DEVS:Networks
#   AMINETXDUO_SANA2_DIR          subdirectory of DEVS: to stage into
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

# Where a hand-placed driver lives.  Most of these cannot be fetched -- the
# licences in docs/RESEARCH.md 77 permit two of eight -- so the rest arrive by
# somebody putting the file there, and that store is the supported way to do
# it.  Echoes the path, or nothing.
sana2_local_driver() {
    for _d in ${AMINETXDUO_SANA2_STORE:-} "$HOME/amiga-assets/devs"; do
        [ -f "$_d/$1" ] && { echo "$_d/$1"; return 0; }
    done
    return 0
}

# Stage the driver for $1 into $2 and point DEVS:NetInterfaces/eth0 at it.
# Echoes a warning and returns 0 when there is no driver to stage: the card is
# then in the machine with nothing able to open it, which is a result.
sana2_stage() {
    _board=$1
    _devs=$2
    _drv_name=${AMINETXDUO_SANA2_DRIVER_NAME:-$(sana2_driver_for "$_board")}
    _drv_path=${AMINETXDUO_SANA2_DRIVER:-}

    if [ "$_board" = a2065 ]; then
        _dir=${AMINETXDUO_SANA2_DIR-}
    else
        _dir=${AMINETXDUO_SANA2_DIR-Networks}
    fi
    _device=${AMINETXDUO_SANA2_DEVICE:-$_drv_name}

    sed "s|^DEVICE=.*|DEVICE=$_device|" "$_devs/NetInterfaces/eth0" \
        > "$_devs/NetInterfaces/eth0.new"
    mv "$_devs/NetInterfaces/eth0.new" "$_devs/NetInterfaces/eth0"

    if [ -n "$_drv_path" ] && [ -f "$_drv_path" ]; then
        if [ -n "$_dir" ]; then
            mkdir -p "$_devs/$_dir"
            cp "$_drv_path" "$_devs/$_dir/$_drv_name"
        else
            cp "$_drv_path" "$_devs/$_drv_name"
        fi
    elif [ "$_board" != a2065 ]; then
        echo "!! no $_drv_name staged: set AMINETXDUO_SANA2_DRIVER=<path> to one." >&2
        echo "!! The card will be in the machine and nothing will be able to" >&2
        echo "!! open it, which is what this run will show." >&2
    fi

    SANA2_DRIVER=$_drv_name
    SANA2_DEVICE=$_device
}
