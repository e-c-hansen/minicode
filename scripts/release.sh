#!/bin/bash
# Cut a MiniCode release and update the Homebrew tap in one shot.
#   scripts/release.sh 1.2.0
#
# The version given here is the one source of truth. The script:
#   1. checks it is releasing a clean, tested main,
#   2. stamps the version into Info.plist and the reference cask, and commits,
#   3. builds the zip and checks the built app reports that same version,
#   4. publishes a GitHub Release on the tap repo with the zip attached, and
#      bumps the live cask's version and sha256,
#   5. tags the source commit and pushes main and the tag.
# Requires the gh CLI, authenticated with push access to both repos.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: scripts/release.sh <version>   e.g. 1.2.0}"
TAP_REPO="e-c-hansen/homebrew-tap"
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
if gh release view "v$VERSION" --repo "$TAP_REPO" >/dev/null 2>&1; then
    echo "error: v$VERSION is already released on $TAP_REPO" >&2; exit 1
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

# Keep the reference copy of the cask in step with the live one.
/usr/bin/sed -i '' "s/version \"[^\"]*\"/version \"$VERSION\"/" packaging/minicode.rb
/usr/bin/sed -i '' "s/sha256 \"[^\"]*\"/sha256 \"$SHA\"/" packaging/minicode.rb
git add "$PLIST" packaging/minicode.rb
git diff --cached --quiet || git commit -q -m "Release $VERSION"

echo "==> Publishing GitHub Release v$VERSION on $TAP_REPO"
gh release create "v$VERSION" MiniCode.zip \
    --repo "$TAP_REPO" \
    --title "MiniCode $VERSION" \
    --notes "MiniCode $VERSION"

echo "==> Updating the cask"
TMP="$(mktemp -d)"
gh repo clone "$TAP_REPO" "$TMP" -- -q
CASK="$TMP/Casks/minicode.rb"
/usr/bin/sed -i '' "s/version \"[^\"]*\"/version \"$VERSION\"/" "$CASK"
/usr/bin/sed -i '' "s/sha256 \"[^\"]*\"/sha256 \"$SHA\"/" "$CASK"
git -C "$TMP" add -A
git -C "$TMP" commit -q -m "MiniCode $VERSION"
git -C "$TMP" push -q
rm -rf "$TMP"

echo "==> Tagging and pushing the source"
git tag -a "v$VERSION" -m "MiniCode $VERSION"
git push -q origin main "v$VERSION"

echo "==> Done. brew upgrade --cask e-c-hansen/tap/minicode will now pull $VERSION."
