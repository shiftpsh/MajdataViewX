#!/usr/bin/env bash
# Builds Assets/Plugins/macOS/libRenderingOut.dylib: the portable RenderingOut
# with a minimal FFmpeg and x264 linked in statically, as a universal binary.
#
# FFmpeg tracks the same release branch as ffmpeg-builder/build.ps1. The
# encoder set mirrors the Windows build with VideoToolbox in place of
# NVENC/AMF/QSV/MF, so the dylib depends on system frameworks only.
#
# Requirements: Xcode command line tools, cmake, git, nasm, pkg-config
#   brew install cmake nasm pkgconf
#
# Environment:
#   ARCHS                      default "arm64 x86_64"
#   MACOSX_DEPLOYMENT_TARGET   default 11.0
#   JOBS                       default: all cores
#   SKIP_TESTS=1               skip test/verify.sh (needs ffmpeg/ffprobe CLI)
#   REUSE_DEPS=1               reuse FFmpeg/x264 already built in .work and
#                              only rebuild the plugin
set -euo pipefail

FFMPEG_REF=release/8.1
X264_REF=master

ARCHS=${ARCHS:-"arm64 x86_64"}
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}

ROOT=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$ROOT/../.." && pwd)
WORK=$ROOT/.work
SRC=$WORK/src
OUTPUT=$REPO/Assets/Plugins/macOS/libRenderingOut.dylib

step() { printf '\n==> %s\n' "$*"; }

for tool in cmake git nasm pkg-config clang lipo; do
    command -v "$tool" >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

fetch() { # url ref dir
    if [ -d "$3/.git" ]; then
        git -C "$3" fetch --depth 1 origin "$2"
        git -C "$3" checkout -q --detach FETCH_HEAD
    else
        git clone --depth 1 --branch "$2" "$1" "$3"
    fi
}

if [ "${REUSE_DEPS:-0}" != 1 ]; then
    step "Fetching FFmpeg $FFMPEG_REF and x264 $X264_REF"
    mkdir -p "$SRC"
    fetch https://github.com/FFmpeg/FFmpeg.git "$FFMPEG_REF" "$SRC/ffmpeg"
    fetch https://code.videolan.org/videolan/x264.git "$X264_REF" "$SRC/x264"
fi

SLICES=()
for arch in $ARCHS; do
    case $arch in
        arm64)  host=aarch64-apple-darwin; ffarch=aarch64 ;;
        x86_64) host=x86_64-apple-darwin;  ffarch=x86_64 ;;
        *) echo "unsupported arch: $arch" >&2; exit 1 ;;
    esac
    cross=()
    [ "$arch" = "$(uname -m)" ] || cross=(--enable-cross-compile)

    prefix=$WORK/$arch/install
    build=$WORK/$arch/build
    flags="-arch $arch -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
    if [ "${REUSE_DEPS:-0}" = 1 ] && [ -f "$prefix/lib/libavcodec.a" ]; then
        step "[$arch] Reusing FFmpeg and x264 from $prefix"
        rm -rf "$build/renderingout"
    else
        rm -rf "$WORK/$arch"
        mkdir -p "$prefix" "$build"

        step "[$arch] x264 (static, 8-bit 4:2:0)"
        (
            cd "$SRC/x264"
            make distclean >/dev/null 2>&1 || true
            CC=clang ./configure \
                --host="$host" \
                --prefix="$prefix" \
                --enable-static \
                --disable-cli \
                --enable-pic \
                --bit-depth=8 \
                --chroma-format=420 \
                --disable-opencl \
                --extra-cflags="$flags -Os" \
                --extra-asflags="$([ "$arch" = arm64 ] && echo "$flags")" \
                --extra-ldflags="$flags"
            make -j"$JOBS"
            make install
        )

        step "[$arch] FFmpeg (static, minimal)"
        (
            mkdir -p "$build/ffmpeg"
            cd "$build/ffmpeg"
            PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" \
            "$SRC/ffmpeg/configure" \
                --prefix="$prefix" \
                --arch="$ffarch" \
                --target-os=darwin \
                ${cross[@]+"${cross[@]}"} \
                --cc=clang \
                --extra-cflags="$flags" \
                --extra-ldflags="$flags" \
                --pkg-config=pkg-config \
                --pkg-config-flags=--static \
                --enable-static \
                --disable-shared \
                --enable-pic \
                --enable-small \
                --enable-gpl \
                --disable-programs \
                --disable-doc \
                --disable-debug \
                --disable-network \
                --disable-autodetect \
                --disable-avdevice \
                --disable-avfilter \
                --enable-pthreads \
                --disable-everything \
                --enable-avcodec \
                --enable-avformat \
                --enable-avutil \
                --enable-swscale \
                --enable-swresample \
                --enable-encoder=aac \
                --enable-encoder=h264_videotoolbox \
                --enable-encoder=libx264 \
                --enable-muxer=mov,mp4 \
                --enable-protocol=file \
                --enable-libx264 \
                --enable-videotoolbox
            make -j"$JOBS"
            make install
        )
    fi

    step "[$arch] RenderingOut"
    cmake -S "$ROOT" -B "$build/renderingout" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
        -DFFMPEG_PREFIX="$prefix" \
        -DRENDERINGOUT_STATIC_FFMPEG=ON \
        -DRENDERINGOUT_BUILD_TESTS=ON
    cmake --build "$build/renderingout" -j"$JOBS"
    SLICES+=("$build/renderingout/libRenderingOut.dylib")

    if [ "${SKIP_TESTS:-0}" != 1 ]; then
        step "[$arch] Verifying"
        # On Apple Silicon the x86_64 encode_test runs under Rosetta.
        "$ROOT/test/verify.sh" "$build/renderingout/encode_test"
    fi
done

step "Creating universal $OUTPUT"
mkdir -p "$(dirname "$OUTPUT")"
lipo -create "${SLICES[@]}" -output "$OUTPUT"
codesign --force --sign - "$OUTPUT"

step "Checking the result"
lipo -info "$OUTPUT"
# Only system libraries and frameworks may appear here.
for arch in $ARCHS; do
    if otool -arch "$arch" -L "$OUTPUT" | tail -n +2 |
        grep -vE '^\s+(/usr/lib/|/System/Library/|@rpath/libRenderingOut)'; then
        echo "unexpected non-system dependency ($arch)" >&2
        exit 1
    fi
done
nm -gU "$OUTPUT" | awk '{ print $3 }' | sort -u
ls -l "$OUTPUT"
