#!/bin/bash
# Cut a MiniCode release and update the Homebrew tap in one shot.
#   scripts/release.sh 1.1.0
#
# It builds the zip, computes its checksum, publishes a GitHub Release on the
# tap repo with the zip attached, and bumps the cask's version and sha256.
# Requires the gh CLI, authenticated with push access to the tap repo.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: scripts/release.sh <version>   e.g. 1.1.0}"
TAP_REPO="e-c-hansen/homebrew-tap"

echo "==> Building release artifact"
make dist-zip
SHA="$(shasum -a 256 MiniCode.zip | awk '{print $1}')"
echo "    version $VERSION   sha256 $SHA"

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

echo "==> Done. brew upgrade --cask $TAP_REPO/minicode will now pull $VERSION."
