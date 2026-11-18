# shellcheck shell=bash
#
# The updater scenarios, the body of install/test/run-workbench.sh -Y.
#
# SOURCED, not run: it uses that script's boot(), startup_with(),
# build_driver(), product_manifest(), foreign_intact() and amiga_path(), and
# the drive, Workbench, card and archive it has staged by the time it takes
# the emulator lock.  Those are defined among top-level code that runs, so no
# other script can source them.  tests/updater/run-scenarios.sh is the usual
# caller: it builds the releases, starts the mirror and sets
#
#   AMINETXDUO_UPDATE_RELEASES  tests/updater/make-releases.sh's output
#   AMINETXDUO_UPDATE_MIRROR    http://<peer address>:<port>
#   AMINETXDUO_UPDATE_PEER      user@host the mirror runs on
#   AMINETXDUO_UPDATE_PEER_LOG  its request log there
#   AMINETXDUO_UPDATE_RUN       a token naming this run in that log
#
# Per row of tests/updater/scenarios.tsv with this Workbench's os: the old
# release installed by Commodore's Installer for the row's layout and flavour
# (once per pair, then copied), the row's setup applied and checked, its
# boots, and its checks.  Output:
#
#   base=<layout-flavour> result=ok|fail install_status=<n> wall=<s>
#   setup=<item> scenario=<id> ok=yes|no
#   check=<name> scenario=<id> ok=yes|no ...
#   scenario=<id> result=PASS|FAIL|XFAIL|XPASS reason=<key> wall=<s> boots=<n>
#   scenarios=<n> pass=<n> fail=<n> xfail=<n> xpass=<n>
#
# UPD_EXIT is 0 only with no FAIL and no XPASS, 2 when an ingredient is
# missing.
#
# SPDX-License-Identifier: MIT

UPD_TSV="$ROOT/tests/updater/scenarios.tsv"
UPD_REL="${AMINETXDUO_UPDATE_RELEASES:-$ROOT/build/updscn/releases}"
UPD_MIRROR="${AMINETXDUO_UPDATE_MIRROR:-}"
UPD_PEER="${AMINETXDUO_UPDATE_PEER:-}"
UPD_PEER_LOG="${AMINETXDUO_UPDATE_PEER_LOG:-}"
UPD_RUN="${AMINETXDUO_UPDATE_RUN:-r$(date +%s)}"
UPD_STORE="$ROOT/build/updscn/$TAG"
UPD_OUT="$UPD_STORE/results"
UPD_HDF="$UPD_STORE/upd.hdf"
UPD_CUT_MAX="${AMINETXDUO_UPD_CUT_MAX:-400}"
UPD_EXIT=0

upd_refuse() { echo "updater_scenarios=refused reason=$1"; UPD_EXIT=2; }

if [ ! -f "$UPD_REL/releases.env" ]; then
    upd_refuse "no_releases:$UPD_REL"
    return 0
fi
# shellcheck source=/dev/null
. "$UPD_REL/releases.env"
if [ -z "$UPD_MIRROR" ] || [ -z "$UPD_PEER" ] || [ -z "$UPD_PEER_LOG" ]; then
    upd_refuse "no_mirror:set_AMINETXDUO_UPDATE_MIRROR_PEER_PEER_LOG"
    return 0
fi
[ -n "$XDFTOOL" ] && [ -x "$XDFTOOL" ] || { upd_refuse no_xdftool; return 0; }

mkdir -p "$UPD_STORE" "$UPD_OUT"
UPD_NEW_MANIFEST="$UPD_REL/new/update/update.manifest"
UPD_OLD_MANIFEST="$UPD_REL/old/update/update.manifest"

sha256_file() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

# ------------------------------------------------------------ snapshots --

upd_save() { # dir
    rm -rf "$1"
    mkdir -p "$1"
    cp -a "$HD" "$1/HD"
    cp -a "$WORK" "$1/WORK"
    [ ! -f "$UPD_HDF" ] || cp -a "$UPD_HDF" "$1/upd.hdf"
}

upd_restore() { # dir
    rm -rf "$HD" "$WORK"
    cp -a "$1/HD" "$HD"
    cp -a "$1/WORK" "$WORK"
    rm -f "$UPD_HDF"
    [ ! -f "$1/upd.hdf" ] || cp -a "$1/upd.hdf" "$UPD_HDF"
}

upd_save "$UPD_STORE/pristine"

# Every boot ends by the emulator being killed after DH0:.done.  DH0: is a
# host directory and loses nothing, but the Upd volume is FFS: a kill inside
# its flush delay leaves it unvalidated and the next boot stops on it.  So a
# boot that may have written there waits for the flush first.
UPD_SETTLE="Wait 5"

# ------------------------------------------------------- the Upd volume --
#
# drawer@Upd: the drawer layout on a volume of its own, an FFS hardfile, so
# that it can be write-protected or filled.  A directory volume reports the
# host's free space and cannot be either.  Not bootable (-128).
upd_hdf_cfg() { # rw|ro
    echo "hardfile2=$1,UPD:$UPD_HDF,32,1,2,512,-128,,uae0"
}

upd_tree() { # dir: the Upd volume unpacked there
    rm -rf "$1"
    mkdir -p "$1"
    "$XDFTOOL" "$UPD_HDF" unpack "$1" >/dev/null 2>&1 || return 1
    [ -d "$1/Upd" ] || return 1
}

# --------------------------------------------------------- base installs --

