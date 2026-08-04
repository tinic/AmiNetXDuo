#!/usr/bin/env bash
#
# The test PKI the hermetic TLS half of the curl verification suite runs on.
#
#   tests/peer/mkpki.sh [OUTDIR]        (default build/peer-pki)
#
# WHY A LOCAL PKI AND NOT badssl.com
#
#   Because a suite that needs the internet is not a baseline.  Every TLS
#   property the brief asks for, chain depth 2, 3 and 4, RSA and ECDSA,
#   expired, self-signed, wrong host, --cacert, is a property of the
#   certificates, and certificates are free.  badssl is still worth pointing at
#   occasionally against a real server, but it is a
#   different kind of test: it tells you the internet still works, not that the
#   stack does.
#
# WHAT IS GENERATED
#
#   root-rsa          RSA-2048 CA, self-signed            , in the trust store
#     int-rsa-1       RSA-2048 CA, signed by root-rsa
#       leaf rsa2.test                                     chain: leaf,int1
#       leaf expired.test        notAfter in 2020          chain: leaf,int1
#       int-rsa-2     signed by int-rsa-1
#         leaf rsa3.test                                   chain: leaf,int2,int1
#         int-rsa-3   signed by int-rsa-2
#           leaf rsa4.test                                 chain: leaf,int3,int2,int1
#   root-ec           P-256 CA, self-signed               , in the trust store
#     int-ec-1
#       leaf ec2.test                                      chain: leaf,int1
#       int-ec-2
#         leaf ec3.test                                    chain: leaf,int2,int1
#   root-other        RSA-2048 CA, self-signed            , NOT in the store,
#                                                             for --cacert tests
#   selfsigned.test   its own issuer, in nobody's store
#
#   teststore         tools/mkcertstore.py of root-rsa + root-ec, which is what
#                     --cacert names on the Amiga side
#   otherstore        tools/mkcertstore.py of root-other alone: a --cacert that
#                     is a valid store and the wrong one
#
#   Every leaf carries the name in BOTH the subject CN and a SAN dNSName.
#   nx_secure checks the SAN when there is one, and a certificate with only a
#   CN is a 2010 artefact that would test the wrong path.
#
#   The depth counts are the number of certificates the SERVER SENDS, which is
#   what curl reports and what tls.library walks.  The root is never sent.
#
# All of it is regenerated only when OUTDIR/.stamp is missing, because a
# handshake measurement wants the same keys from run to run.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT="${1:-$ROOT/build/peer-pki}"

STAMP="$OUT/.stamp"
if [ -f "$STAMP" ]; then
    echo "==> test PKI already in $OUT (delete $STAMP to regenerate)"
    exit 0
fi

command -v openssl >/dev/null || { echo "no openssl on PATH" >&2; exit 2; }

rm -rf "$OUT"
mkdir -p "$OUT"
# Absolute from here on: the script cd's into $OUT and then keeps writing to
# "$OUT/ext.cnf", so a relative argument stops resolving after the cd and the
# run dies one file in.  Callers pass an absolute path, which is
# why it never showed.
OUT=$(cd "$OUT" && pwd)
STAMP="$OUT/.stamp"
cd "$OUT"

# FIXED DATES, NOT -days, AND THE REASON IS THE GUEST'S CLOCK.
#
#   AmigaOS DateStamp() is local time and has no timezone concept at all, so
#   an emulated machine synchronised to this host reads whatever the host's
#   wall clock says, and tls.library compares that against a certificate's
#   notBefore, which is UTC.  West of Greenwich the Amiga is therefore hours
#   BEHIND every certificate issued today, and `-days 7300` (notBefore = now,
#   in UTC) produced a whole PKI that the guest rejected as "not yet valid".
#   Measured, not guessed: host 04:09 UTC, guest 21:09, every leaf refused.
#
#   So both ends are pinned.  notBefore is after tls.library's clock floor
#   (2026-01-01, src/tls_time.c) so the validity check still RUNS, pinning it
#   to 1990 would make the library treat the clock as unset and skip the very
#   check the expired-certificate case exists to prove.
NOT_BEFORE=20260201000000Z
NOT_AFTER=20450101000000Z

ext_ca()
{
    cat > "$OUT/ext.cnf" <<EOF
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
EOF
}

ext_leaf()
{
    cat > "$OUT/ext.cnf" <<EOF
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:$1
subjectKeyIdentifier=hash
EOF
}

# $1 name  $2 alg (rsa|ec)
newkey()
{
    if [ "$2" = "ec" ]; then
        openssl ecparam -name prime256v1 -genkey -noout -out "$1.key.pem" 2>/dev/null
    else
        openssl genrsa -out "$1.key.pem" 2048 2>/dev/null
    fi
}

# $1 name  $2 alg  $3 CN
selfsigned_ca()
{
    newkey "$1" "$2"
    ext_ca
    openssl req -new -x509 -key "$1.key.pem" -sha256 \
            -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
            -subj "/CN=$3" -extensions v3 -config <(
                printf '[req]\ndistinguished_name=dn\n[dn]\n[v3]\n'
                cat "$OUT/ext.cnf") \
            -out "$1.cert.pem" 2>/dev/null
}

