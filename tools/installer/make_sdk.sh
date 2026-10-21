#!/bin/bash
# Fills the installer bundle with what building the game on a player's Mac needs: XenonRecomp, the prebuilt
# runtime libraries, the headers the recompiled code includes, the recompiler configuration, the game app's
# Info.plist and FFmpeg. None of it comes from the game.
# usage: make_sdk.sh <installer.app> <source root> <runtime.a> <xenos.a> <xenonutils.a> <fmt.a> <XenonRecomp> <ffmpeg lib dir>
set -euo pipefail
APP=$1; ROOT=$2; FFMPEG=$8
SDK="$APP/Contents/Resources/sdk"
rm -rf "$SDK"; mkdir -p "$SDK/bin" "$SDK/lib" "$SDK/include" "$SDK/config" "$APP/Contents/Frameworks"
cp "$7" "$SDK/bin/XenonRecomp"
cp "$3" "$SDK/lib/libPeggle2Runtime.a"; cp "$4" "$SDK/lib/libXenos.a"; cp "$5" "$SDK/lib/libXenonUtils.a"; cp "$6" "$SDK/lib/libfmt.a"
cp "$ROOT/third_party/XenonRecomp/XenonUtils/ppc_context.h" "$ROOT/runtime/recomp_hooks.h" "$SDK/include/"
rsync -a --exclude .git "$ROOT/third_party/XenonRecomp/thirdparty/simde/" "$SDK/include/simde/"
cp "$ROOT/config/Peggle2.toml" "$ROOT/config/switch_tables.toml" "$SDK/config/"
cp "$ROOT/packaging/Info.plist" "$SDK/Info.plist"
cp "$ROOT/tools/installer/build_game.sh" "$ROOT/tools/make_icon.sh" "$SDK/"
cp "$FFMPEG/libavcodec.58.dylib" "$FFMPEG/libavutil.56.dylib" "$APP/Contents/Frameworks/"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$ROOT/packaging/InstallerIcon.icns" "$APP/Contents/Resources/"
rsync -a "$ROOT/packaging/licenses/" "$APP/Contents/Resources/licenses/"
# Any change to what the installer builds with gives a new id, which rebuilds the player's game.
cat "$SDK/bin/XenonRecomp" "$SDK/lib/"*.a "$SDK/include/"*.h "$SDK/"*.sh "$SDK/Info.plist" "$SDK/config/"* | shasum | cut -c1-16 > "$SDK/build-id"
"$ROOT/tools/sign_bundle.sh" "$APP"
