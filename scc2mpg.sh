#!/bin/sh
# usage: scc2mpg.sh source_video captions.scc output.mpg [--tc-start 01:00:00;00] [other scc2m2v opts]
set -e
SRC="$1"; SCC="$2"; OUT="$3"; shift 3
TMP="${OUT%.*}.tmp.ts"

FILTERGRAPHS="fps=30000/1001,scale=1920:1080:flags=lanczos,setsar=1,format=yuv420p"

# for interlaced video, add yadif first to filtergraphs
if ffprobe -v error -select_streams v:0 -show_entries frame=interlaced_frame -of default=noprint_wrappers=1 "$SRC" | grep -q "interlaced_frame=1"; then
	FILTERGRAPHS="yadif,fps=30000/1001,scale=1920:1080:flags=lanczos,setsar=1,format=yuv420p"
	echo interlaced
fi

# conform picture to the target: 1920x1080, 29.97p, 4:2:0, raw -> scc2m2v (encodes MPEG-2 + injects CC)
ffmpeg -v error -i "$SRC" -an \
  -vf "$FILTERGRAPHS" \
  -f rawvideo - | ./scc2m2v "$SCC" "$TMP" "$@"
# mux with MP2 224k/48k audio into a real MPEG-2 program stream (video stream-copied, captions untouched).
#    Video timestamps come from the encoder (kept in the .ts; 90 kHz, exact 3003-tick spacing) so every picture has correct PTS/DTS.
ffmpeg -v error -y -i "$TMP" -i "$SRC" \
  -map 0:v -map 1:a:0 -c:v copy -c:a mp2 -b:a 224k -ar 48000 -ac 2 \
  -f vob -muxrate 16200000 "$OUT"
rm -f "$TMP"


