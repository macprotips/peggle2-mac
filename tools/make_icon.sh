#!/bin/bash
# Puts AppIcon.icns, composed from the game's artwork, into the app bundle. usage: make_icon.sh <app> <game dir>
set -euo pipefail
APP=$1; GAME=$2
[[ -f $GAME/BannerLogo.png || -f $GAME/icon.png ]] || exit 0
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$APP/Contents/MacOS/Peggle2" --write-icon "$GAME" "$WORK/icon.png"
mkdir "$WORK/AppIcon.iconset"
for size in 16 32 128 256 512; do
    sips -z $size $size "$WORK/icon.png" --out "$WORK/AppIcon.iconset/icon_${size}x${size}.png" > /dev/null
    sips -z $((size * 2)) $((size * 2)) "$WORK/icon.png" --out "$WORK/AppIcon.iconset/icon_${size}x${size}@2x.png" > /dev/null
done
mkdir -p "$APP/Contents/Resources"
iconutil -c icns "$WORK/AppIcon.iconset" -o "$APP/Contents/Resources/AppIcon.icns"
