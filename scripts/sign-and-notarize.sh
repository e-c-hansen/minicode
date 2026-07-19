#!/bin/bash
# Sign, notarize, and staple MiniCode so it opens cleanly on any Mac.
#
# Requirements:
#   1. An Apple Developer Program membership ($99/year).
#   2. A "Developer ID Application" certificate installed in your login keychain
#      (create it in the Apple Developer portal, then download and double-click).
#   3. A one-time notarytool credential profile, created with:
#        xcrun notarytool store-credentials minicode-notary \
#            --apple-id "you@example.com" --team-id "YOURTEAMID"
#      (Use an app-specific password from appleid.apple.com, not your login one.)
#
# Then set DEV_ID below to match your certificate and run this script.
set -euo pipefail
cd "$(dirname "$0")/.."

DEV_ID="Developer ID Application: YOUR NAME (YOURTEAMID)"   # <-- edit this
NOTARY_PROFILE="minicode-notary"

make app

# Sign with the hardened runtime (required for notarization). MiniCode uses no
# private APIs, so no special entitlements are needed; if a future change needs
# one, add --entitlements scripts/entitlements.plist here.
codesign --force --deep --options runtime --timestamp \
    --sign "$DEV_ID" MiniCode.app
codesign --verify --strict --verbose=2 MiniCode.app

# Package, submit the dmg for notarization, then staple the ticket to both the
# dmg and the app inside it so they verify offline.
./scripts/make-dmg.sh
xcrun notarytool submit MiniCode.dmg --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple MiniCode.dmg
xcrun stapler staple MiniCode.app

echo
echo "Signed, notarized, and stapled. MiniCode.dmg is ready to hand out."
