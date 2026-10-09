#!/bin/bash
# Cut a MiniCode release and update the Homebrew tap in one shot.
#   scripts/release.sh 1.2.0
#   scripts/release.sh --linux 1.2.1   a Linux-only release: the PPA alone
#
# The version given here is the one source of truth. The script:
#   1. checks it is releasing a clean, tested main,
#   2. stamps the version and checksum into Info.plist and packaging/minicode.rb,
#      commits that, and pushes main,
#   3. builds the zip and checks the built app reports that same version,
#   4. builds the signed Android APK for the same version,
#   5. publishes a GitHub Release here, tagged vVERSION, with the zip and the
#      APK attached (Obtainium and people installing by hand take it from there),
#   6. copies packaging/minicode.rb into the tap as the live cask,
#   7. uploads the Ubuntu source package to the Launchpad PPA through
#      scripts/ppa.sh, when a signing key is configured (packaging/PPA.md),
#      and says it skipped that step when none is.
#
# The zip ships from this repo's own Releases, next to the tag it was built
# from; the tap holds nothing but the cask. packaging/minicode.rb is the one
# copy of the cask that is edited: the tap's copy is written from it, so the two
# cannot drift.
# Requires the gh CLI, authenticated with push access to both repos, and for
# the APK the Android SDK, JDK 21, and the release key: the keystore at
# ~/.config/minicode/release.keystore with its password in the Keychain
# (service minicode-android-keystore). Every Android update must be signed with
# that one key, so keep a backup of it. MINICODE_NO_APK=1 releases without it.
# The PPA upload needs gpg, the key id in MINICODE_PPA_KEY or
# ~/.config/minicode/ppa-key-id, and Docker or the packaging tools installed
# (scripts/ppa.sh); MINICODE_NO_PPA=1 releases without it. It runs last, so a
# failed upload leaves the GitHub release and the cask in place, and
# scripts/ppa.sh VERSION retries it alone.
#
# --linux is for a change that only Ubuntu users would notice, and it runs on
# the ThinkPad as well as the Mac: it tests, stamps the version into Info.plist
# (where the Linux build reads it), commits, pushes main with the tag vVERSION,
# and uploads to the PPA. It makes no GitHub Release, zip, APK or cask, so
# Homebrew and Obtainium stay on the last full release (Obtainium would find
# no APK in a release without one). The next full release takes a higher
# version and carries the change to the Mac and Android.
set -euo pipefail
cd "$(dirname "$0")/.."

LINUX_ONLY=""
if [ "${1:-}" = "--linux" ]; then LINUX_ONLY=1; shift; fi
VERSION="${1:?usage: scripts/release.sh [--linux] <version>   e.g. 1.2.0}"
TAP_REPO="e-c-hansen/homebrew-tap"
APP_REPO="e-c-hansen/minicode"
CASK="packaging/minicode.rb"
PLIST="Info.plist"
BUDDY=/usr/libexec/PlistBuddy

if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: version must look like 1.2.0, got '$VERSION'" >&2; exit 1
fi
if [ "$(git rev-parse --abbrev-ref HEAD)" != "main" ]; then
    echo "error: release from main (on $(git rev-parse --abbrev-ref HEAD))" >&2; exit 1
fi
if [ -n "$(git status --porcelain)" ]; then
    echo "error: the working tree has uncommitted changes" >&2; exit 1
fi
if gh release view "v$VERSION" --repo "$APP_REPO" >/dev/null 2>&1; then
    echo "error: v$VERSION is already released on $APP_REPO" >&2; exit 1
fi
# A Linux-only release leaves a tag and no GitHub Release, and gh would hang a
# full release on that old commit; each takes a version of its own.
if git rev-parse -q --verify "refs/tags/v$VERSION" >/dev/null ||
   git ls-remote --exit-code --tags origin "refs/tags/v$VERSION" >/dev/null 2>&1; then
    echo "error: the tag v$VERSION already exists; give the next version" >&2; exit 1
fi

# The PPA step runs only with a key configured. With one, check now that it
# can run, rather than finding out after main is pushed.
PPA_KEY=""
if [ -z "${MINICODE_NO_PPA:-}" ]; then
    PPA_KEY="${MINICODE_PPA_KEY:-}"
    if [ -z "$PPA_KEY" ] && [ -s "$HOME/.config/minicode/ppa-key-id" ]; then
        PPA_KEY="$(tr -d '[:space:]' < "$HOME/.config/minicode/ppa-key-id")"
    fi
fi
if [ -n "$PPA_KEY" ]; then
    if ! command -v gpg >/dev/null || ! gpg --list-secret-keys "$PPA_KEY" >/dev/null 2>&1; then
        echo "error: no gpg secret key $PPA_KEY for the PPA (packaging/PPA.md), or set MINICODE_NO_PPA=1" >&2
        exit 1
    fi
    if ! docker info >/dev/null 2>&1 &&
       ! { command -v dch && command -v dh && command -v lintian && command -v dput; } >/dev/null; then
        echo "error: the PPA upload needs Docker running (open -a Docker), or here the packaging" >&2
        echo "       tools (scripts/ppa.sh says which), or set MINICODE_NO_PPA=1" >&2
        exit 1
    fi
fi

