#!/bin/bash
# Build MiniCode's Ubuntu source package and upload it to the Launchpad PPA.
#   scripts/ppa.sh 1.4.12             uploads 1.4.12-1~ppa1~resolute1
#   scripts/ppa.sh 1.4.12 2           a packaging fix for the same release: ~ppa2
#   scripts/ppa.sh --dry-run 1.4.12   builds and lints only, into build/ppa/;
#                                     needs no key, signs and sends nothing
#   scripts/ppa.sh --simulate 1.4.12  everything, signing included, but dput
#                                     only checks (dput -s) and sends nothing
#
# The version given here is the one source of truth, as in release.sh: the
# changelog entry is written from it by dch, never by hand. Launchpad builds
# the binaries from the source package, so only that is made here.
#
# What goes in:
#   - the upstream tarball, minicode_VERSION.orig.tar.gz, is `git archive` of
#     the tag vVERSION minus debian/ (fetched from origin if it is not here
#     yet; release.sh lets gh create the tag on GitHub), compressed by
#     gzip_zlib below. That gives the same bytes every time and on every
#     machine, which matters because Launchpad refuses a second, different
#     tarball under the same name;
#   - debian/ is taken from HEAD as committed, so a packaging fix made after
#     a release can go out as ~ppa2 without a new release.
#
# On the Mac the build runs in an ubuntu:26.04 container
# (packaging/ppa/Dockerfile) that sees only those two files, read-only, and
# builds in its own /build. The signing key never enters the container: the
# unsigned .dsc and .changes are copied out, signed with gpg on the host
# (pinentry asks for the passphrase as usual), and copied back, the .changes
# regenerated in between so it carries the signed .dsc's checksums. dput then
# uploads from the container, after checking the signatures against the
# public key.
#
# Where Docker is not running, on an Ubuntu machine with the same tools
# installed (sudo apt install --no-install-recommends debhelper devscripts
# distro-info dput libdistro-info-perl lintian), the same steps run directly
# in a scratch folder instead, and dput uses the host's own gpg.
#
# A real upload needs gpg and the key id in MINICODE_PPA_KEY or
# ~/.config/minicode/ppa-key-id. The key must be one registered to the
# Launchpad account that owns the PPA (the Mac and the ThinkPad each have
# their own); see packaging/PPA.md for the one-time setup.
#   MINICODE_PPA_SERIES   Ubuntu series to build for (default resolute, 26.04)
#   MINICODE_PPA          upload target (default ppa:echansen/minicode)
set -euo pipefail
cd "$(dirname "$0")/.."

DRY=""
SIMULATE=""
case "${1:-}" in
    --dry-run) DRY=1; shift ;;
    --simulate) SIMULATE=1; shift ;;
esac
VERSION="${1:?usage: scripts/ppa.sh [--dry-run|--simulate] <version> [ppa revision]   e.g. 1.4.12}"
REV="${2:-1}"
SERIES="${MINICODE_PPA_SERIES:-resolute}"
PPA="${MINICODE_PPA:-ppa:echansen/minicode}"
IMAGE="minicode-ppa"
KEYFILE="$HOME/.config/minicode/ppa-key-id"

if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: version must look like 1.2.0, got '$VERSION'" >&2; exit 1
fi
if ! [[ "$REV" =~ ^[1-9][0-9]*$ ]]; then
    echo "error: the PPA revision must be a number from 1, got '$REV'" >&2; exit 1
fi
if ! [[ "$PPA" =~ ^ppa:([^/]+)/([^/]+)$ ]]; then
    echo "error: MINICODE_PPA must look like ppa:owner/name, got '$PPA'" >&2; exit 1
fi
PPA_OWNER="${BASH_REMATCH[1]}"
PPA_NAME="${BASH_REMATCH[2]}"

# The Debian revision is always 1; ~ppaN counts uploads of this release and
# ~SERIES1 keeps one upload per Ubuntu series apart. See packaging/PPA.md.
PKGVER="$VERSION-1~ppa$REV~${SERIES}1"
ORIG="minicode_$VERSION.orig.tar.gz"
DSC="minicode_${PKGVER}.dsc"
CHANGES="minicode_${PKGVER}_source.changes"
# The upstream tarball goes up with the first upload of a release only;
# Launchpad already holds it for ~ppa2 and later.
if [ "$REV" = 1 ]; then SRCOPT=-sa; else SRCOPT=-sd; fi