UPD_BASE_KEY=""
upd_base() { # layout flavour
    local layout="$1" flavour="$2" key dir lvl="NOVICE" spec="" t0 st
    local answers=() lib want got
    key="${layout/@/-at-}-$flavour"
    UPD_BASE_KEY="$key"
    dir="$UPD_STORE/base-$key"
    [ -f "$dir/ok" ] && return 0
    [ -f "$dir/failed" ] && return 1
    t0=$(date +%s)
    upd_restore "$UPD_STORE/pristine"
    case "$flavour" in
        full) ;;
        minimal) spec="4:3:0" ;;
        micro)   spec="4:4:0" ;;
        fat)     spec="4:5:0" ;;
        *) echo "base=$key result=fail reason=flavour"; return 1 ;;
    esac
    [ "$flavour" = full ] || lvl=AVERAGE
    BOOT_EXTRA_CFG=""
    case "$layout" in
        system) ;;
        drawer) : > "$HD/S/AmiNetXDuo-drawer" ;;
        drawer@Upd)
            : > "$HD/S/AmiNetXDuo-drawer"
            lvl=AVERAGE
            answers+=("1|STRING|SYS:|Upd:")
            rm -f "$UPD_HDF"
            "$XDFTOOL" "$UPD_HDF" create size=16Mi + format Upd ffs >/dev/null ||
                { echo "base=$key result=fail reason=hdf_create"; return 1; }
            BOOT_EXTRA_CFG=$(upd_hdf_cfg rw) ;;
        *) echo "base=$key result=fail reason=layout"; return 1 ;;
    esac
    # Every page Average draws takes its default but this one: micro's
    # default is to start nothing at boot, and every scenario needs the
    # network the old release starts.
    [ "$lvl" = NOVICE ] || answers+=("1|BOOL|Yes, at boot")
    rm -f "$HD/install-scenario.txt"
    if [ "${#answers[@]}" -gt 0 ]; then
        printf '%s\n' "${answers[@]}" > "$HD/install-scenario.txt"
        chmod 644 "$HD/install-scenario.txt"
    fi
    local _lvl="$LEVEL"
    LEVEL="$lvl"
    build_driver "$ROOT/build/installdrive-$TAG-upd-$key" 1 "" "$spec" >/dev/null
    LEVEL="$_lvl"
    cp "$ROOT/build/installdrive-$TAG-upd-$key" "$HD/C/installdrive"
    chmod 755 "$HD/C/installdrive"
    startup_with "FailAt 9999
C:installdrive >DH0:install-console.txt
Set scnrc \$RC
$UPD_SETTLE
Echo >DH0:.done \"\$scnrc\""
    boot "upd-install-$key" "$INSTALL_TIMEOUT"
    st=$BOOT_STATUS
    mkdir -p "$UPD_OUT/base-$key"
    for f in installdrive.txt install-log.txt install-console.txt installer-out.txt; do
        [ ! -f "$HD/$f" ] || mv "$HD/$f" "$UPD_OUT/base-$key/"
    done
    rm -f "$HD/C/installdrive" "$HD/install-scenario.txt" "$HD/S/AmiNetXDuo-drawer"
    rm -f "$HD/.done" "$HD/progress.txt"
    # The flavour asked for, byte for byte: its bsdsocket.library is the old
    # manifest's row for it.
    want=$(awk -v f="$flavour" '$1 == "file=" f && $2 == "LIBS" && $5 == "bsdsocket.library" { print $4 }' \
               "$UPD_OLD_MANIFEST")
    got=""
    case "$layout" in
        system) lib="$HD/Libs/bsdsocket.library"
                got=$(sha256_file "$lib") ;;
        drawer) lib="$HD/AmiNetXDuo/Libs/bsdsocket.library"
                got=$(sha256_file "$lib") ;;
        drawer@Upd)
            if upd_tree "$UPD_STORE/tree"; then
                got=$(sha256_file "$UPD_STORE/tree/Upd/AmiNetXDuo/Libs/bsdsocket.library")
            fi ;;
    esac
    if [ "$st" != 0 ] || [ -z "$want" ] || [ "$got" != "$want" ]; then
        echo "base=$key result=fail install_status=$st flavour_sha_ok=no wall=$(( $(date +%s) - t0 ))"
        mkdir -p "$dir"; : > "$dir/failed"
        return 1
    fi
    upd_save "$dir"
    : > "$dir/ok"
    echo "base=$key result=ok install_status=$st flavour_sha_ok=yes wall=$(( $(date +%s) - t0 ))"
}

# ---------------------------------------------------- per-row resolution --

# Host path of <f> (relative to the layout's drawers) on DH0:.
upd_hpath() {
    case "$S_LAYOUT" in
        system) printf '%s/%s\n' "$HD" "$1" ;;
        *)      printf '%s/AmiNetXDuo/%s\n' "$HD" "$1" ;;
    esac
}

# The tree the checks read: DH0:, or the Upd volume unpacked.
upd_root() { # before|after
    if [ "$S_LAYOUT" = drawer@Upd ]; then
        upd_tree "$UPD_STORE/tree-$1" >/dev/null 2>&1 || true
        printf '%s\n' "$UPD_STORE/tree-$1/Upd"
    else
        printf '%s\n' "$HD"
    fi
}

# Destination of a manifest row, relative to the tree root: the layout's
# drawer first, the system's second, and the first that exists in $1.
upd_dest() { # root class path
    local root="$1" cls="$2" p="$3" c
    local -a cands
    case "$cls" in
        LIBS)          cands=(AmiNetXDuo/Libs Libs) ;;
        DEVS/Networks) cands=(AmiNetXDuo/Devs/Networks Devs/Networks) ;;
        C)             cands=(AmiNetXDuo/C C) ;;
        certificates)  cands=(AmiNetXDuo/Devs/Internet Devs/Internet) ;;
        *) return 1 ;;
    esac
    [ "$S_LAYOUT" = system ] && cands=("${cands[1]}" "${cands[0]}")
    for c in "${cands[@]}"; do
        [ -f "$root/$c/$p" ] && { printf '%s/%s\n' "$c" "$p"; return 0; }
    done
    return 1
}

# Every product file, and the Upd volume's, minus what is not the product's:
# the harness's own Startup-Sequence, the settings and hooks it writes, and
# the updater's log and state, which a refusal is allowed to write.
upd_manifest() { # before|after
    local r
    product_manifest
    if [ "$S_LAYOUT" = drawer@Upd ]; then
        r=$(upd_root "$1")
        ( cd "$r" && find . -type f -print0 | sort -z | xargs -0 -r sha256sum |
          sed 's|  \./|  Upd/|' )
    fi
}

upd_manifest_filtered() {
    upd_manifest "$1" | grep -v -e 'S/Startup-Sequence$' \
        -e 'Prefs/Env-Archive/AmiNetXDuo$' -e 'Prefs/Env-Archive/AmiNetXDuo/' \
        -e 'AmiNetXDuo/Installed/Update/log$' \
        -e 'AmiNetXDuo/Installed/Update/state$' || true
}