if [ -n "$LINUX_ONLY" ]; then
    if [ -z "$PPA_KEY" ]; then
        echo "error: a Linux-only release is the PPA upload, and there is no signing key" >&2
        echo "       (MINICODE_PPA_KEY or ~/.config/minicode/ppa-key-id; packaging/PPA.md)" >&2
        exit 1
    fi

    echo "==> Running the core tests"
    make test

    # PlistBuddy is the Mac's; this edit works on both.
    echo "==> Stamping version $VERSION into $PLIST"
    perl -0pi -e "s{(<key>CFBundle(?:ShortVersionString|Version)</key>\s*<string>)[^<]*}{\${1}$VERSION}g" "$PLIST"
    git add "$PLIST"
    git diff --cached --quiet || git commit -q -m "Release $VERSION (Linux)"

    echo "==> Pushing main and the tag v$VERSION"
    git tag "v$VERSION"
    git push -q origin main "refs/tags/v$VERSION"

    echo "==> Uploading $VERSION to the Ubuntu PPA"
    if ! MINICODE_PPA_KEY="$PPA_KEY" scripts/ppa.sh "$VERSION"; then
        echo "error: the PPA upload failed; main and the tag are pushed. Retry it with" >&2
        echo "       scripts/ppa.sh $VERSION" >&2
        exit 1
    fi
    echo "==> Done. Ubuntu users get $VERSION once Launchpad has built it (about half an hour)."
    echo "    Homebrew, GitHub Releases and Obtainium stay on the last full release."
    exit 0
fi

APK=""
if [ -z "${MINICODE_NO_APK:-}" ]; then
    KEYSTORE="$HOME/.config/minicode/release.keystore"
    if [ ! -f "$KEYSTORE" ] ||
       ! security find-generic-password -a minicode -s minicode-android-keystore -w >/dev/null 2>&1; then
        echo "error: no Android release key (see the top of this script), or set MINICODE_NO_APK=1" >&2
        exit 1
    fi
fi

echo "==> Running the core tests"
make test

echo "==> Stamping version $VERSION"
"$BUDDY" -c "Set :CFBundleShortVersionString $VERSION" "$PLIST"
"$BUDDY" -c "Set :CFBundleVersion $VERSION" "$PLIST"

echo "==> Building release artifact"
make dist-zip
BUILT="$("$BUDDY" -c "Print :CFBundleShortVersionString" MiniCode.app/Contents/Info.plist)"
if [ "$BUILT" != "$VERSION" ]; then
    echo "error: the built app says $BUILT, expected $VERSION" >&2; exit 1
fi
SHA="$(shasum -a 256 MiniCode.zip | awk '{print $1}')"
echo "    version $VERSION   sha256 $SHA"

/usr/bin/sed -i '' "s/version \"[^\"]*\"/version \"$VERSION\"/" "$CASK"
/usr/bin/sed -i '' "s/sha256 \"[^\"]*\"/sha256 \"$SHA\"/" "$CASK"
git add "$PLIST" "$CASK"
git diff --cached --quiet || git commit -q -m "Release $VERSION"

if [ -z "${MINICODE_NO_APK:-}" ]; then
    echo "==> Building the Android APK"
    APK="MiniCode-$VERSION.apk"
    (
        cd android
        # Homebrew's openjdk@21 is not registered with java_home.
        if [ -z "${JAVA_HOME:-}" ]; then
            JAVA_HOME="$(/usr/libexec/java_home -v 21 2>/dev/null ||
                         echo /opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home)"
        fi
        export JAVA_HOME
        export MINICODE_KEYSTORE="$KEYSTORE"
        MINICODE_KEYSTORE_PASSWORD="$(security find-generic-password -a minicode -s minicode-android-keystore -w)"
        export MINICODE_KEYSTORE_PASSWORD
        ./gradlew -q -PminicodeVersion="$VERSION" assembleRelease
    )
    cp android/app/build/outputs/apk/release/app-release.apk "$APK"
fi

echo "==> Pushing main"
git push -q origin main

echo "==> Publishing GitHub Release v$VERSION on $APP_REPO"
gh release create "v$VERSION" MiniCode.zip ${APK:+"$APK"} \
    --repo "$APP_REPO" \
    --target "$(git rev-parse HEAD)" \
    --title "MiniCode $VERSION" \
    --notes "**macOS:** \`brew install --cask e-c-hansen/tap/minicode\`, or download MiniCode.zip below.

**Android:** download ${APK:-the APK} on your phone and open it; Android asks once whether your browser may install apps. Termux setup and updates through Obtainium: [android/README.md](https://github.com/$APP_REPO/blob/main/android/README.md#install).

Edit these notes on GitHub to say what changed."

echo "==> Writing the cask to $TAP_REPO"
TMP="$(mktemp -d)"
gh repo clone "$TAP_REPO" "$TMP" -- -q
cp "$CASK" "$TMP/Casks/minicode.rb"
git -C "$TMP" add -A
if ! git -C "$TMP" diff --cached --quiet; then
    git -C "$TMP" commit -q -m "MiniCode $VERSION"
    git -C "$TMP" push -q
fi
rm -rf "$TMP"
[ -n "$APK" ] && rm -f "$APK"

PPA_FAILED=""
if [ -n "${MINICODE_NO_PPA:-}" ]; then
    echo "==> Skipping the Ubuntu PPA (MINICODE_NO_PPA is set)"
elif [ -z "$PPA_KEY" ]; then
    echo "==> Skipping the Ubuntu PPA: no signing key configured (MINICODE_PPA_KEY or"
    echo "    ~/.config/minicode/ppa-key-id; packaging/PPA.md has the one-time setup)"
else
    echo "==> Uploading $VERSION to the Ubuntu PPA"
    if ! MINICODE_PPA_KEY="$PPA_KEY" scripts/ppa.sh "$VERSION"; then
        PPA_FAILED=1
    fi
fi

echo "==> Done. brew upgrade --cask e-c-hansen/tap/minicode will now pull $VERSION."
if [ -n "$PPA_FAILED" ]; then
    echo "error: the PPA upload failed; everything else is released. Retry it with" >&2
    echo "       scripts/ppa.sh $VERSION" >&2
    exit 1
fi