KEY=""
if [ -z "$DRY" ]; then
    KEY="${MINICODE_PPA_KEY:-}"
    if [ -z "$KEY" ] && [ -s "$KEYFILE" ]; then
        KEY="$(tr -d '[:space:]' < "$KEYFILE")"
    fi
    if [ -z "$KEY" ]; then
        echo "error: no signing key: set MINICODE_PPA_KEY or put the key id in $KEYFILE" >&2
        echo "       (packaging/PPA.md has the one-time setup)" >&2
        exit 1
    fi
    if ! command -v gpg >/dev/null; then
        echo "error: gpg is not installed (brew install gnupg pinentry-mac)" >&2; exit 1
    fi
    if ! gpg --list-secret-keys "$KEY" >/dev/null 2>&1; then
        echo "error: gpg has no secret key $KEY" >&2; exit 1
    fi
    # The changelog's signer line is the key's own name and address.
    SIGNER="$(gpg --with-colons --list-keys "$KEY" | awk -F: '$1 == "uid" { print $10; exit }')"
else
    SIGNER="$(sed -n 's/^Maintainer: //p' debian/control)"
fi
if ! [[ "$SIGNER" =~ ^(.+)\ \<([^>]+)\>$ ]]; then
    echo "error: cannot read a name and address from '$SIGNER'" >&2; exit 1
fi
# Without the key's comment, "(MiniCode releases, ThinkPad)", which says which
# key it is and is no part of the name.
SIGNER_NAME="${BASH_REMATCH[1]% (*)}"
SIGNER_EMAIL="${BASH_REMATCH[2]}"

# In the container when Docker runs, as on the Mac; otherwise here, which
# takes an Ubuntu (or Debian) machine with the container's tools installed.
NATIVE=""
if ! docker info >/dev/null 2>&1; then
    NATIVE=1
    MISSING=""
    for tool in dch dpkg-buildpackage dpkg-genchanges dh lintian dput; do
        command -v "$tool" >/dev/null || MISSING="$MISSING $tool"
    done
    if [ -n "$MISSING" ]; then
        echo "error: Docker is not running (on the Mac: open -a Docker), and building here" >&2
        echo "       needs$MISSING. On Ubuntu: sudo apt install --no-install-recommends" >&2
        echo "       debhelper devscripts distro-info dput libdistro-info-perl lintian" >&2
        exit 1
    fi
    HOST_SERIES="$(. /etc/os-release 2>/dev/null && echo "${VERSION_CODENAME:-}")"
    if [ "$HOST_SERIES" != "$SERIES" ]; then
        echo "warning: building for $SERIES on ${HOST_SERIES:-an unknown release}; the container" >&2
        echo "         (Docker) builds with $SERIES's own tools" >&2
    fi
fi
if ! git cat-file -e HEAD:debian/control 2>/dev/null; then
    echo "error: there is no committed debian/ at HEAD" >&2; exit 1
fi
if ! git diff --quiet HEAD -- debian; then
    echo "warning: debian/ has uncommitted changes, which are left out (HEAD's debian/ is used)" >&2
fi
if ! git rev-parse -q --verify "refs/tags/v$VERSION^{commit}" >/dev/null; then
    echo "==> Fetching the tag v$VERSION"
    git fetch -q origin "refs/tags/v$VERSION:refs/tags/v$VERSION"
fi

# Launchpad never takes the same version twice, even after a deletion. The
# PPA's list is public; when it cannot be read (no PPA yet, no network) the
# upload itself is left to say so.
if [ -z "$DRY" ]; then
    API="https://api.launchpad.net/1.0/~$PPA_OWNER/+archive/ubuntu/$PPA_NAME"
    if curl -fsS "$API?ws.op=getPublishedSources&source_name=minicode&exact_match=true&version=$PKGVER" 2>/dev/null |
           grep -qE "\"source_package_version\": *\"$PKGVER\""; then
        echo "error: $PKGVER is already in $PPA; give a higher PPA revision:" >&2
        echo "       scripts/ppa.sh $VERSION $((REV + 1))" >&2
        exit 1
    fi
fi

TMPBASE="${TMPDIR:-/tmp}"
WORK="$(mktemp -d "${TMPBASE%/}/minicode-ppa.XXXXXX")"
CID=""
cleanup() {
    [ -n "$CID" ] && docker rm -f "$CID" >/dev/null 2>&1
    rm -rf "$WORK"
}
trap cleanup EXIT

