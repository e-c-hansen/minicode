#!/bin/sh
# make-signing-identity.sh: creates "MiniCode Local Signing", a self-signed
# code signing certificate, in the login keychain, once. `make` signs
# MiniCode.app with it when it is there, and with an ad-hoc signature
# otherwise.
#
# Why: macOS remembers what an app may do (a folder in Documents, the
# microphone a program in the terminal asks for) by the app's signature. An
# ad-hoc signature names the exact binary, so every new build, a restart into
# one, and every release counted as a new app that had to ask again. A
# certificate names the signer instead, so the answers stay. macOS does not
# have to trust the certificate for that, and nothing here asks for a
# password.
#
# The private key lives only in the login keychain. Back it up (export it
# from Keychain Access) if you sign releases with it: a new key means every
# Mac asks once more. To remove it:
#   security delete-identity -c "MiniCode Local Signing"
set -eu

NAME="MiniCode Local Signing"
KEYCHAIN="$HOME/Library/Keychains/login.keychain-db"

if security find-certificate -c "$NAME" "$KEYCHAIN" >/dev/null 2>&1; then
    echo "\"$NAME\" is already in the login keychain."
    exit 0
fi

DIR=$(mktemp -d "${TMPDIR:-/tmp}/minicode-sign.XXXXXX")
trap 'rm -rf "$DIR"' EXIT
umask 077
cat > "$DIR/cert.cnf" <<CNF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = $NAME
[ext]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
CNF
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 7300 \
    -keyout "$DIR/key.pem" -out "$DIR/cert.pem" -config "$DIR/cert.cnf" 2>/dev/null
PASS=$(/usr/bin/openssl rand -hex 16)
/usr/bin/openssl pkcs12 -export -inkey "$DIR/key.pem" -in "$DIR/cert.pem" \
    -name "$NAME" -out "$DIR/id.p12" -passout "pass:$PASS" 2>/dev/null
# -T lets codesign use the key without asking each time.
security import "$DIR/id.p12" -k "$KEYCHAIN" -P "$PASS" -T /usr/bin/codesign >/dev/null
echo "Added \"$NAME\" to the login keychain. Run make to sign with it."
