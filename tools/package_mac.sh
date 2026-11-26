#!/bin/sh
# package_mac.sh - the macOS release: a Developer-ID-signed, notarized and stapled universal app, as a zip and as a
# disk image (D25 in docs/DECISIONS.md).
#
#   tools/package_mac.sh [MAYA_DATA folder]
#
# Environment (defaults in brackets):
#   MAYA_SIGN_ID      the codesign identity ["Developer ID Application: Onur Buyukcaglar (SZ4F4WP934)"]
#   MAYA_NOTARY       the notarytool keychain profile [civ64-notary]; made once with
#                     xcrun notarytool store-credentials <profile> --apple-id <id> --team-id <team>
#   MAYA_NOTARIZE=0   sign only (no notarization, no stapling): for a local check
#
# Outputs in build/release: maya64-<v>-macos.zip (the app, README.txt, LICENSE.txt), maya64-<v>-macos.dmg (the
# same with a link to /Applications), their .sha256, and the notary's answers (notary-*.json).  The game is not in
# either: the app needs the player's own copy of the Amiga game.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="${1:-$ROOT/_extracted/amiga/Maya/data}"
SIGN_ID="${MAYA_SIGN_ID:-Developer ID Application: Onur Buyukcaglar (SZ4F4WP934)}"
PROFILE="${MAYA_NOTARY:-civ64-notary}"
NOTARIZE="${MAYA_NOTARIZE:-1}"
VERSION="$(sed -n 's/^#define HOST_VERSION_STR "\(.*\)"/\1/p' "$ROOT/host/macos/host_config.h")"
NAME="Le Fetiche Maya 64bit"
BASE="maya64-$VERSION-macos"
OUT="$ROOT/build/release"
APP="$OUT/$NAME.app"

rm -rf "$OUT"
mkdir -p "$OUT"

# 1. a clean universal Release build, signed with the hardened runtime and a secure timestamp
rm -rf "$ROOT/build/mac"
"$ROOT/build.sh" "$DATA" -DMAYA_DEVTEST=OFF "-DMAYA_CODESIGN_IDENTITY=$SIGN_ID"
ditto "$ROOT/build/mac/$NAME.app" "$APP"
codesign --verify --strict --deep --verbose=2 "$APP"
lipo -archs "$APP/Contents/MacOS/$NAME"

notarize() {   # notarize <file> <tag>: submit, wait, keep the answer and the log
    xcrun notarytool submit "$1" --keychain-profile "$PROFILE" --wait --output-format json > "$OUT/notary-$2.json"
    cat "$OUT/notary-$2.json"; echo
    ID="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$OUT/notary-$2.json")"
    xcrun notarytool log "$ID" --keychain-profile "$PROFILE" "$OUT/notary-$2-log.json" >/dev/null
    python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(d["status"] != "Accepted")' "$OUT/notary-$2.json"
}

# 2. the app: notarized and stapled, so it also opens offline once copied out of the zip or the disk image
if [ "$NOTARIZE" = 1 ]; then
    ditto -c -k --keepParent "$APP" "$OUT/app-for-notary.zip"
    notarize "$OUT/app-for-notary.zip" app
    rm -f "$OUT/app-for-notary.zip"
    xcrun stapler staple "$APP"
    spctl --assess --type execute --verbose=2 "$APP"
fi

# 3. the zip: a folder with the app, README.txt and LICENSE.txt
STAGE="$OUT/stage/$BASE"
mkdir -p "$STAGE"
ditto "$APP" "$STAGE/$NAME.app"
cp "$ROOT/res/macos/README.txt" "$STAGE/README.txt"
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
(cd "$OUT/stage" && ditto -c -k --norsrc --noextattr --noacl --keepParent "$BASE" "$OUT/$BASE.zip")

# 4. the disk image: the same files and a link to /Applications; signed, notarized and stapled
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "$NAME $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -imagekey zlib-level=9 \
    -ov "$OUT/$BASE.dmg"
codesign --force --sign "$SIGN_ID" --timestamp "$OUT/$BASE.dmg"
codesign --verify --verbose=2 "$OUT/$BASE.dmg"
if [ "$NOTARIZE" = 1 ]; then
    notarize "$OUT/$BASE.dmg" dmg
    xcrun stapler staple "$OUT/$BASE.dmg"
    xcrun stapler validate "$OUT/$BASE.dmg"
    spctl --assess --type open --context context:primary-signature --verbose=2 "$OUT/$BASE.dmg"
fi
rm -rf "$OUT/stage"

(cd "$OUT" && shasum -a 256 "$BASE.zip" > "$BASE.zip.sha256" && shasum -a 256 "$BASE.dmg" > "$BASE.dmg.sha256")
echo
ls -l "$OUT"
cat "$OUT"/*.sha256