# ------------------------------------------------------------ the guest --

upd_settings() { # session token
    local envarc dir
    envarc=$(amiga_path Prefs/Env-Archive 2>/dev/null || true)
    [ -n "$envarc" ] || { envarc="$HD/Prefs/Env-Archive"; mkdir -p "$envarc"; }
    dir="$envarc/AmiNetXDuo"
    mkdir -p "$dir"
    if [ "$S_NOPREFS" = 0 ]; then
        {
            echo "AUTO=$S_AUTO"
            [ -z "$S_CHANNEL" ] || echo "CHANNEL=$S_CHANNEL"
            echo "MIRROR=$UPD_MIRROR/t/$2/$S_TREE/"
        } > "$dir/Update"
    fi
    if [ -n "$S_ANSWER" ]; then
        echo "$S_ANSWER" > "$dir/UpdateAnswer"
    else
        rm -f "$dir/UpdateAnswer"
    fi
    if [ -n "${S_STOP:-}" ]; then
        echo "$S_STOP DH0:.done" > "$dir/UpdateTestStop"
    else
        rm -f "$dir/UpdateTestStop"
    fi
    UPD_ENVARC_DIR="$dir"
}

# The two helper scripts the segments Execute.
upd_scripts() {
    cat > "$HD/scn/waitnet" <<'EOF'
FailAt 9999
Set scnn 0
Lab top
GetNetStatus >NIL: CHECK=DEFAULTROUTE QUIET
If NOT WARN
  Echo >DH0:scn/net.txt "net=up waited=$scnn"
  Skip done
EndIf
Wait 1
Set scnn `Eval $scnn + 1`
Echo >DH0:scn/hb "$scnn"
If VAL $scnn LT 90
  Skip top BACK
EndIf
Echo >DH0:scn/net.txt "net=down waited=$scnn"
Lab done
EOF
    # $1 is the command name; waits while it runs, at most 240 s.  Skip to
    # an end label, never Quit: Quit in an Executed script ends the boot's
    # own script as well.
    cat > "$HD/scn/waitgone" <<'EOF'
.key name/a
FailAt 9999
Set scng 0
Lab top
Status >NIL: COMMAND=<name>
If WARN
  Echo >>DH0:scn/gone.txt "<name>=gone waited=$scng"
  Skip done
EndIf
Wait 1
Set scng `Eval $scng + 1`
Echo >DH0:scn/hb "$scng"
If VAL $scng LT 240
  Skip top BACK
EndIf
Echo >>DH0:scn/gone.txt "<name>=running waited=$scng"
Lab done
EOF
    chmod 644 "$HD/scn/waitnet" "$HD/scn/waitgone"
}

# One segment: what one guest boot does after the startup.  $1 session,
# $2 segment, $3 the comma list of commands, $4 first segment of the row.
UPD_CMDNO=0
upd_segment() {
    local s="$1" g="$2" words="$3" first="$4" w cmd
    local f="$HD/scn/s$s-$g.seg"
    {
        echo "FailAt 9999"
        echo "Echo >>DH0:progress.txt \"scn: session $s segment $g\""
        if [ "$S_NONET" = 0 ]; then
            echo "Execute DH0:scn/waitnet"
            echo "Type DH0:scn/net.txt >>DH0:scn/s$s-$g-net.txt"
            echo "ShowNetStatus >DH0:scn/s$s-$g-netstatus.txt"
        fi
        if [ "$first" = 1 ]; then
            [ "$S_NONET" = 1 ] ||
                echo "fetch $UPD_MIRROR/t/$UPD_TOKEN/probe TO DH0:scn/probe.txt QUIET"
            echo "Which >DH0:scn/which.txt UpdateAmiNetXDuo"
            echo "If WARN"
            echo "  Echo >DH0:scn/tool.txt \"tool=missing\""
            echo "Else"
            echo "  Echo >DH0:scn/tool.txt \"tool=present\""
            echo "EndIf"
            printf '%s\n' "${S_PROBES[@]}"
        fi
        for w in ${words//,/ }; do
            case "$w" in
                -|startup)
                    echo "Execute DH0:scn/waitgone UpdateAmiNetXDuo"
                    continue ;;
                check)        cmd="CHECK" ;;
                check=*)      cmd="CHECK ANSWER=$(echo "${w#check=}" | tr '[:lower:]' '[:upper:]')" ;;
                install)      cmd="INSTALL" ;;
                install+restart) cmd="INSTALL RESTART" ;;
                *) echo "; unknown word $w" ;;
            esac
            UPD_CMDNO=$((UPD_CMDNO + 1))
            if [ "$S_CLOCK" = 1978 ]; then
                echo "Execute DH0:scn/waitgone sntp"
                echo "Date 01-Jan-78 12:00:00"
                echo "Date >DH0:scn/date.txt"
            fi
            echo "Which >NIL: UpdateAmiNetXDuo"
            echo "If NOT WARN"
            echo "  UpdateAmiNetXDuo $cmd >>DH0:scn/out.txt"
            echo "  Echo >>DH0:scn/rc.txt \"cmd=$UPD_CMDNO rc=\$RC\""
            echo "Else"
            echo "  Echo >>DH0:scn/rc.txt \"cmd=$UPD_CMDNO rc=missing\""
            echo "EndIf"
        done
    } > "$f"
    chmod 644 "$f"
}

# The tail of one boot session: its segments in order, each once.  A
# segment the guest rebooted out of (restart) is not run again; the next is.
upd_tail() { # session nsegments
    local s="$1" n="$2" g
    printf 'FailAt 9999\n'
    printf '%s\n' "${S_PRE[@]}"
    for ((g = 1; g <= n; g++)); do
        printf 'If NOT EXISTS DH0:scn/s%s-%s.started\n' "$s" "$g"
        printf '  Echo >DH0:scn/s%s-%s.started x\n' "$s" "$g"
        printf '  Execute DH0:scn/s%s-%s.seg\n' "$s" "$g"
        printf 'EndIf\n'
    done
    printf '%s\n' "$UPD_SETTLE"
    printf 'Echo >DH0:.done "0"\n'
}