# $1 name  $2 alg  $3 CN  $4 issuer-name  $5 ext-kind (ca|leaf)  [$6 notAfter]
issue()
{
    newkey "$1" "$2"
    if [ "$5" = "ca" ]; then ext_ca; else ext_leaf "$3"; fi
    openssl req -new -key "$1.key.pem" -subj "/CN=$3" -out "$1.csr" 2>/dev/null

    if [ -n "${6:-}" ]; then
        openssl x509 -req -in "$1.csr" -CA "$4.cert.pem" -CAkey "$4.key.pem" \
                -CAcreateserial -sha256 \
                -not_before "20200101000000Z" -not_after "$6" \
                -extfile "$OUT/ext.cnf" -out "$1.cert.pem" 2>/dev/null
    else
        openssl x509 -req -in "$1.csr" -CA "$4.cert.pem" -CAkey "$4.key.pem" \
                -CAcreateserial -sha256 \
                -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
                -extfile "$OUT/ext.cnf" -out "$1.cert.pem" 2>/dev/null
    fi
    rm -f "$1.csr"
}

echo "==> roots"
selfsigned_ca root-rsa   rsa "AmiNetXDuo Test Root RSA"
selfsigned_ca root-ec    ec  "AmiNetXDuo Test Root EC"
selfsigned_ca root-other rsa "AmiNetXDuo Test Root Nobody Trusts"

echo "==> intermediates"
issue int-rsa-1 rsa "AmiNetXDuo Test Intermediate RSA 1" root-rsa   ca
issue int-rsa-2 rsa "AmiNetXDuo Test Intermediate RSA 2" int-rsa-1  ca
issue int-rsa-3 rsa "AmiNetXDuo Test Intermediate RSA 3" int-rsa-2  ca
issue int-ec-1  ec  "AmiNetXDuo Test Intermediate EC 1"  root-ec    ca
issue int-ec-2  ec  "AmiNetXDuo Test Intermediate EC 2"  int-ec-1   ca

echo "==> leaves"
issue leaf-rsa2    rsa rsa2.test    int-rsa-1 leaf
issue leaf-rsa3    rsa rsa3.test    int-rsa-2 leaf
issue leaf-rsa4    rsa rsa4.test    int-rsa-3 leaf
issue leaf-ec2     ec  ec2.test     int-ec-1  leaf
issue leaf-ec3     ec  ec3.test     int-ec-2  leaf
issue leaf-expired rsa expired.test int-rsa-1 leaf "20210101000000Z"
selfsigned_leaf()
{
    newkey leaf-selfsigned rsa
    ext_leaf selfsigned.test
    openssl req -new -x509 -key leaf-selfsigned.key.pem -sha256 \
            -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
            -subj "/CN=selfsigned.test" -extensions v3 -config <(
                printf '[req]\ndistinguished_name=dn\n[dn]\n[v3]\n'
                cat "$OUT/ext.cnf") \
            -out leaf-selfsigned.cert.pem 2>/dev/null
}
selfsigned_leaf

echo "==> chains (leaf first, root never sent)"
cat leaf-rsa2.cert.pem    int-rsa-1.cert.pem                                    > leaf-rsa2.chain.pem
cat leaf-rsa3.cert.pem    int-rsa-2.cert.pem int-rsa-1.cert.pem                 > leaf-rsa3.chain.pem
cat leaf-rsa4.cert.pem    int-rsa-3.cert.pem int-rsa-2.cert.pem int-rsa-1.cert.pem > leaf-rsa4.chain.pem
cat leaf-ec2.cert.pem     int-ec-1.cert.pem                                     > leaf-ec2.chain.pem
cat leaf-ec3.cert.pem     int-ec-2.cert.pem int-ec-1.cert.pem                   > leaf-ec3.chain.pem
cat leaf-expired.cert.pem int-rsa-1.cert.pem                                    > leaf-expired.chain.pem
cp  leaf-selfsigned.cert.pem                                                      leaf-selfsigned.chain.pem

echo "==> trust stores"
cat root-rsa.cert.pem root-ec.cert.pem > testroots.pem
python3 "$ROOT/tools/mkcertstore.py" --output teststore  testroots.pem
python3 "$ROOT/tools/mkcertstore.py" --output otherstore root-other.cert.pem

rm -f "$OUT/ext.cnf" "$OUT"/*.srl
date > "$STAMP"

echo
echo "==> test PKI in $OUT"
for f in leaf-rsa2 leaf-rsa3 leaf-rsa4 leaf-ec2 leaf-ec3 leaf-expired leaf-selfsigned; do
    printf '    %-16s %d certificate(s) sent\n' "$f" \
        "$(grep -c 'BEGIN CERTIFICATE' "$f.chain.pem")"
done
ls -l teststore otherstore | sed 's/^/    /'
