#!/bin/bash
# Build MiniCode.app and package it into a distributable disk image (.dmg).
#
# This works WITHOUT an Apple Developer account, but the result is unsigned, so
# people who download it will need to right-click the app and choose Open the
# first time (Gatekeeper blocks a double-click on unsigned apps). For a clean
# open-on-double-click experience, sign and notarize instead: see
# scripts/sign-and-notarize.sh.
set -euo pipefail
cd "$(dirname "$0")/.."

make app

APP="MiniCode.app"
DMG="MiniCode.dmg"
STAGING="$(mktemp -d)"

cp -R "$APP" "$STAGING/"
ln -s /Applications "$STAGING/Applications"   # drag-to-install target

rm -f "$DMG"
hdiutil create -volname "MiniCode" -srcfolder "$STAGING" \
    -ov -format UDZO "$DMG" >/dev/null
rm -rf "$STAGING"

echo "Created $DMG"