# Which mirror class a session's requests make: none | manifest | container,
# with the channel folder.  $1 is the token.
upd_mirror_class() {
    local lines cls=none ch=""
    lines=$(ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$UPD_PEER" \
                "grep -F 'token=$1 ' '$UPD_PEER_LOG'" 2>/dev/null | grep -v ' path=probe ' || true)
    printf '%s\n' "$lines" | sed '/^$/d' >> "$UPD_EV/mirror.txt"
    if printf '%s\n' "$lines" | grep -q 'path=[^ ]*\.lha '; then
        cls=container
    elif printf '%s\n' "$lines" | grep -q 'path=[^ ]*/update\.manifest'; then
        cls=manifest
    fi
    ch=$(printf '%s\n' "$lines" | sed -n 's|.* path=[^/ ]*/\([^/ ]*\)/update\.manifest .*|\1|p' | head -1)
    printf '%s%s\n' "$cls" "${ch:+@$ch}"
}

# ----------------------------------------------------------------- checks --

UPD_CHECK_FAIL=""
upd_check() { # name ok detail
    echo "check=$1 scenario=$S_ID ok=$2${3:+ $3}"
    [ "$2" = yes ] || [ -n "$UPD_CHECK_FAIL" ] || UPD_CHECK_FAIL="$1"
}

UPD_SETUP_FAIL=""
upd_setup_ok() { # item ok
    echo "setup=$1 scenario=$S_ID ok=$2"
    [ "$2" = yes ] || [ -n "$UPD_SETUP_FAIL" ] || UPD_SETUP_FAIL="$1"
}

upd_subst() { local v="$1"; v=${v//@NEW/$NEW_VERSION}; printf '%s\n' "${v//@OLD/$OLD_VERSION}"; }

# --------------------------------------------------------------- one row --

upd_row() {
    IFS=$'\t' read -r S_ID S_OS S_LAYOUT S_FLAVOUR S_SETUP S_ACTION S_FILES \
        S_RC S_LOG S_REQ S_TREE S_MIRROR S_PR S_STATUS <<< "$1"
    local t0 item f p old_sum sessions=() s seg nsess nseg boots=0 tok
    local before_root after_root before_man after_man
    t0=$(date +%s)
    UPD_CHECK_FAIL=""; UPD_SETUP_FAIL=""
    UPD_EV="$UPD_OUT/$S_ID"
    rm -rf "$UPD_EV"; mkdir -p "$UPD_EV"
    UPD_CMDNO=0

    if ! upd_base "$S_LAYOUT" "$S_FLAVOUR"; then
        echo "scenario=$S_ID result=FAIL reason=base_install wall=$(( $(date +%s) - t0 )) boots=0"
        UPD_RESULT=FAIL; return 0
    fi
    upd_restore "$UPD_STORE/base-$UPD_BASE_KEY"
    rm -rf "$HD/scn"; mkdir -p "$HD/scn"
    upd_scripts

    S_AUTO=OFF; S_CHANNEL=""; S_ANSWER=""; S_NOPREFS=0; S_CLOCK=""
    S_NONET=0; S_MODE=""; S_STOP=""; S_PRE=(); S_PROBES=(); S_GUEST=()
    BOOT_EXTRA_CFG=""
    [ "$S_LAYOUT" != drawer@Upd ] || BOOT_EXTRA_CFG=$(upd_hdf_cfg rw)

    # ---- setup, host side, each checked
    local IFS_SAVE="$IFS"
    IFS=','
    local -a items=($S_SETUP)
    IFS="$IFS_SAVE"
    for item in "${items[@]}"; do
        [ "$item" = - ] && continue
        case "$item" in
            foreign=*|otherbuild=*|newer=*)
                f=${item#*=}; p=$(upd_hpath "$f")
                if [ ! -f "$p" ]; then upd_setup_ok "$item" no; continue; fi
                case "$item" in
                    foreign=*)    printf 'a %s from another vendor\n$VER: %s 3.2 (1.1.2020)\n' "${f##*/}" "${f##*/}" > "$p" ;;
                    otherbuild=*) printf '$VER: %s 1.0.0-rc1 (1.1.2026) AmiNetXDuo, another build\n' "${f##*/}" > "$p" ;;
                    newer=*)      printf '$VER: %s 9.9.9 (1.1.2030) AmiNetXDuo, a newer build\n' "${f##*/}" > "$p" ;;
                esac
                chmod 755 "$p"
                case "$item" in
                    foreign=*) grep -q AmiNetXDuo "$p" && upd_setup_ok "$item" no || upd_setup_ok "$item" yes ;;
                    *)         grep -q AmiNetXDuo "$p" && upd_setup_ok "$item" yes || upd_setup_ok "$item" no ;;
                esac ;;
            delete=*)
                p=$(upd_hpath "${item#*=}")
                [ -f "$p" ] && rm -f "$p" && [ ! -e "$p" ] &&
                    upd_setup_ok "$item" yes || upd_setup_ok "$item" no ;;
            protect=*)
                # By the guest, in a setup boot of its own: Amiberry keeps the
                # d bit of a directory volume's file outside its host mode.
                # A directory volume cannot hold a cleared d bit (Amiberry
                # maps only r, w and e to the host mode), so the rows put it
                # on the drawer@Upd FFS volume.
                f=${item#*=}
                case "$S_LAYOUT" in
                    system) f="C:${f#C/}" ;;
                    *)      f="AmiNetXDuo:$f" ;;
                esac
                S_GUEST+=("Protect $f wd SUB" "List >DH0:scn/protect.txt $f") ;;
            foreignold=*)
                p=$(upd_hpath "${item#*=}")
                printf 'a foreign %s.old another stack keeps for Uninstall\n' "${p##*/}" > "$p.old"
                chmod 644 "$p.old"
                [ -f "$p" ] && [ -f "$p.old" ] && upd_setup_ok "$item" yes || upd_setup_ok "$item" no ;;
            noreceipt)
                rm -f "$HD/AmiNetXDuo/Installed/Update/receipt" "$HD/S/AmiNetXDuo-Install"
                [ ! -e "$HD/AmiNetXDuo/Installed/Update/receipt" ] && [ ! -e "$HD/S/AmiNetXDuo-Install" ] &&
                    upd_setup_ok "$item" yes || upd_setup_ok "$item" no ;;
            nosntp)
                p=$(upd_hpath C/sntp)
                [ -f "$p" ] && mv "$p" "$p.scnoff" && upd_setup_ok "$item" yes || upd_setup_ok "$item" no ;;
            clock=1978)
                S_CLOCK=1978 ;;
            auto=on)  S_AUTO=ON ;;
            auto=off) S_AUTO=OFF ;;
            channel=release)    S_CHANNEL=RELEASE ;;
            channel=prerelease) S_CHANNEL=PRERELEASE ;;
            answer=*) S_ANSWER=${item#answer=} ;;
            noprefs)  S_NOPREFS=1 ;;
            foreignstack)
                "$GCC" -O2 -m68000 -Wall -Wextra -I"$NDK" -o "$HD/scn/foreignlib" \
                    "$ROOT/tests/updater/foreignlib.c" >/dev/null 2>&1 &&
                    chmod 755 "$HD/scn/foreignlib" || { upd_setup_ok "$item" no; continue; }
                S_MODE=nouserstartup; S_NONET=1
                S_PRE+=("Run >NIL: DH0:scn/foreignlib" "Wait 2")
                S_PROBES+=("Version >DH0:scn/foreignstack.txt bsdsocket.library") ;;
            wpvolume)
                BOOT_EXTRA_CFG=$(upd_hdf_cfg ro)
                # Read from Info: a test write would stop the boot on DOS's
                # "write protected" requester.
                S_PROBES+=("Info >DH0:scn/info.txt Upd:") ;;
            fullvolume)
                local freeb fill
                freeb=$("$XDFTOOL" "$UPD_HDF" info 2>/dev/null | awk '$1 == "free:" { print $2 }')
                # FFS: 512 data bytes a block, a header or extension block
                # per 72 of them.
                fill=$(( (freeb - freeb / 72 - 2 - 64) * 512 ))
                head -c "$fill" /dev/zero > "$UPD_STORE/filler"
                "$XDFTOOL" "$UPD_HDF" write "$UPD_STORE/filler" filler >/dev/null 2>&1 || true
                rm -f "$UPD_STORE/filler"
                freeb=$("$XDFTOOL" "$UPD_HDF" info 2>/dev/null | awk '$1 == "free:" { print $2 }')
                echo "fullvolume_free_blocks=$freeb" >> "$UPD_EV/setup.txt"
                [ -n "$freeb" ] && [ "$freeb" -le 128 ] && [ "$freeb" -ge 16 ] &&
                    upd_setup_ok "$item" yes || upd_setup_ok "$item" no
                S_PROBES+=("Info >DH0:scn/info.txt Upd:") ;;
            *) upd_setup_ok "$item" no ;;
        esac
    done

    # ---- the boots: sessions split at reboots, segments at restarts
    local -a segs
    IFS=';' read -ra segs <<< "$S_ACTION"
    local -a sess_words=() sess_kinds=()
    local cur="" kind="first" word rest
    for seg in "${segs[@]}"; do
        word=${seg%%,*}
        rest=""; [ "$word" = "$seg" ] || rest=${seg#*,}
        case "$word" in
            reboot|reboot+1d|reboot+cut=*)
                [ -z "$cur" ] || { sess_words+=("$cur"); sess_kinds+=("$kind"); }
                kind="$word"; cur="${rest:--}" ;;
            restart)
                cur="$cur|${rest:--}" ;;
            *)
                cur="$seg" ;;
        esac
    done
    sess_words+=("$cur"); sess_kinds+=("$kind")
    nsess=${#sess_words[@]}

    if [ "$S_ACTION" = cut=each ]; then
        upd_cut_each "$t0"
        return 0
    fi

    # Setup only the guest can make, in a boot of its own before the
    # snapshot, so what it changes is the starting state.
    if [ "${#S_GUEST[@]}" -gt 0 ]; then
        startup_with "$(printf 'FailAt 9999\n'; printf '%s\n' "${S_GUEST[@]}"; printf '%s\n' "$UPD_SETTLE"; printf 'Echo >DH0:.done "0"\n')"
        boot "upd-$S_ID-setup" "$INSTALL_TIMEOUT"
        echo "setup_boot scenario=$S_ID status=$BOOT_STATUS" >> "$UPD_EV/sessions.txt"
    fi

    before_root=$(upd_root before)
    before_man=$(upd_manifest_filtered before)
    printf '%s\n' "$before_man" > "$UPD_EV/manifest-before.txt"
    declare -gA UPD_BEFORE=()
    upd_hash_rows "$before_root" UPD_BEFORE
    declare -gA UPD_KEEP=()
    IFS=','; local -a fitems=($S_FILES); IFS="$IFS_SAVE"
    for item in "${fitems[@]}"; do
        case "$item" in
            keep=*) f=$(upd_rel "${item#keep=}")
                    UPD_KEEP["$f"]=$(sha256_file "$before_root/$f") ;;
        esac
    done
    local user_startup_before files_rec_before
    user_startup_before=$(sha256_file "$HD/S/User-Startup")
    files_rec_before=$(sha256_file "$HD/S/AmiNetXDuo-Files")

    local -a mclass=()
    for ((s = 1; s <= nsess; s++)); do
        kind=${sess_kinds[$((s - 1))]}
        IFS='|' read -ra parts <<< "${sess_words[$((s - 1))]}"
        nseg=${#parts[@]}
        S_STOP=""
        case "$kind" in
            reboot+1d) upd_day_back ;;
            reboot+cut=*) S_STOP=$(echo "${kind#reboot+cut=}" | tr ':' ' ') ;;
        esac
        UPD_TOKEN="$UPD_RUN-$S_ID-s$s"
        upd_settings "$s" "$UPD_TOKEN"
        for ((g = 1; g <= nseg; g++)); do
            local first=0
            [ "$s" = 1 ] && [ "$g" = 1 ] && first=1
            upd_segment "$s" "$g" "${parts[$((g - 1))]}" "$first"
        done
        startup_with "$(upd_tail "$s" "$nseg")" $S_MODE
        boot "upd-$S_ID-s$s" "$BOOT_TIMEOUT" net
        boots=$((boots + 1))
        echo "session=$s scenario=$S_ID status=$BOOT_STATUS done=$(tr -d '\n' 2>/dev/null < "$HD/.done")" >> "$UPD_EV/sessions.txt"
        mclass+=("$(upd_mirror_class "$UPD_TOKEN")")
        # A session that did not end with its own .done is a defect of its
        # own, whatever the row expects (a cut ends with "cut <n>").
        case "$(tr -d '\n' 2>/dev/null < "$HD/.done")" in
            0) ;;
            cut*) [ -n "$S_STOP" ] || upd_check session_end no "session=$s done=cut" ;;
            *) upd_check session_end no "session=$s status=$BOOT_STATUS" ;;
        esac
    done
    cp -a "$HD/scn" "$UPD_EV/scn" 2>/dev/null || true

    # ---- setup checks the guest answered
    for item in "${items[@]}"; do
        case "$item" in
            protect=*)
                # List's field is hsparwed: w and d cleared.
                if grep -owE '[-hsparwed]{8}' "$HD/scn/protect.txt" 2>/dev/null |
                        head -1 | grep -q '^.....-.-$'; then
                    upd_setup_ok "$item" yes
                else
                    upd_setup_ok "$item" no
                fi ;;
            clock=1978)
                grep -q -e '-78' -e '1978' "$HD/scn/date.txt" 2>/dev/null &&
                    upd_setup_ok "$item" yes || {
                        # Never reached: the tool is missing, nothing to set.
                        [ -f "$HD/scn/date.txt" ] && upd_setup_ok "$item" no ||
                            upd_setup_ok "$item" yes
                    } ;;
            foreignstack)
                grep -q 'bsdsocket.library 4.364' "$HD/scn/foreignstack.txt" 2>/dev/null &&
                    upd_setup_ok "$item" yes || upd_setup_ok "$item" no ;;
            wpvolume)
                grep -qi 'Read Only' "$HD/scn/info.txt" 2>/dev/null &&
                    upd_setup_ok "$item" yes || upd_setup_ok "$item" no ;;
        esac
    done
    if [ "$S_NONET" = 0 ]; then
        grep -q "^probe $UPD_RUN-$S_ID-s1\$" "$HD/scn/probe.txt" 2>/dev/null &&
            upd_setup_ok mirror_probe yes || upd_setup_ok mirror_probe no
        grep -q 'net=up' "$UPD_EV/scn/s1-1-net.txt" 2>/dev/null &&
            upd_setup_ok old_install_online yes || upd_setup_ok old_install_online no
    fi
    if foreign_intact; then upd_check foreign_intact yes; else upd_check foreign_intact no; fi

    # ---- checks
    after_root=$(upd_root after)
    after_man=$(upd_manifest_filtered after)
    printf '%s\n' "$after_man" > "$UPD_EV/manifest-after.txt"
    local unchanged=no
    [ "$before_man" = "$after_man" ] && unchanged=yes
    echo "unchanged=$unchanged scenario=$S_ID"
    [ "$unchanged" = yes ] || diff <(printf '%s\n' "$before_man") <(printf '%s\n' "$after_man") \
        > "$UPD_EV/manifest.diff" || true

    local tool
    tool=$(sed -n 's/^tool=//p' "$HD/scn/tool.txt" 2>/dev/null | head -1)
    upd_check tool "$([ "$tool" = present ] && echo yes || echo no)" "tool=${tool:-unknown}"

    upd_files_checks "$before_root" "$after_root" "$unchanged" "$files_rec_before"
    upd_rc_check
    upd_log_checks
    upd_mirror_check "${mclass[@]}"

    # ---- verdict
    local result reason
    if [ -n "$UPD_SETUP_FAIL" ]; then
        result=FAIL; reason="setup:$UPD_SETUP_FAIL"
    elif [ "$tool" != present ]; then
        reason=tool_missing
        [ "$S_STATUS" = expected-fail ] && result=XFAIL || result=FAIL
    elif [ -z "$UPD_CHECK_FAIL" ]; then
        reason=ok
        [ "$S_STATUS" = expected-fail ] && result=XPASS || result=PASS
    else
        reason="$UPD_CHECK_FAIL"
        [ "$S_STATUS" = expected-fail ] && result=XFAIL || result=FAIL
    fi
    echo "scenario=$S_ID result=$result reason=$reason pr=$S_PR wall=$(( $(date +%s) - t0 )) boots=$boots"
    UPD_RESULT=$result
}

