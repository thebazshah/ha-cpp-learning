#!/usr/bin/env bash
# Creates a few small test files so you can try the server without your own media.
#
#   ./scripts/generate-test-media.sh
#
# It writes:
#   videos/test-1080p.mp4       20 s, 1920x1080, H.264 + AAC (landscape)
#   videos/test-portrait.mp4    10 s,  720x1280, H.264 + AAC (phone-style portrait video)
#   audios/test-tone.mp3        20 s, MP3 sine tone
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p videos audios

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg is not installed (macOS: brew install ffmpeg, Ubuntu: sudo apt install ffmpeg)" >&2
  exit 1
fi

echo "Creating videos/test-1080p.mp4 ..."
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc2=size=1920x1080:rate=30" -f lavfi -i "sine=frequency=440:sample_rate=48000" \
  -t 20 -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -shortest videos/test-1080p.mp4

echo "Creating videos/test-portrait.mp4 ..."
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc=size=720x1280:rate=25" -f lavfi -i "sine=frequency=660:sample_rate=44100" \
  -t 10 -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -shortest videos/test-portrait.mp4

echo "Creating audios/test-tone.mp3 ..."
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "sine=frequency=330:sample_rate=44100" -t 20 -c:a libmp3lame -b:a 192k audios/test-tone.mp3

echo "Done. Refresh the web page (or press the rescan button) to see the new files."
