#!/bin/bash
# Run by the installer on the player's Mac: recompiles their Default.xex, compiles it with the Command Line
# Tools, links it with the prebuilt runtime and assembles Peggle 2.app. Prints STEP <text>, TOTAL <n> and
# DONE lines for the installer's progress display.
# usage: build_game.sh <sdk> <game dir> <work dir> <output app> <installer Contents dir> <version>
set -euo pipefail
SDK=$1; GAME=$2; WORK=$3; APP=$4; INSTALLER=$5; VERSION=$6
rm -rf "$WORK"; mkdir -p "$WORK/ppc" "$WORK/obj"

echo "STEP Translating the game's code"
# XenonRecomp resolves the configuration's paths against the configuration's own folder.
ln -s "$GAME" "$WORK/game"
cp "$SDK/config/switch_tables.toml" "$WORK/"
sed -e 's|^file_path = .*|file_path = "game/Default.xex"|' -e 's|^out_directory_path = .*|out_directory_path = "ppc"|' \
    "$SDK/config/Peggle2.toml" > "$WORK/Peggle2.toml"
"$SDK/bin/XenonRecomp" "$WORK/Peggle2.toml" "$SDK/include/ppc_context.h" > "$WORK/recomp.log" 2>&1
cp "$SDK/include/ppc_context.h" "$WORK/ppc/"

echo "STEP Compiling"
SOURCES=("$WORK"/ppc/ppc_recomp.*.cpp "$WORK/ppc/ppc_func_mapping.cpp")
echo "TOTAL ${#SOURCES[@]}"
export SDK WORK
compile() {
    clang++ -std=gnu++20 -O3 -DNDEBUG -arch arm64 -mmacosx-version-min=13.0 -fno-strict-aliasing -w \
        -include "$SDK/include/recomp_hooks.h" -I "$WORK/ppc" -I "$SDK/include" -I "$SDK/include/simde" \
        -c "$1" -o "$WORK/obj/$(basename "$1" .cpp).o" && echo DONE
}
export -f compile
printf '%s\n' "${SOURCES[@]}" | xargs -P "$(sysctl -n hw.ncpu)" -I{} bash -c 'compile "$1"' _ {}

echo "STEP Linking"
FW=(AudioToolbox ImageIO CoreGraphics CoreAudio AppKit CoreHaptics IOKit GameController Metal QuartzCore)
clang++ -arch arm64 -mmacosx-version-min=13.0 -o "$WORK/Peggle2" "$WORK"/obj/*.o \
    -Wl,-force_load,"$SDK/lib/libPeggle2Runtime.a" "$SDK/lib/libXenos.a" "$SDK/lib/libXenonUtils.a" "$SDK/lib/libfmt.a" \
    "$INSTALLER/Frameworks/libavcodec.58.dylib" "$INSTALLER/Frameworks/libavutil.56.dylib" \
    -Wl,-rpath,@executable_path/../Frameworks $(printf -- '-framework %s ' "${FW[@]}")

echo "STEP Finishing"
NEW="$WORK/Peggle 2.app"
mkdir -p "$NEW/Contents/MacOS" "$NEW/Contents/Frameworks" "$NEW/Contents/Resources"
cp "$SDK/Info.plist" "$NEW/Contents/"
cp "$WORK/Peggle2" "$NEW/Contents/MacOS/"
cp "$INSTALLER"/Frameworks/*.dylib "$NEW/Contents/Frameworks/"
cp -R "$INSTALLER/Resources/licenses" "$INSTALLER/Resources/THIRD_PARTY_NOTICES.md" "$NEW/Contents/Resources/"
echo "$VERSION" > "$NEW/Contents/Resources/installer-version"
"$SDK/make_icon.sh" "$NEW" "$GAME"
codesign --force --sign - "$NEW"/Contents/Frameworks/*.dylib > /dev/null
codesign --force --sign - "$NEW" > /dev/null
rm -rf "$APP"; mkdir -p "$(dirname "$APP")"; mv "$NEW" "$APP"
rm -rf "$WORK"
echo "STEP Done"