# Every row of the installed flavour in the NEW manifest: its destination
# and the hash it has under $1, into the named array (dest -> sha).
UPD_ROWS=()
upd_hash_rows() { # root arrayname
    local root="$1" line cls p d
    local -n arr="$2"
    UPD_ROWS=()
    while read -r _ cls _ _ p; do
        d=$(upd_dest "$root" "$cls" "$p") || continue
        UPD_ROWS+=("$cls|$p|$d")
        arr["$d"]=$(sha256_file "$root/$d")
    done < <(awk -v f="$S_FLAVOUR" '$1 == "file=" f' "$UPD_NEW_MANIFEST")
}

# <f> as the TSV names it, relative to the layout's drawers, to a tree path.
upd_rel() { case "$S_LAYOUT" in system) echo "$1" ;; *) echo "AmiNetXDuo/$1" ;; esac; }

upd_new_sha() { # class path
    awk -v f="$S_FLAVOUR" -v c="$1" -v p="$2" '$1 == "file=" f && $2 == c && $5 == p { print $4; exit }' \
        "$UPD_NEW_MANIFEST"
}

upd_files_checks() { # before_root after_root unchanged files_rec_before
    local br="$1" ar="$2" item row cls p d want got n_new=0 n_ok=0 n_old=0 rel
    local IFS_SAVE="$IFS" mods=() keep=() absent=() olds=()
    IFS=','; local -a items=($S_FILES); IFS="$IFS_SAVE"
    for item in "${items[@]}"; do
        case "$item" in
            keep=*)   keep+=("${item#keep=}") ;;
            absent=*) absent+=("${item#absent=}") ;;
            old=*)    olds+=("${item#old=}") ;;
        esac
    done
    local skip=" "
    for p in "${keep[@]}" "${absent[@]}" "${olds[@]}"; do skip="$skip$(upd_rel "$p") "; done
    for row in "${UPD_ROWS[@]}"; do
        IFS='|' read -r cls p d <<< "$row"
        case "$skip" in *" $d "*) continue ;; esac
        want=$(upd_new_sha "$cls" "$p")
        got=$(sha256_file "$ar/$d")
        n_new=$((n_new + 1))
        [ "$got" = "$want" ] && n_ok=$((n_ok + 1))
        [ "$got" = "${UPD_BEFORE[$d]}" ] && n_old=$((n_old + 1))
    done
    for item in "${items[@]}"; do
        case "$item" in
            unchanged) upd_check files_unchanged "$3" ;;
            new)  upd_check files_new "$([ "$n_new" -gt 0 ] && [ "$n_ok" = "$n_new" ] && echo yes || echo no)" "new=$n_ok/$n_new" ;;
            old)  upd_check files_old "$([ "$n_new" -gt 0 ] && [ "$n_old" = "$n_new" ] && echo yes || echo no)" "old=$n_old/$n_new" ;;
            atomic) upd_check files_atomic "$([ "$n_new" -gt 0 ] && { [ "$n_old" = "$n_new" ] || [ "$n_ok" = "$n_new" ]; } && echo yes || echo no)" "old=$n_old new=$n_ok of=$n_new" ;;
            keep=*)
                rel=$(upd_rel "${item#keep=}")
                upd_check "keep:${item#keep=}" "$([ -f "$ar/$rel" ] && [ "$(sha256_file "$ar/$rel")" = "${UPD_KEEP[$rel]:-x}" ] && echo yes || echo no)" ;;
            absent=*)
                rel=$(upd_rel "${item#absent=}")
                upd_check "absent:${item#absent=}" "$([ ! -e "$ar/$rel" ] && echo yes || echo no)" ;;
            old=*)
                rel=$(upd_rel "${item#old=}")
                upd_check "old:${item#old=}" "$([ -f "$ar/$rel.old" ] && [ "$(sha256_file "$ar/$rel.old")" = "${UPD_BEFORE[$rel]:-x}" ] && echo yes || echo no)" ;;
            receipt)
                local rc_ok=yes rf="$ar/AmiNetXDuo/Installed/Update/receipt"
                [ -f "$rf" ] || rc_ok=no
                if [ "$rc_ok" = yes ]; then
                    for row in "${UPD_ROWS[@]}"; do
                        IFS='|' read -r cls p d <<< "$row"
                        grep -q "^$(upd_new_sha "$cls" "$p") " "$rf" || { rc_ok=no; break; }
                    done
                fi
                upd_check receipt "$rc_ok" ;;
            filesrec)
                local fr_ok=yes ff="$HD/S/AmiNetXDuo-Files"
                [ -f "$ff" ] && [ "$(sha256_file "$ff")" != "$4" ] || fr_ok=no
                if [ "$fr_ok" = yes ]; then
                    for row in "${UPD_ROWS[@]}"; do
                        IFS='|' read -r cls p d <<< "$row"
                        grep -qiF "${p##*/}" "$ff" || { fr_ok=no; break; }
                    done
                fi
                upd_check filesrec "$fr_ok" ;;
            nostaging)
                local left
                left=$( { find "$ar" -name '*.new' -o -name '*.update.lha'; } 2>/dev/null |
                        grep -v '/Unpacked/' | head -3 | tr '\n' ' ')
                upd_check nostaging "$([ -z "$left" ] && echo yes || echo no)" "${left:+left=$left}" ;;
        esac
    done
}

