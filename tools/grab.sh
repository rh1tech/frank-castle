#!/bin/bash
# Grab one frame from the HDMI capture card ("USB Video", 640x480@30).
# Usage: tools/grab.sh [out.png]
OUT=${1:-/tmp/frank-castle-grab.png}
ffmpeg -hide_banner -loglevel error -y -f avfoundation -pixel_format uyvy422 -framerate 30 -video_size 640x480 \
       -i "USB Video" -ss 0.5 -frames:v 1 "$OUT" && echo "$OUT"
