#!/bin/bash
# Fetches pinned third-party sources, applies our XenonRecomp patches, builds FFmpeg for XMA audio and
# configures the build. Safe to re-run.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
TP=$ROOT/third_party
XENONRECOMP_BASE=ddd128bcca99fe8bfbb99bea583c972351fa6ace
FFMPEG_COMMIT=15ece0882e8d5875051ff5b73c5a8326f7cee9f5  # Xenia's fork
mkdir -p "$TP"

# Set up beside its final place and moved in only when complete, so a failed step is retried on the next run.
if [[ ! -d $TP/XenonRecomp ]]; then
    rm -rf "$TP/XenonRecomp.partial"
    git clone https://github.com/hedge-dev/XenonRecomp "$TP/XenonRecomp.partial"
    git -C "$TP/XenonRecomp.partial" switch -c peggle2 $XENONRECOMP_BASE
    git -C "$TP/XenonRecomp.partial" submodule update --init --recursive --depth 1
    git -C "$TP/XenonRecomp.partial" -c core.hooksPath=/dev/null am "$ROOT"/patches/XenonRecomp/*.patch
    mv "$TP/XenonRecomp.partial" "$TP/XenonRecomp"
fi

# Rebuilt when built for a different minimum macOS than the app (see CMakeLists.txt).
if [[ -f $TP/ffmpeg-install/lib/libavcodec.58.dylib ]] && ! otool -l "$TP/ffmpeg-install/lib/libavcodec.58.dylib" | grep -q 'minos 13.0'; then
    rm -rf "$TP/ffmpeg-install"
fi
if [[ ! -f $TP/ffmpeg-install/lib/libavcodec.58.dylib ]]; then
    # XMA audio decoding: only the xmaframes decoder, as shared libraries the app bundles (LGPL-2.1).
    [[ -d $TP/ffmpeg-xenia ]] || git clone --filter=blob:none https://github.com/xenia-project/FFmpeg "$TP/ffmpeg-xenia"
    git -C "$TP/ffmpeg-xenia" checkout -q $FFMPEG_COMMIT
    cd "$TP/ffmpeg-xenia"
    ./configure --prefix="$TP/ffmpeg-install" --install-name-dir='@rpath' --disable-everything --disable-programs \
        --disable-doc --disable-network --disable-avformat --disable-avdevice --disable-swresample --disable-swscale \
        --disable-postproc --disable-avfilter --enable-decoder=xmaframes --enable-shared --disable-static \
        --arch=arm64 --cc=clang --disable-asm --extra-cflags=-mmacosx-version-min=13.0 \
        --extra-ldflags=-mmacosx-version-min=13.0 > configure.log
    make clean > /dev/null 2>&1 || true
    make -j"$(sysctl -n hw.ncpu)" > make.log
    make install > install.log
fi

cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
# The tools; the app needs the recompiled code, which tools/regen.sh generates from the game.
cmake --build "$ROOT/build" --target XenonRecomp xexdump install_game