upd_rc_check() {
    [ "$S_RC" = - ] && return 0
    local got
    got=$(sed -n 's/^cmd=[0-9]* rc=//p' "$HD/scn/rc.txt" 2>/dev/null | tr -d '\r' | paste -sd, -)
    upd_check rc "$([ "$got" = "$S_RC" ] && echo yes || echo no)" "want=$S_RC got=${got:-none}"
}

upd_log_checks() {
    local text state us envarc item it ok
    text=$( { cat "$HD/scn/out.txt" 2>/dev/null
              cat "$(upd_root after)/AmiNetXDuo/Installed/Update/log" 2>/dev/null; } | tr -d '\r')
    state=$(tr -d '\r' 2>/dev/null < "$(upd_root after)/AmiNetXDuo/Installed/Update/state" || true)
    us=$(tr -d '\r' 2>/dev/null < "$HD/S/User-Startup" || true)
    envarc=$(tr -d '\r' 2>/dev/null < "$UPD_ENVARC_DIR/Update" || true)
    printf '%s\n' "$text" > "$UPD_EV/log.txt"
    if [ "$S_LOG" != - ]; then
        IFS=';' read -ra its <<< "$S_LOG"
        for it in "${its[@]}"; do
            item=$(upd_subst "$it")
            case "$item" in
                '!'*)       ! printf '%s\n' "$text" | grep -qF -- "${item#!}" && ok=yes || ok=no ;;
                state:*)    printf '%s\n' "$state" | grep -q "^${item#state:}" && ok=yes || ok=no ;;
                ustartup:*) printf '%s\n' "$us" | grep -qF -- "${item#ustartup:}" && ok=yes || ok=no ;;
                envarc:*)   printf '%s\n' "$envarc" | grep -q "^${item#envarc:}" && ok=yes || ok=no ;;
                *)          printf '%s\n' "$text" | grep -q "^$item" && ok=yes || ok=no ;;
            esac
            upd_check "log:$item" "$ok"
        done
    fi
    if [ "$S_REQ" = - ]; then
        printf '%s\n' "$text" | grep -q '^requester=' && ok=no || ok=yes
    else
        printf '%s\n' "$text" | grep -q "^requester=$S_REQ\\b" && ok=yes || ok=no
    fi
    upd_check "requester:$S_REQ" "$ok"
}

