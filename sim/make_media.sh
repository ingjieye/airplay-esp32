#!/bin/bash
# Generate simulator test audio: sine + pink noise (frames with both channels at 0 almost never occur, which makes silence padding easy to tell apart)
set -e
cd "$(dirname "$0")/media"
gen() { # name sample_rate bit_depth seconds
  local fmt=s32 extra="-bits_per_raw_sample 24"
  [ "$3" = 16 ] && fmt=s16 && extra=""
  ffmpeg -v error -y \
    -f lavfi -i "sine=frequency=440:sample_rate=$2:duration=$4" \
    -f lavfi -i "sine=frequency=660:sample_rate=$2:duration=$4" \
    -f lavfi -i "anoisesrc=color=pink:amplitude=0.2:sample_rate=$2:duration=$4:seed=7" \
    -filter_complex "[0][1]amerge=inputs=2,volume=0.4[s];[2]pan=stereo|c0=c0|c1=0.7*c0[n];[s][n]amix=inputs=2:normalize=0" \
    -ar "$2" -sample_fmt $fmt $extra -c:a flac "$1"
}
gen t44_16.flac 44100 16 20
gen t48_24.flac 48000 24 15
gen t96_24.flac 96000 24 8
gen t192_24.flac 192000 24 3
gen t44_30s.flac 44100 16 30
ffmpeg -v error -y -f lavfi -i "nullsrc=s=900x900,geq=random(1)*255:random(2)*255:random(3)*255" -frames:v 1 -q:v 2 cover.jpg
metaflac --import-picture-from="3|image/jpeg|||cover.jpg" t48_24.flac
ffmpeg -v error -y -i t44_16.flac -c:a libmp3lame -b:a 320k -id3v2_version 3 t44.mp3
# 44.1k/24bit + ~400KB cover art (mimics NetEase 24bit tracks with large covers, e.g. one with a 509KB cover)
gen t44_cover24.flac 44100 24 10
ffmpeg -v error -y -f lavfi -i "nullsrc=s=620x620,geq=random(1)*255:random(2)*255:random(3)*255" -frames:v 1 -q:v 2 cover400.jpg
metaflac --import-picture-from="3|image/jpeg|||cover400.jpg" t44_cover24.flac
# Newer NetEase files have a SEEKTABLE (a point every ~2 s); add one to t44_30s, keep t44_16 without (covers the binary-search path for old files)
metaflac --add-seekpoint=2s t44_30s.flac

# WAV (some NetEase tracks are WAV, labelled audio/mpeg)
ffmpeg -v error -y -i t44_16.flac -c:a pcm_s16le t44_16.wav
ffmpeg -v error -y -i t48_24.flac -map 0:a -c:a pcm_s24le t48_24.wav

# M4A (AAC): some NetEase tracks only exist in this format. One with moov at the front (faststart), one with moov at the end of the file
ffmpeg -v error -y -i t44_30s.flac -c:a aac -b:a 256k -movflags +faststart t44_fast.m4a
ffmpeg -v error -y -i t44_30s.flac -c:a aac -b:a 256k t44_tail.m4a
