#!/usr/bin/env bash
#
# Regenerate tests/x509/x509_ext_vectors.h, the certificates the
# extension-policy section of test_tls_x509.c verifies.
#
#   tests/x509/mkextvectors.sh tests/x509/x509_ext_vectors.h
#
# Run by hand, not by the build.  The header is checked in because a test that
# mints its own certificates proves whatever the generator did that run, and
# because these are inputs to a security check: they should change when
# somebody means them to.
#
# SPDX-License-Identifier: MIT
set -euo pipefail

OUT="${1:?usage: mkextvectors.sh <output-header>}"
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
cd "$D"

DAYS=7300

key() { openssl genrsa -out "$1.key" 2048 2>/dev/null; }

for k in root int leaf; do key "$k"; done

# ---- root -----------------------------------------------------------------
openssl req -new -x509 -key root.key -out root.pem -days $DAYS \
    -subj "/CN=AmiNetXDuo Ext Root" -sha256 \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"

sign() {   # sign <name> <issuer-base> <extfile> <subject>
    openssl req -new -key "$3.key" -out "$1.csr" -subj "$4"
    openssl x509 -req -in "$1.csr" -CA "$2.pem" -CAkey "$2.key" \
        -set_serial "0x$(openssl rand -hex 8)" -days $DAYS -sha256 \
        -extfile "$1.cnf" -out "$1.pem"
}

# ---- leaves directly under the root ---------------------------------------
cat > leaf_ok.cnf <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:ok.test
EOF
sign leaf_ok root leaf "/CN=ok.test"

cat > leaf_clientauth.cnf <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth,emailProtection
subjectAltName=DNS:ok.test
EOF
sign leaf_clientauth root leaf "/CN=ok.test"

# An extension nothing in nx_secure acts on, marked critical.  inhibitAnyPolicy
# is a real one with a real OID (2.5.29.54) that the OID table already knows,
# so this tests the critical sweep and not the unknown-OID path.
cat > leaf_critical.cnf <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:ok.test
inhibitAnyPolicy=critical,0
EOF
sign leaf_critical root leaf "/CN=ok.test"

# ---- a constrained intermediate, shaped like a public one -----------------
# Critical extendedKeyUsage is what Let's Encrypt puts on its intermediates,
# so a verifier that rejects unknown criticals has to accept this one.
cat > int.cnf <<'EOF'
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
extendedKeyUsage=critical,serverAuth,clientAuth
nameConstraints=critical,permitted;DNS:permitted.test,excluded;DNS:bad.permitted.test
EOF
sign int root int "/CN=AmiNetXDuo Ext Intermediate"

mkleaf() {  # mkleaf <name> <dns>
    cat > "$1.cnf" <<EOF
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:$2
EOF
    sign "$1" int leaf "/CN=$2"
}

mkleaf leaf_inside   host.permitted.test
mkleaf leaf_outside  evil.test
mkleaf leaf_excluded host.bad.permitted.test

# ---------------------------------------------------------------------------
emit() {    # emit <c-name> <pem>
    local name=$1 pem=$2
    openssl x509 -in "$pem.pem" -outform DER -out "$pem.der"
    local n
    n=$(wc -c < "$pem.der" | tr -d ' ')
    printf 'static const unsigned char %s[] = {\n' "$name"
    od -An -v -tx1 "$pem.der" | tr -s ' ' | sed 's/^ //' |
        awk '{ printf "   "; for (i = 1; i <= NF; i++) printf " 0x%s,", $i; printf "\n" }'
    printf '};\nstatic const unsigned %s_len = %s;\n\n' "$name" "$n"
}

{
cat <<'EOF'
/*
 * Fixed inputs for the extension-policy section of tests/x509/test_tls_x509.c.
 * Generated once with OpenSSL and checked in, for the reason
 * x509_test_vectors.h gives: a test that generates its own key material proves
 * whatever the generator did that run.
 *
 * One root, one intermediate under it, and six leaves.  Every leaf is RSA-2048
 * with the same key, so the only thing that differs between them is the
 * extensions, which is what is under test.
 *
 *   x509_ext_root            CA:TRUE critical, keyCertSign critical.  The
 *                            trust anchor for everything below.
 *
 *   x509_ext_leaf_ok         ok.test, extendedKeyUsage serverAuth.  The
 *                            ordinary certificate, and the guard: a
 *                            strictness change that rejects this one is a
 *                            defect, not a hardening.
 *
 *   x509_ext_leaf_clientauth ok.test, extendedKeyUsage clientAuth and
 *                            emailProtection.  A certificate its issuer said
 *                            is not for TLS servers.
 *
 *   x509_ext_leaf_critical   ok.test, plus inhibitAnyPolicy marked CRITICAL.
 *                            A real extension with a real OID that nothing in
 *                            nx_secure acts on.
 *
 *   x509_ext_int             CA:TRUE pathlen 0, with extendedKeyUsage marked
 *                            critical -- the shape Let's Encrypt ships -- and
 *                            nameConstraints permitting permitted.test and
 *                            excluding bad.permitted.test.
 *
 *   x509_ext_leaf_inside     host.permitted.test under that intermediate.
 *   x509_ext_leaf_outside    evil.test under it.
 *   x509_ext_leaf_excluded   host.bad.permitted.test under it.
 *
 * Regenerate with tests/x509/mkextvectors.sh.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_X509_EXT_VECTORS_H
#define AMINETXDUO_X509_EXT_VECTORS_H

EOF
emit x509_ext_root            root
emit x509_ext_leaf_ok         leaf_ok
emit x509_ext_leaf_clientauth leaf_clientauth
emit x509_ext_leaf_critical   leaf_critical
emit x509_ext_int             int
emit x509_ext_leaf_inside     leaf_inside
emit x509_ext_leaf_outside    leaf_outside
emit x509_ext_leaf_excluded   leaf_excluded
printf '#endif /* AMINETXDUO_X509_EXT_VECTORS_H */\n'
} > "$OUT"

echo "wrote $OUT"