upd_mirror_check() {
    [ "$S_MIRROR" = - ] && return 0
    local -a want
    IFS=',' read -ra want <<< "$S_MIRROR"
    local got w g i ok=yes
    got=$(IFS=,; echo "$*")
    [ "${#want[@]}" = "$#" ] || ok=no
    i=0
    for g in "$@"; do
        w=${want[$i]:-}
        case "$w" in
            *@*) [ "$g" = "$w" ] || ok=no ;;
            *)   [ "${g%@*}" = "$w" ] || ok=no ;;
        esac
        i=$((i + 1))
    done
    upd_check mirror "$ok" "want=$S_MIRROR got=$got"
}

# reboot+1d: the state's last_check and later moved one day back.  To the
# tool that is the clock one day on, and unlike the clock S:Network-Startup's
# sntp cannot set it back.
upd_day_back() {
    local st
    st=$(upd_root before)/AmiNetXDuo/Installed/Update/state
    [ -f "$st" ] || return 0
    local y
    y=$(date -u -d yesterday +%Y-%m-%d)
    sed -i -e "s/^last_check=.*/last_check=$y/" -e "s/^later=.*/later=$y/" "$st"
}

# S40: a cut at every cut point of each phase, a recovery boot after each,
# and all-old or all-new after every one.
upd_cut_each() { # t0
    local t0="$1" phase n done_txt boots=0 cuts=0 bad=0 tool="" reason result
    for phase in prepare swap; do
        n=1
        while [ "$n" -le "$UPD_CUT_MAX" ]; do
            upd_restore "$UPD_STORE/base-$UPD_BASE_KEY"
            rm -rf "$HD/scn"; mkdir -p "$HD/scn"
            upd_scripts
            declare -gA UPD_BEFORE=()
            upd_hash_rows "$HD" UPD_BEFORE
            S_STOP="$phase $n"
            UPD_TOKEN="$UPD_RUN-$S_ID-$phase$n"
            upd_settings 1 "$UPD_TOKEN"
            UPD_CMDNO=0
            upd_segment 1 1 install 1
            startup_with "$(upd_tail 1 1)"
            boot "upd-$S_ID-$phase$n" "$BOOT_TIMEOUT" net
            boots=$((boots + 1))
            tool=$(sed -n 's/^tool=//p' "$HD/scn/tool.txt" 2>/dev/null | head -1)
            done_txt=$(tr -d '\n' 2>/dev/null < "$HD/.done")
            [ "$tool" = present ] || break
            case "$done_txt" in cut*) ;; *) break ;; esac
            cuts=$((cuts + 1))
            S_STOP=""
            UPD_TOKEN="$UPD_RUN-$S_ID-$phase$n-r"
            upd_settings 2 "$UPD_TOKEN"
            upd_segment 2 1 startup 0
            startup_with "$(upd_tail 2 1)"
            boot "upd-$S_ID-$phase$n-r" "$BOOT_TIMEOUT" net
            boots=$((boots + 1))
            UPD_CHECK_FAIL=""
            local _files="$S_FILES"
            S_FILES="atomic,nostaging"
            upd_files_checks "$HD" "$HD" no "" > "$UPD_EV/cut.tmp"
            S_FILES="$_files"
            sed "s/^/cut=$phase:$n /" "$UPD_EV/cut.tmp"
            rm -f "$UPD_EV/cut.tmp"
            [ -z "$UPD_CHECK_FAIL" ] || bad=$((bad + 1))
            n=$((n + 1))
        done
        [ "$tool" = present ] || break
    done
    if [ "$tool" != present ]; then
        reason=tool_missing
        [ "$S_STATUS" = expected-fail ] && result=XFAIL || result=FAIL
    elif [ "$cuts" -gt 0 ] && [ "$bad" = 0 ]; then
        reason=ok
        [ "$S_STATUS" = expected-fail ] && result=XPASS || result=PASS
    else
        reason="cuts=$cuts mixed=$bad"
        [ "$S_STATUS" = expected-fail ] && result=XFAIL || result=FAIL
    fi
    upd_check tool "$([ "$tool" = present ] && echo yes || echo no)" "tool=${tool:-unknown}"
    echo "scenario=$S_ID result=$result reason=$reason pr=$S_PR wall=$(( $(date +%s) - t0 )) boots=$boots cuts=$cuts"
    UPD_RESULT=$result
}

