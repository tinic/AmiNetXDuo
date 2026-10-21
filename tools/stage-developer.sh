#!/usr/bin/env bash
#
# Assemble the Developer drawer into a destination directory.
#
#   tools/stage-developer.sh <destdir>
#
# One script because there are two consumers and they must not disagree about
# what is published: dist/make-dist.sh stages it into the archive, and the
# CMake build stages it into the build tree so tests/tools/ifnames.c can be
# compiled against the drawer ALONE, no -I into include/, no internal
# headers.  A header that is missing here fails that build.
#
# PUBLIC_HEADERS is a list and not a wildcard on purpose.  include/aminetxduo/
# is where our internal headers live too; being published is a decision per
# file, and adding one here freezes it.  docs/NDK-ADDENDUM.md.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEST="${1:?usage: stage-developer.sh <destdir>}"

# The published half of include/aminetxduo/.
#
#   ifindex.h  RFC 3493 section 4, the four LVOs at -0x372..-0x384.
#   in6.h      compatibility include for the common IPv6 SDK declarations.
#   addrinfo.h AI_ADDRCONFIG, an explicit opt-in for AmiNetXDuo 4.11+.
#   icmp6.h    ICMPv6 filter addendum, not a complete protocol header.
#   cmsg.h     IPv4 IP_PKTINFO and CMSG_BUFFER convenience macros.
#              Standard ancillary declarations come through sys/socket.h.
#   capabilities.h  identity/revision/table-size checks for private vectors.
#
# netstatus.h publishes the versioned NetStackQuery/NetStackControl request
# structures. NOT bpf.h: the
# application-facing BPF surface, BIOC*, bpf_hdr, bpf_program, bpf_stat,
# DLT_*, the BPF_* opcodes, FIONREAD, SIOCGIFADDR, is already in the NDK's
# net/bpf.h, sys/filio.h and sys/sockio.h, and everything AMI_BPF_* in ours is
# implementation internals.
#   tcp.h      TCP_USER_TIMEOUT and TCP_STALLINFO, the two IPPROTO_TCP option
#              numbers <netinet/tcp.h> has no room for. Option numbers and one
#              struct, no vectors, same shape as in6.h.
#   tlslib.h   tls.library's tags, error codes, struct TLSInfo and struct
#              TLSSelect.  The VECTORS are in developer/sfd/tls_lib.sfd like
#              every other library's, so this half is the data half; the two
#              together are what makes tls.library callable from SAS/C and
#              vbcc, which it was not.
#   anxs2ext.h the SANA-II extensions a DRIVER may implement: the direct
#              receive pair (RX_DIRECT / RX_FILLED), the link-header tag and
#              the RX_FLAGS tag with the VERIFIED checksum verdict.  GRO is
#              stack-side.  Tags and typedefs only, no vectors;
#              published so any SANA-II driver can offer them, not just ours.
PUBLIC_HEADERS=(capabilities.h ifindex.h in6.h addrinfo.h cmsg.h icmp6.h netstatus.h tcp.h tlslib.h anxs2ext.h)

# Copy only when the bytes differ.  The CMake build re-runs this on EVERY
# build (developer_drawer, tests/tools/CMakeLists.txt) so that editing a
# drawer header is not compiled from a stale stage; a plain cp gives the
# destination a new mtime every time, and IfNames.c and V6Only.c are compiled
# from the stage, so an untouched tree recompiled and relinked both of them on
# every no-op build.
stage() {
    local src="$1" dst="$2"
    mkdir -p "$(dirname "$dst")"
    cmp -s "$src" "$dst" || cp "$src" "$dst"
}

mkdir -p "$DEST/include/aminetxduo" "$DEST/sfd" "$DEST/examples"

for h in "${PUBLIC_HEADERS[@]}"; do
    stage "$ROOT/include/aminetxduo/$h" "$DEST/include/aminetxduo/$h"
done

# The pre-1.0 socket ABI correction must reach consumers through the same
# standard headers used to compile the library. This is a source-controlled
# SDK overlay, not a mutation of the user's installed compiler headers.
# Retire the exact pre-1.0 filter-only header; preserve unrelated complete
# protocol headers a caller may have installed here. POSIX cksum is available
# on the supported build hosts and fingerprints the old 1580-byte file.
obsolete="$DEST/include/netinet/icmp6.h"
if [[ -f "$obsolete" && "$(cksum < "$obsolete")" == "378839421 1580" ]]; then
    rm -- "$obsolete"
fi

for h in netinet/in.h netinet6/in6.h sys/socket.h sys/cmsg.h netdb.h net/if.h; do
    stage "$ROOT/developer/netinclude/$h" "$DEST/include/$h"
done

# The generated glue is committed, so staging never needs sfdc.  Walked rather
# than `cp -R` so that stage() sees one file at a time; the drawer's include/
# is proto/, clib/, lvo/, inline/ and pragmas/, and stage() recreates each.
while IFS= read -r f; do
    f=${f#./}
    stage "$ROOT/developer/include/$f" "$DEST/include/$f"
done < <(cd "$ROOT/developer/include" && find . -type f ! -name '.*')
stage "$ROOT/developer/sfd/aminetxduo_lib.sfd" "$DEST/sfd/aminetxduo_lib.sfd"
stage "$ROOT/developer/sfd/tls_lib.sfd"        "$DEST/sfd/tls_lib.sfd"
stage "$ROOT/developer/ReadMe"                 "$DEST/ReadMe"
stage "$ROOT/developer/netinclude/ReadMe"      "$DEST/netinclude-ReadMe"
stage "$ROOT/developer/examples/IfNames.c"     "$DEST/examples/IfNames.c"
stage "$ROOT/developer/examples/V6Only.c"      "$DEST/examples/V6Only.c"
stage "$ROOT/developer/examples/TLSInfo.c"     "$DEST/examples/TLSInfo.c"

stage "$ROOT/developer/support/in6_addr.c" "$DEST/support/in6_addr.c"
