#!/usr/bin/env bash
#
# Assemble the Developer drawer into a destination directory.
#
#   tools/stage-developer.sh <destdir>
#
# One script because there are two consumers and they must not disagree about
# what is published: dist/make-dist.sh stages it into the archive, and the
# CMake build stages it into the build tree so tests/tools/ifnames.c can be
# compiled against the drawer ALONE -- no -I into include/, no internal
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
#   ifindex.h  RFC 3493 section 4 -- the four LVOs at -0x372..-0x384.
#   in6.h      the AF_INET6 names the NDK does not define.  No vectors: it is
#              option numbers, address macros and the sockaddr_in6 warning.
#   cmsg.h     RFC 3542 ancillary data.  No vectors either: it rides
#              sendmsg/recvmsg.  Two of the NDK's own CMSG_* macros are
#              replaced here, so a caller wants it AFTER <sys/socket.h>.
#
# NOT netstatus.h: NetStackQuery and NetStackControl work and are stable, but
# publishing them freezes their request structures, and that is a decision to
# take deliberately rather than by adding a line here.  NOT bpf.h: the
# application-facing BPF surface -- BIOC*, bpf_hdr, bpf_program, bpf_stat,
# DLT_*, the BPF_* opcodes, FIONREAD, SIOCGIFADDR -- is already in the NDK's
# net/bpf.h, sys/filio.h and sys/sockio.h, and everything AMI_BPF_* in ours is
# implementation internals.
PUBLIC_HEADERS=(ifindex.h in6.h cmsg.h)

mkdir -p "$DEST/include/aminetxduo" "$DEST/sfd" "$DEST/examples"

for h in "${PUBLIC_HEADERS[@]}"; do
    cp "$ROOT/include/aminetxduo/$h" "$DEST/include/aminetxduo/$h"
done

# The generated glue is committed, so staging never needs sfdc.
cp -R "$ROOT/developer/include/." "$DEST/include/"
cp "$ROOT/developer/sfd/aminetxduo_lib.sfd" "$DEST/sfd/"
cp "$ROOT/developer/ReadMe"                 "$DEST/ReadMe"
cp "$ROOT/developer/examples/IfNames.c"     "$DEST/examples/"
cp "$ROOT/developer/examples/V6Only.c"      "$DEST/examples/"