# What the Mac's gzip -n -9 wrote for 1.4.12, byte for byte: zlib at level 9
# behind a header with no name and no time. Ubuntu's GNU gzip deflates with
# code of its own, and its different bytes would be refused for any release
# the PPA already holds.
gzip_zlib() {
    python3 -I -c '
import struct, sys, zlib
data = sys.stdin.buffer.read()
z = zlib.compressobj(9, zlib.DEFLATED, -15, 8)
out = sys.stdout.buffer
out.write(b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\x03")
out.write(z.compress(data) + z.flush())
out.write(struct.pack("<II", zlib.crc32(data), len(data) & 0xffffffff))
'
}

echo "==> Making $ORIG from v$VERSION, and debian/ from HEAD"
git archive --format=tar --prefix="minicode-$VERSION/" "v$VERSION" -- . ':(exclude)debian' |
    gzip_zlib > "$WORK/$ORIG"
git archive --format=tar HEAD debian > "$WORK/debian.tar"

# build_run runs a step where the package is built, with the inputs in $IN
# and the work in $B; fetch and put copy a file out of and into $B.
if [ -n "$NATIVE" ]; then
    echo "==> Building the source package $PKGVER here (Docker is not running)"
    IN="$WORK" B="$WORK/build"
    build_run() {
        env VERSION="$VERSION" PKGVER="$PKGVER" SERIES="$SERIES" SRCOPT="$SRCOPT" \
            DEBFULLNAME="$SIGNER_NAME" DEBEMAIL="$SIGNER_EMAIL" IN="$IN" B="$B" \
            bash -euo pipefail -c "$1"
    }
    fetch() { cp "$B/$1" "$2"; }
    put() { cp "$1" "$B/$2"; }
else
    echo "==> Building the source package $PKGVER in an ubuntu:26.04 container"
    docker build -q -t "$IMAGE" packaging/ppa >/dev/null
    CID="$(docker run -d -v "$WORK:/in:ro" -w /build \
            -e VERSION="$VERSION" -e PKGVER="$PKGVER" -e SERIES="$SERIES" -e SRCOPT="$SRCOPT" \
            -e DEBFULLNAME="$SIGNER_NAME" -e DEBEMAIL="$SIGNER_EMAIL" -e IN=/in -e B=/build \
            "$IMAGE" sleep infinity)"
    build_run() { docker exec "$CID" bash -euo pipefail -c "$1"; }
    fetch() { docker cp -q "$CID:/build/$1" "$2"; }
    put() { docker cp -q "$1" "$CID:/build/$2"; }
fi

build_run '
    mkdir -p "$B" && cd "$B"
    cp "$IN/minicode_$VERSION.orig.tar.gz" .
    tar xzf "minicode_$VERSION.orig.tar.gz"
    cd "minicode-$VERSION"
    tar xf "$IN/debian.tar"
    # One entry per upload, written here: the committed changelog is only a
    # placeholder for builds from a checkout.
    rm debian/changelog
    dch --create --package minicode -v "$PKGVER" -D "$SERIES" -u medium \
        "New upstream release $VERSION (tag v$VERSION)."
    dpkg-buildpackage -S -d -us -uc "$SRCOPT" >/dev/null
    # The .buildinfo is left out of the upload: it records the .dsc checksum,
    # which signing the .dsc changes, and Launchpad does not need it.
    rm -f ../*_source.buildinfo
    sed -i "/_source\.buildinfo /d" debian/files
    dpkg-genchanges -S "$SRCOPT" -O"../minicode_${PKGVER}_source.changes" 2>/dev/null
    cd ..
    lintian --fail-on error,warning "minicode_${PKGVER}_source.changes" 2>/dev/null
'

if [ -n "$DRY" ]; then
    mkdir -p build/ppa
    rm -f build/ppa/minicode_*
    for f in "$ORIG" "$DSC" "minicode_${PKGVER}.debian.tar.xz" "$CHANGES"; do
        fetch "$f" build/ppa/
    done
    echo "==> Dry run: the unsigned source package is in build/ppa/; nothing was signed or uploaded."
    ls -1 build/ppa
    exit 0
fi

sign() {
    gpg --local-user "$KEY" --digest-algo SHA512 --yes --clearsign \
        --output "$WORK/$1.asc" "$WORK/$1"
    mv "$WORK/$1.asc" "$WORK/$1"
}

echo "==> Signing with $KEY"
fetch "$DSC" "$WORK/$DSC"
sign "$DSC"
put "$WORK/$DSC" "$DSC"
build_run 'cd "$B/minicode-$VERSION" && dpkg-genchanges -S "$SRCOPT" -O"../minicode_${PKGVER}_source.changes" 2>/dev/null'
fetch "$CHANGES" "$WORK/$CHANGES"
sign "$CHANGES"
put "$WORK/$CHANGES" "$CHANGES"

# dput checks the signatures with gpg where it runs: the container needs the
# public key; here, the host's gpg has it already.
[ -n "$NATIVE" ] || gpg --export "$KEY" | docker exec -i "$CID" gpg --quiet --import
if [ -n "$SIMULATE" ]; then
    echo "==> Checking the upload to $PPA (dput -s: nothing is sent)"
    build_run "cd \"\$B\" && dput -s '$PPA' \"minicode_\${PKGVER}_source.changes\""
    echo "==> Simulated: $PKGVER is signed and passes dput's checks; nothing was uploaded."
    exit 0
fi
echo "==> Uploading to $PPA"
build_run "cd \"\$B\" && dput '$PPA' \"minicode_\${PKGVER}_source.changes\""

echo "==> Uploaded $PKGVER. Launchpad mails $SIGNER_EMAIL when it is accepted and"
echo "    built; progress at https://launchpad.net/~$PPA_OWNER/+archive/ubuntu/$PPA_NAME/+packages"
