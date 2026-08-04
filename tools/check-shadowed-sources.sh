#!/bin/sh
# Fail when a vendored NetX Duo file we shadow has changed upstream.
#
# Three files in third_party/netxduo are dropped from their glob so that a copy
# or a rewrite of ours is what the linker resolves to.  The mechanism is silent
# in both directions: the vendored file stays in the tree, still compiles, still
# shows up in `git log` when the submodule is bumped, and has no effect on the
# binary.  A fix that lands upstream is therefore invisible unless somebody
# remembers to look.
#
# That is not hypothetical.  Submodule commit 1b9fb637 replaced a file-scope
# UCHAR[128] that both TLS record paths shared and held across a suspension
# point, which leaked one session's plaintext into another's packet.  The
# submodule was bumped, the fix was in the tree, and the shipped record path
# still had the shared buffer, because the shadowing copies under
# src/tls/rfc7905 were not touched.
#
# So each shadowed file is pinned here by the hash of the vendored content its
# shadow was last reconciled against.  Upstream moving is the alarm.  Read the
# upstream change, port it or reject it on purpose, then update the hash in the
# same commit.
#
# Usage: tools/check-shadowed-sources.sh [source-root]

set -eu

ROOT=${1:-$(CDPATH= cd, "$(dirname, "$0")/.." && pwd)}

# vendored path : sha256 it was reconciled at : what shadows it
MANIFEST='
third_party/netxduo/nx_secure/src/nx_secure_tls_record_payload_encrypt.c:e4dbffdd5172b1cf2d1c35c6e0c4666a85637006a6936eb7f737eecdbeb31703:src/tls/rfc7905/nx_secure_tls_record_payload_encrypt.c
third_party/netxduo/nx_secure/src/nx_secure_tls_record_payload_decrypt.c:377035a8b7fa0bd26d277eeb85c7d8a5882129228a1adfd16ff5b2bf6156bd32:src/tls/rfc7905/nx_secure_tls_record_payload_decrypt.c
third_party/netxduo/common/src/nx_ip_checksum_compute.c:80de6d00317ac27ddbcc1679d7496c057af1f43abeeea28ed11a8053ff929a80:src/net68k/n68k_checksum.c
'

if command -v sha256sum > /dev/null 2>&1; then
    hash_of() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum > /dev/null 2>&1; then
    hash_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    echo "check-shadowed-sources: no sha256sum or shasum" >&2
    exit 1
fi

rc=0
n=0

for entry in $MANIFEST; do
    vendored=${entry%%:*}
    rest=${entry#*:}
    want=${rest%%:*}
    shadow=${rest#*:}

    if [ ! -f "$ROOT/$vendored" ]; then
        echo "MISSING  $vendored (submodule not checked out?)" >&2
        rc=1
        continue
    fi
    if [ ! -f "$ROOT/$shadow" ]; then
        echo "MISSING  $shadow, the shadow named in the manifest is gone" >&2
        rc=1
        continue
    fi

    got=$(hash_of "$ROOT/$vendored")
    n=$((n + 1))

    if [ "$got" != "$want" ]; then
        rc=1
        echo "" >&2
        echo "CHANGED  $vendored" >&2
        echo "         shadowed by $shadow, which the build compiles instead" >&2
        echo "         reconciled at $want" >&2
        echo "         now          $got" >&2
        echo "         Read the upstream change.  Port it into the shadow or" >&2
        echo "         reject it deliberately, then put the new hash in" >&2
        echo "         tools/check-shadowed-sources.sh in the same commit." >&2
    fi
done

if [ "$rc" -eq 0 ]; then
    echo "$n shadowed vendored files unchanged since reconciliation"
fi

exit "$rc"
