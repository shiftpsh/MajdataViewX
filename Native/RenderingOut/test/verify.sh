#!/usr/bin/env bash
# End-to-end checks for the portable RenderingOut build.
#
#   test/verify.sh <path/to/encode_test>
#
# Needs the ffmpeg/ffprobe CLI on PATH (only for verification).
set -euo pipefail

ENCODE_TEST=${1:?usage: verify.sh path/to/encode_test}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
FAILURES=0

fail() { echo "  FAIL: $*"; FAILURES=$((FAILURES + 1)); }
pass() { echo "  ok:   $*"; }

probe() { # file stream entry
    ffprobe -v error -select_streams "$2" -show_entries "stream=$3" \
        -of default=nw=1:nk=1 "$1" | head -1
}

near() { # actual expected tolerance
    awk -v a="$1" -v e="$2" -v t="$3" 'BEGIN { exit !((a - e) <= t && (e - a) <= t) }'
}

# Mean Y/U/V of a 16x16 block of the first frame at (x, y).
sample_yuv() { # file x y
    ffmpeg -v error -i "$1" -frames:v 1 \
        -vf "crop=16:16:$2:$3,format=yuv444p" -f rawvideo - |
        od -An -v -tu1 | tr -s ' ' '\n' | grep -v '^$' |
        awk '{ v[int((NR - 1) / 256)] += $1 } END {
            printf "%d %d %d", v[0] / 256 + 0.5, v[1] / 256 + 0.5, v[2] / 256 + 0.5 }'
}

# name encoder quality w h fps frames flip audio_seconds matrix [noise]
run_case() {
    local name=$1 encoder=$2 quality=$3 w=$4 h=$5 fps=$6 frames=$7 flip=$8 audio=$9 matrix=${10} noise=${11:-0}
    local file="$OUT/$name.mp4"
    echo "== $name ($encoder q$quality ${w}x${h}@$fps, $frames frames, flip=$flip, audio=${audio}s)"

    if ! RENDERINGOUT_ENCODER=$encoder "$ENCODE_TEST" "$file" "$quality" "$w" "$h" "$fps" "$frames" "$flip" "$audio" "$noise" \
        2> "$OUT/$name.log"; then
        fail "encode_test exited non-zero"; sed 's/^/    /' "$OUT/$name.log"; return
    fi
    grep -q "encoder selection \[.*$encoder=selected" "$OUT/$name.log" \
        && pass "selected $encoder" || fail "$encoder not selected: $(grep 'encoder selection' "$OUT/$name.log")"
    if grep -q '^\[error\]' "$OUT/$name.log"; then
        fail "plugin logged errors"; grep '^\[error\]' "$OUT/$name.log" | sed 's/^/    /'
    fi

    [ "$(probe "$file" v:0 codec_name)" = h264 ] && pass "h264" || fail "codec $(probe "$file" v:0 codec_name)"
    [ "$(probe "$file" v:0 width)x$(probe "$file" v:0 height)" = "${w}x${h}" ] && pass "size" || fail "size"
    local counted
    counted=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$file")
    [ "$counted" = "$frames" ] && pass "$frames frames" || fail "frames: $counted != $frames"
    [ "$(probe "$file" v:0 color_range)" = tv ] && pass "limited range" || fail "range $(probe "$file" v:0 color_range)"
    [ "$(probe "$file" v:0 color_space)" = "$matrix" ] && pass "tagged $matrix" || fail "matrix $(probe "$file" v:0 color_space)"

    [ "$(probe "$file" a:0 codec_name)" = aac ] && pass "aac" || fail "no aac track"
    [ "$(probe "$file" a:0 sample_rate)" = 44100 ] && [ "$(probe "$file" a:0 channels)" = 2 ] \
        && pass "44100 Hz stereo" || fail "audio format"

    # Audio must cover the video, and only run past it if the source did.
    local vdur adur expect
    vdur=$(awk -v f="$frames" -v r="$fps" 'BEGIN { printf "%.4f", f / r }')
    adur=$(probe "$file" a:0 duration)
    expect=$(awk -v v="$vdur" -v a="$audio" 'BEGIN { printf "%.4f", (a > v ? a : v) }')
    near "$adur" "$expect" 0.05 && pass "audio ${adur}s ~ ${expect}s" || fail "audio ${adur}s, expected ~${expect}s"

    # Top half red, bottom half blue, regardless of flip.
    local top bottom ey_red ey_blue
    if [ "$matrix" = bt709 ]; then ey_red=63; ey_blue=32; else ey_red=82; ey_blue=41; fi
    top=$(sample_yuv "$file" $((w * 3 / 4)) $((h / 4)))
    bottom=$(sample_yuv "$file" $((w * 3 / 4)) $((h * 3 / 4)))
    read -r ty tu tv <<< "$top"
    read -r by bu bv <<< "$bottom"
    near "$ty" "$ey_red" 3 && near "$tv" 240 4 && pass "top is red (YUV $top)" || fail "top YUV $top, expected red Y~$ey_red V~240"
    near "$by" "$ey_blue" 3 && near "$bu" 240 4 && pass "bottom is blue (YUV $bottom)" || fail "bottom YUV $bottom, expected blue Y~$ey_blue U~240"

    probe "$file" v:0 bit_rate > "$OUT/$name.bitrate"
}

ENCODERS=${ENCODERS:-"h264_videotoolbox libx264"}

for enc in $ENCODERS; do
    run_case "$enc-1080p60"        "$enc" 2 1920 1080 60 120 0 2.0    bt709
    run_case "$enc-1080p60-flip"   "$enc" 2 1920 1080 60 120 1 2.0    bt709
    run_case "$enc-720p30-short"   "$enc" 1 1280  720 30  60 0 1.3337 bt709
    run_case "$enc-720p30-long"    "$enc" 1 1280  720 30  60 0 3.5    bt709
    run_case "$enc-720p30-silent"  "$enc" 1 1280  720 30  60 0 0      bt709
    run_case "$enc-480p-601"       "$enc" 2  640  480 60  60 0 1.0    smpte170m

    # Noise in the middle third makes rate control, not content, bound size.
    for q in 0 1 2 3; do
        run_case "$enc-noise-q$q" "$enc" "$q" 1920 1080 60 240 0 4.0 bt709 1
    done
    echo "== $enc quality ladder"
    prev=0
    for q in 0 1 2 3; do
        rate=$(cat "$OUT/$enc-noise-q$q.bitrate")
        [ "$rate" -gt "$prev" ] && pass "q$q $((rate / 1000)) kbps > previous" || fail "q$q $((rate / 1000)) kbps not above previous"
        prev=$rate
    done
    if [ "$enc" = h264_videotoolbox ]; then
        # ENCODER_QUALITY_PRESETS.md ladder at 1080p60: 4/8/16/32 Mbps.
        q=0
        for target in 4000000 8000000 16000000 32000000; do
            rate=$(cat "$OUT/$enc-noise-q$q.bitrate")
            near "$rate" "$target" $((target * 15 / 100)) \
                && pass "q$q within 15% of $((target / 1000000)) Mbps" \
                || fail "q$q $((rate / 1000)) kbps, target $((target / 1000)) kbps"
            q=$((q + 1))
        done
    fi
done

echo
if [ "$FAILURES" -eq 0 ]; then echo "ALL PASSED"; else echo "$FAILURES FAILURE(S)"; exit 1; fi
