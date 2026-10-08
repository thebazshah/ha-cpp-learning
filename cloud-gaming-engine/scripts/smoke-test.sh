#!/usr/bin/env bash
# End-to-end check of a running Cloud Gaming Engine:
#   REST API -> create a Tic-Tac-Toe session -> two bots play it over WebSockets.
#
# Start the server first (./build/cloud-gaming-engine), then run:
#   ./scripts/smoke-test.sh              # uses 127.0.0.1:8090
#   ./scripts/smoke-test.sh myhost 9000
set -uo pipefail
cd "$(dirname "$0")/.."

HOST="${1:-127.0.0.1}"
PORT="${2:-8090}"
BASE="http://$HOST:$PORT"
failures=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; failures=$((failures + 1)); }

echo "Smoke test against $BASE"

curl -fsS "$BASE/api/health" | grep -q '"status":"ok"' && pass "GET /api/health" || fail "GET /api/health"

ready="$(curl -fsS "$BASE/api/games?status=ready" | python3 -c 'import json,sys; print(" ".join(g["id"] for g in json.load(sys.stdin)["games"]))')"
if [[ " $ready " == *" tictactoe "* ]]; then pass "tictactoe is ready (ready games: $ready)"; else fail "tictactoe is not ready (ready games: '$ready')"; fi

session="$(curl -fsS -X POST "$BASE/api/games/tictactoe/sessions" | python3 -c 'import json,sys; print(json.load(sys.stdin)["session"]["id"])')"
[ -n "$session" ] && pass "created session $session" || { fail "create session"; exit 1; }

curl -fsS "$BASE/play/$session" | grep -q "Alpine" && pass "GET /play/$session serves the web page" || fail "web page"

# Two bots join and click around for 8 seconds; each must decode frames without errors.
./build/cge-bot --host "$HOST" --port "$PORT" --session "$session" --name BotX --seconds 8 --click-ms 250 --quiet > /tmp/cge-botx.log 2>&1 &
botX=$!
./build/cge-bot --host "$HOST" --port "$PORT" --session "$session" --name BotO --seconds 8 --click-ms 250 --quiet > /tmp/cge-boto.log 2>&1 &
botO=$!
wait $botX && pass "bot X played without decode errors" || fail "bot X (see /tmp/cge-botx.log)"
wait $botO && pass "bot O played without decode errors" || fail "bot O (see /tmp/cge-boto.log)"

moves="$(curl -fsS "$BASE/api/sessions/$session" | python3 -c 'import json,sys; m={x["name"]: x["value"] for x in json.load(sys.stdin)["gameMetrics"]}; print(m.get("Total moves", "0"))')"
[ "${moves:-0}" -gt 0 ] && pass "the game received input ($moves moves played)" || fail "no moves were played"

curl -fsS -X DELETE "$BASE/api/sessions/$session" >/dev/null && pass "DELETE session" || fail "DELETE session"

echo
grep -E "input -> frame|round trip" /tmp/cge-botx.log | sed 's/^/  BotX /'
echo
if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "$failures check(s) failed."; fi
exit "$failures"
