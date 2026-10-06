#!/bin/sh
# Regenerates the media.local native-test fixtures. Needs ffmpeg and lame;
# the tests themselves read the committed outputs and need neither.
set -eu
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ffmpeg -v error -y -f lavfi -i "sine=frequency=440:duration=1:sample_rate=44100" -ac 2 "$tmp/tone44.wav"
ffmpeg -v error -y -f lavfi -i "sine=frequency=440:duration=1:sample_rate=22050" -ac 1 "$tmp/tone22.wav"
lame --quiet --noreplaygain -b 128 "$tmp/tone44.wav" cbr-info.mp3
lame --quiet --noreplaygain -b 128 -t "$tmp/tone44.wav" cbr-plain.mp3
lame --quiet --noreplaygain -V 2 "$tmp/tone44.wav" vbr-xing.mp3
lame --quiet --noreplaygain -V 2 -t "$tmp/tone44.wav" vbr-plain.mp3
lame --quiet --noreplaygain -m m -b 64 "$tmp/tone22.wav" mono22.mp3
ffmpeg -v error -y -f lavfi -i "testsrc=size=96x64:rate=1" -frames:v 1 cover-wide.jpg
ffmpeg -v error -y -f lavfi -i "testsrc=size=40x40:rate=1" -frames:v 1 cover-small.png
lame --quiet --noreplaygain -b 128 --id3v2-only --tt "Café" --ta "Björk" --tl "Début" --tn "3/12" \
  --ti cover-wide.jpg "$tmp/tone44.wav" tagged-v23.mp3
ffmpeg -v error -y -i cbr-info.mp3 -i cover-small.png -map 0:a -map 1:v -c copy -id3v2_version 4 \
  -metadata title="Ünïcødé" -metadata artist="Sigur Rós" -metadata album="Ágætis byrjun" -metadata track="7" \
  -metadata:s:v comment="Cover (front)" tagged-v24.mp3
lame --quiet --noreplaygain -b 128 --id3v1-only --tt "Old Tag" --ta "V1 Artist" --tl "V1 Album" --tn 5 \
  "$tmp/tone44.wav" tagged-v1.mp3
