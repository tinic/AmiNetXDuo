#!/usr/bin/env bash
# Regenerate tests/x509/x509_pss_vectors.h, the RSASSA-PSS certificates the
# PSS section of test_tls_x509.c verifies.
# SPDX-License-Identifier: MIT
set -euo pipefail

OUT="${1:?usage: mkpssvectors.sh <output-header>}"
OUT=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
cd "$D"

DAYS=7300

for k in root int leaf; do openssl genrsa -out "$k.key" 2048 2>/dev/null; done

openssl req -new -x509 -key root.key -out root.pem -days $DAYS \
    -subj "/CN=AmiNetXDuo PSS Root" -sha256 \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null

# sign <name> <issuer-base> <key-base> <subject> <digest> <saltlen>
sign() {
    openssl req -new -key "$3.key" -out "$1.csr" -subj "$4" 2>/dev/null
    openssl x509 -req -in "$1.csr" -CA "$2.pem" -CAkey "$2.key" \
        -set_serial "0x$(openssl rand -hex 8)" -days $DAYS "-$5" \
        -sigopt rsa_padding_mode:pss -sigopt "rsa_pss_saltlen:$6" \
        -extfile "$1.cnf" -out "$1.pem" 2>/dev/null
}

cat > int.cnf <<'EOF'
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
EOF
sign int root int "/CN=AmiNetXDuo PSS Intermediate" sha256 digest

mkleaf() {  # mkleaf <name> <issuer> <dns> <digest> <saltlen>
    cat > "$1.cnf" <<EOF
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:$3
EOF
    sign "$1" "$2" leaf "/CN=$3" "$4" "$5"
}

mkleaf leaf      int  pss.test      sha256 digest
mkleaf leaf384   root pss384.test   sha384 digest
mkleaf leaf_salt root pss-salt.test sha256 max

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
 * Fixed inputs for the RSASSA-PSS section of tests/x509/test_tls_x509.c.
 * Generated once with OpenSSL and checked in, for the reason
 * x509_test_vectors.h gives.
 *
 *   x509_pss_root       RSA-2048 CA, self-signed PKCS#1 v1.5 SHA-256.  The
 *                       chain walk never checks a root's signature on itself,
 *                       so this one is the anchor and not a PSS case.
 *
 *   x509_pss_int        CA under that root, signed RSASSA-PSS SHA-256.
 *
 *   x509_pss_leaf       pss.test under the intermediate, RSASSA-PSS SHA-256
 *                       with a 32-byte salt.  The two-certificate chain a
 *                       PSS-issuing CA actually presents.
 *
 *   x509_pss_leaf384    pss384.test directly under the root, RSASSA-PSS
 *                       SHA-384.  The digest has to come out of the
 *                       parameters: nothing else in the certificate says it.
 *
 *   x509_pss_leaf_salt  pss-salt.test under the root, RSASSA-PSS SHA-256 with
 *                       the salt at the maximum the modulus allows rather
 *                       than at the digest length.  RFC 4055 permits it and
 *                       a verifier that assumes sLen == hLen refuses it.
 *
 * Regenerate with tests/x509/mkpssvectors.sh.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_X509_PSS_VECTORS_H
#define AMINETXDUO_X509_PSS_VECTORS_H

EOF
emit x509_pss_root      root
emit x509_pss_int       int
emit x509_pss_leaf      leaf
emit x509_pss_leaf384   leaf384
emit x509_pss_leaf_salt leaf_salt
printf '#endif /* AMINETXDUO_X509_PSS_VECTORS_H */\n'
} > "$OUT"

echo "wrote $OUT"
