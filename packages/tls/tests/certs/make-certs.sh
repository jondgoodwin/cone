#!/bin/sh
# Makes the tls package's test certificates, once; they are committed, and
# this is how they were made (README.md beside it). Run from this folder with
# any OpenSSL 3.4 or later on PATH (Git for Windows' own, 3.5.7, made these):
#
#   sh make-certs.sh
#
# Every key is ECDSA P-256. Nothing is installed anywhere: the files are all
# there is. The good certificates are valid from 1 Jan 2026 to 31 Dec 2125.
set -e
# Git for Windows' shell would rewrite "/O=..." as a path
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"

days="-not_before 20260101000000Z -not_after 21251231235959Z"

key() { openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$1"; }

cat > ca.ext <<EOF
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
EOF
cat > leaf.ext <<EOF
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1
EOF

# The test root, the only trust anchor the tests give a client
key ca.key
openssl req -x509 -new -key ca.key -subj "/O=Cone tests/CN=Cone Test Root CA" $days \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -out ca.pem

# An intermediate under it, which the server sends after its own certificate
key intermediate.key
openssl req -new -key intermediate.key -subj "/O=Cone tests/CN=Cone Test Intermediate CA" \
  -out intermediate.csr
openssl x509 -req -in intermediate.csr -CA ca.pem -CAkey ca.key -set_serial 2 $days \
  -extfile ca.ext -out intermediate.pem

# The server: localhost and 127.0.0.1. server.pem is its chain, leaf first
key server.key
openssl req -new -key server.key -subj "/O=Cone tests/CN=localhost" -out server.csr
openssl x509 -req -in server.csr -CA intermediate.pem -CAkey intermediate.key \
  -set_serial 3 $days -extfile leaf.ext -out server-leaf.pem
cat server-leaf.pem intermediate.pem > server.pem

# The same names, expired: valid for one day in January 2020
key expired.key
openssl req -new -key expired.key -subj "/O=Cone tests/CN=localhost" -out expired.csr
openssl x509 -req -in expired.csr -CA intermediate.pem -CAkey intermediate.key \
  -set_serial 4 -not_before 20200101000000Z -not_after 20200102000000Z \
  -extfile leaf.ext -out expired-leaf.pem
cat expired-leaf.pem intermediate.pem > expired.pem

# The same names under a root the client is not given
key other-ca.key
openssl req -x509 -new -key other-ca.key -subj "/O=Cone tests/CN=Cone Test Other Root" $days \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -out other-ca.pem
key untrusted.key
openssl req -new -key untrusted.key -subj "/O=Cone tests/CN=localhost" -out untrusted.csr
openssl x509 -req -in untrusted.csr -CA other-ca.pem -CAkey other-ca.key \
  -set_serial 5 $days -extfile leaf.ext -out untrusted.pem

# The server's pin: the SHA-256 of its public key (SubjectPublicKeyInfo), hex
openssl pkey -in server.key -pubout -outform DER | openssl dgst -sha256 -r | cut -d' ' -f1 > server.pin

rm -f ./*.csr ./*.ext ca.key intermediate.key other-ca.key server-leaf.pem expired-leaf.pem
