#!/bin/bash
# Check that a built MiniCode.app runs on the macOS it says it does.
#   scripts/check-minos.sh [path/to/MiniCode.app]
#
# Info.plist's LSMinimumSystemVersion is the one place the minimum is written:
# the Makefile builds with -mmacosx-version-min from it. This checks the
# result: the minos in each binary's LC_BUILD_VERSION (every architecture),
# the bundle's own Info.plist, and the cask's depends_on line. A binary built
# without the flag has the build machine's macOS as its minos and refuses to
# launch on anything older, which is how 1.5.6 shipped. release.sh and the
# macOS CI job run this after building.
set -euo pipefail
cd "$(dirname "$0")/.."

APP="${1:-MiniCode.app}"
BUDDY=/usr/libexec/PlistBuddy
CASK=packaging/minicode.rb

# 11.0 and 11 are the same version.
norm() { echo "$1" | sed -E 's/(\.0)+$//'; }

WANT="$("$BUDDY" -c "Print :LSMinimumSystemVersion" Info.plist)"
fail=0

BUNDLED="$("$BUDDY" -c "Print :LSMinimumSystemVersion" "$APP/Contents/Info.plist")"
if [ "$(norm "$BUNDLED")" != "$(norm "$WANT")" ]; then
    echo "error: $APP/Contents/Info.plist says macOS $BUNDLED, Info.plist $WANT" >&2
    fail=1
fi

for bin in "$APP/Contents/MacOS/MiniCode" "$APP/Contents/MacOS/minicode-cli"; do
    if [ ! -f "$bin" ]; then
        echo "error: $bin is missing" >&2; fail=1; continue
    fi
    found="$(otool -l "$bin" | awk '/cmd LC_BUILD_VERSION/ {b = 1} b && $1 == "minos" {print $2; b = 0}')"
    if [ -z "$found" ]; then
        echo "error: $bin has no LC_BUILD_VERSION" >&2; fail=1; continue
    fi
    for v in $found; do
        if [ "$(norm "$v")" != "$(norm "$WANT")" ]; then
            echo "error: $bin is built for macOS $v and up, Info.plist says $WANT" >&2
            echo "       (the Makefile's -mmacosx-version-min; run make clean, then make)" >&2
            fail=1
        fi
    done
done

# Homebrew 7 knows no macOS older than Big Sur, so for 11 brew style wants a
# plain `depends_on :macos`; for a later minimum, `depends_on macos: :name`.
case "${WANT%%.*}" in
    11) LINE="depends_on :macos" ;;
    12) LINE="depends_on macos: :monterey" ;;
    13) LINE="depends_on macos: :ventura" ;;
    14) LINE="depends_on macos: :sonoma" ;;
    15) LINE="depends_on macos: :sequoia" ;;
    26) LINE="depends_on macos: :tahoe" ;;
    *)  LINE="" ;;
esac
if [ -z "$LINE" ]; then
    echo "error: no Homebrew name known for macOS $WANT; add it to $0" >&2; fail=1
elif ! grep -qx "  $LINE" "$CASK"; then
    echo "error: $CASK should say \`$LINE\` for macOS $WANT" >&2; fail=1
fi

[ "$fail" = 0 ] || exit 1
echo "macOS $WANT and up: the binaries, the bundle's Info.plist and the cask agree"
