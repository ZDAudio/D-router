#!/usr/bin/env bash
# Package D-Router.app into dist/ with a VALID ad-hoc signature, then zip.
#
# Why this exists: copying the built .app (cp -R) invalidates the linker's
# ad-hoc resource seal, leaving the bundle with a broken signature.  A broken
# signature + the quarantine flag a recipient gets from WeChat / AirDrop /
# browser download makes macOS report the app as "damaged and can't be
# opened".  Re-sealing the whole bundle ad-hoc here fixes that.
#
# IMPORTANT: ad-hoc signing is NOT Apple notarization.  Recipients who receive
# the zip over WeChat/AirDrop/browser must still clear the quarantine flag once
# after unzipping, e.g.:
#     xattr -cr /path/to/D-Router.app       # then double-click to open
# (or right-click the app -> Open -> Open).
#
# The only way to remove that step entirely is Developer ID signing +
# notarization, which needs a paid Apple Developer account.  See the bottom of
# this file for the notarization recipe to drop in once an account exists.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APP_SRC="$ROOT/build/dcorerouter_artefacts/Release/D-Router.app"
DIST="$ROOT/dist"

[ -d "$APP_SRC" ] || { echo "Build first -- not found: $APP_SRC"; exit 1; }

rm -rf "$DIST/D-Router.app"
mkdir -p "$DIST"
cp -R "$APP_SRC" "$DIST/D-Router.app"

# Re-seal the bundle ad-hoc (repairs the seal the cp -R just broke).
codesign --force --deep --sign - "$DIST/D-Router.app"
codesign --verify --deep --strict --verbose=2 "$DIST/D-Router.app"

# Strip any local quarantine, then zip with ditto (keeps bundle layout intact).
xattr -cr "$DIST/D-Router.app"
( cd "$DIST" && rm -f D-Router.zip && ditto -c -k --keepParent D-Router.app D-Router.zip )

echo "Packaged: $DIST/D-Router.zip"
echo "Recipient still runs once:  xattr -cr /path/to/D-Router.app"

# ---------------------------------------------------------------------------
# Frictionless distribution (no recipient Terminal step) -- requires a paid
# Apple Developer account ($99/yr).  Once you have a "Developer ID Application"
# cert and an app-specific password, replace the ad-hoc codesign above with:
#
#   codesign --force --deep --options runtime \
#     --sign "Developer ID Application: Your Name (TEAMID)" "$DIST/D-Router.app"
#   ( cd "$DIST" && ditto -c -k --keepParent D-Router.app D-Router.zip )
#   xcrun notarytool submit "$DIST/D-Router.zip" \
#     --apple-id you@example.com --team-id TEAMID --password APP_SPECIFIC_PW --wait
#   xcrun stapler staple "$DIST/D-Router.app"
#   ( cd "$DIST" && rm -f D-Router.zip && ditto -c -k --keepParent D-Router.app D-Router.zip )
#
# After notarization the recipient just double-clicks -- no warning, no xattr.
# ---------------------------------------------------------------------------