# ------------------------------------------------------------------- rows --

UPD_N=0; UPD_PASS=0; UPD_FAIL=0; UPD_XFAIL=0; UPD_XPASS=0
# fd 3: a boot, ssh or the emulator reading stdin would eat the rows.
while IFS= read -r line <&3; do
    case "$line" in ''|'#'*|id$'\t'*) continue ;; esac
    id=${line%%$'\t'*}
    rest=${line#*$'\t'}
    os=${rest%%$'\t'*}
    [ "$os" = "$WBVER" ] || continue
    case ",$SCENARIO," in
        ,all,|*",$id,"*) ;;
        *) continue ;;
    esac
    UPD_RESULT=FAIL
    upd_row "$line" || UPD_RESULT=FAIL
    UPD_N=$((UPD_N + 1))
    case "$UPD_RESULT" in
        PASS) UPD_PASS=$((UPD_PASS + 1)) ;;
        XFAIL) UPD_XFAIL=$((UPD_XFAIL + 1)) ;;
        XPASS) UPD_XPASS=$((UPD_XPASS + 1)) ;;
        *) UPD_FAIL=$((UPD_FAIL + 1)) ;;
    esac
done 3< "$UPD_TSV"

echo "scenarios=$UPD_N pass=$UPD_PASS fail=$UPD_FAIL xfail=$UPD_XFAIL xpass=$UPD_XPASS os=$WBVER"
if [ "$UPD_N" = 0 ]; then
    UPD_EXIT=2
elif [ "$UPD_FAIL" != 0 ] || [ "$UPD_XPASS" != 0 ]; then
    UPD_EXIT=1
fi
# The snapshots are this run's; the results stay.
rm -rf "$UPD_STORE"/pristine "$UPD_STORE"/base-* "$UPD_STORE"/tree* "$UPD_HDF"
return 0
