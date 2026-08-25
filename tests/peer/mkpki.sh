#!/usr/bin/env bash
# The test PKI the hermetic TLS half of the curl verification suite runs on.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT="${1:-$ROOT/build/peer-pki}"

STAMP="$OUT/.stamp"
if [ -f "$STAMP" ] && [ "$STAMP" -nt "${BASH_SOURCE[0]}" ]; then
    echo "==> test PKI already in $OUT (delete $STAMP to regenerate)"
    exit 0
fi

command -v openssl >/dev/null || { echo "no openssl on PATH" >&2; exit 2; }

rm -rf "$OUT"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
STAMP="$OUT/.stamp"
cd "$OUT"

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

newkey()
{
    if [ "$2" = "ec" ]; then
        openssl ecparam -name prime256v1 -genkey -noout -out "$1.key.pem" 2>/dev/null
    else
        openssl genrsa -out "$1.key.pem" 2048 2>/dev/null
    fi
}

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

issue_pss()
{
    newkey "$1" rsa
    ext_leaf "$2"
    openssl req -new -key "$1.key.pem" -subj "/CN=$2" -out "$1.csr" 2>/dev/null
    openssl x509 -req -in "$1.csr" -CA "$3.cert.pem" -CAkey "$3.key.pem" \
            -CAcreateserial -sha256 \
            -sigopt rsa_padding_mode:pss -sigopt rsa_pss_saltlen:digest \
            -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
            -extfile "$OUT/ext.cnf" -out "$1.cert.pem" 2>/dev/null
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
issue_pss leaf-pss2 pss2.test int-rsa-1
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
cat leaf-pss2.cert.pem    int-rsa-1.cert.pem                                    > leaf-pss2.chain.pem
cp  leaf-selfsigned.cert.pem                                                      leaf-selfsigned.chain.pem

echo "==> trust stores"
cat root-rsa.cert.pem root-ec.cert.pem > testroots.pem
python3 "$ROOT/tools/mkcertstore.py" --output teststore  testroots.pem
python3 "$ROOT/tools/mkcertstore.py" --output otherstore root-other.cert.pem

rm -f "$OUT/ext.cnf" "$OUT"/*.srl
date > "$STAMP"

echo
echo "==> test PKI in $OUT"
for f in leaf-rsa2 leaf-rsa3 leaf-rsa4 leaf-ec2 leaf-ec3 leaf-expired leaf-pss2 leaf-selfsigned; do
    printf '    %-16s %d certificate(s) sent\n' "$f" \
        "$(grep -c 'BEGIN CERTIFICATE' "$f.chain.pem")"
done
ls -l teststore otherstore | sed 's/^/    /'
