#!/usr/bin/env bash
# End-to-end check of a running server: REST API, HLS and (if ffmpeg is
# installed) RTSP over TCP. Start the server first, then run:
#
#   ./scripts/smoke-test.sh                 # uses localhost:8080 and :8554
#   ./scripts/smoke-test.sh myhost 8080 8554
#
# It picks the first file of the videos/ folder (or audios/ if there are no videos).
set -uo pipefail

HOST="${1:-127.0.0.1}"
HTTP_PORT="${2:-8080}"
RTSP_PORT="${3:-8554}"
BASE="http://$HOST:$HTTP_PORT"
failures=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; failures=$((failures + 1)); }

echo "Smoke test against $BASE"

# 1. Health
if curl -fsS "$BASE/api/health" | grep -q '"status":"ok"'; then pass "GET /api/health"; else fail "GET /api/health"; fi

# 2. Library
library="$(curl -fsS "$BASE/api/media")" || { fail "GET /api/media"; exit 1; }
pass "GET /api/media"
read -r folder name < <(python3 -c '
import json, sys
data = json.loads(sys.argv[1])
items = data["videos"] or data["audios"]
playable = [i for i in items if i["playable"]]
print((playable[0]["folder"] + " " + playable[0]["urls"]["direct"].split("/")[-1]) if playable else "")
' "$library")
if [ -z "${name:-}" ]; then
  echo "No playable media found. Add files to videos/ or audios/ (or run scripts/generate-test-media.sh)."
  exit 1
fi
echo "  using $folder/$name"

# 3. Prepare and wait until the stream is ready
curl -fsS -X POST "$BASE/api/media/$folder/$name/prepare" >/dev/null && pass "POST prepare" || fail "POST prepare"
for _ in $(seq 1 60); do
  ready="$(curl -fsS "$BASE/api/media/$folder/$name" | python3 -c 'import json,sys; s=json.load(sys.stdin)["stream"]; print(bool(s and s["ready"]))')"
  [ "$ready" = "True" ] && break
  sleep 1
done
[ "$ready" = "True" ] && pass "stream ready" || fail "stream ready (timed out)"

# 4. HLS
master="$(curl -fsS "$BASE/hls/$folder/$name/master.m3u8")"
if echo "$master" | grep -q "EXT-X-STREAM-INF"; then
  pass "master playlist ($(echo "$master" | grep -c EXT-X-STREAM-INF) renditions)"
else
  fail "master playlist"
fi
if curl -fsS "$BASE/hls/$folder/$name/r0/index.m3u8" | grep -q "seg_00000.ts"; then pass "media playlist"; else fail "media playlist"; fi
code="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/hls/$folder/$name/r0/seg_00000.ts")"
[ "$code" = "200" ] && pass "first segment" || fail "first segment (HTTP $code)"

# 5. Direct file with a Range request
code="$(curl -s -o /dev/null -w '%{http_code}' -H 'Range: bytes=0-1023' "$BASE/media/$folder/$name")"
[ "$code" = "206" ] && pass "direct file range request" || fail "direct file range request (HTTP $code)"

# 6. RTSP (needs ffmpeg): play 5 seconds over RTP/TCP
if command -v ffmpeg >/dev/null 2>&1; then
  if ffmpeg -hide_banner -loglevel error -rtsp_transport tcp -i "rtsp://$HOST:$RTSP_PORT/$folder/$name" -t 5 -f null - ; then
    pass "RTSP playback (RTP over TCP)"
  else
    fail "RTSP playback"
  fi
else
  echo "  SKIP  RTSP (ffmpeg not installed)"
fi

echo
if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "$failures check(s) failed."; fi
exit "$failures"
