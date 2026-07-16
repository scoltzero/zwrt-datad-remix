#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/zwrt-datad-http-test.$$
PORT=${ZWRT_TEST_PORT:-19460}
BIN=$TMP/zwrt-datad-test
LOG=$TMP/ubus.log
PID=

cleanup() {
    [ -n "$PID" ] && kill "$PID" 2>/dev/null || true
    [ -n "$PID" ] && wait "$PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM
mkdir -p "$TMP"

cc -std=c11 -O0 -g -Wall -Wextra -Werror -Wno-unused-parameter \
   -D_GNU_SOURCE -I"$ROOT/include" "$ROOT"/src/*.c -o "$BIN"

: > "$LOG"
PATH="$ROOT/tests/fake-bin:$PATH" UBUS_LOG="$LOG" \
    "$BIN" -i 100 -b 127.0.0.1 -p "$PORT" >"$TMP/server.log" 2>&1 &
PID=$!

i=0
until curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || { cat "$TMP/server.log"; exit 1; }
    sleep 0.1
done

python3 - "$PORT" <<'PY'
import json
import sys
import urllib.error
import urllib.request

base = f"http://127.0.0.1:{sys.argv[1]}"

def request(path, method="GET"):
    req = urllib.request.Request(base + path, method=method)
    try:
        with urllib.request.urlopen(req, timeout=3) as res:
            return res.status, res.read().decode()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode()

status, body = request("/healthz")
assert status == 200 and body == "ok\n"

status, body = request("/state")
assert status == 200 and isinstance(json.loads(body), dict)

status, body = request("/modem/signal-metrics")
data = json.loads(body)
assert status == 200 and data["status"] == "ready"
assert data["cell_key"] == "43116896257"
assert data["lte"]["neighbors"] == [
    {"pci": "145", "earfcn": "1300", "band": "B3", "rsrp": "-101", "rsrq": "-14"}
]
assert data["nr"]["neighbors"][0] == {
    "pci": "654", "nrarfcn": "152650", "band": "n28", "rsrp": "-83", "rsrq": "-12"
}
assert len(data["nr"]["neighbors"]) == 2

status, body = request("/modem/control?scan=1", "POST")
control = json.loads(body)
assert status == 200 and control["ok"] is True and control["scan_requested"] is True
assert request("/modem/control")[0] == 405
assert request("/state", "POST")[0] == 405

assert json.loads(request("/modem/latest-signals")[1]) == {"lte": None, "nr": None}
assert json.loads(request("/modem/latest?kind=lte_ml1_raw")[1]) is None
assert json.loads(request("/modem/recent?kind=lte_ml1_raw&limit=5")[1]) == []
PY

sleep 0.2
grep -q 'zte_nwinfo_api nwinfo_scan_nbr' "$LOG"
[ "$(grep -c 'zte_nwinfo_api nwinfo_scan_nbr' "$LOG")" -eq 1 ]

curl -sN --max-time 1 "http://127.0.0.1:$PORT/events" > "$TMP/events" || [ "$?" -eq 28 ]
grep -q 'event: state' "$TMP/events"

kill "$PID"
wait "$PID" 2>/dev/null || true
PID=
EMPTY_PORT=$((PORT + 1))
PATH="$ROOT/tests/fake-bin:$PATH" UBUS_LOG="$LOG" UBUS_NEIGHBOR_MODE=empty \
    "$BIN" -i 100 -b 127.0.0.1 -p "$EMPTY_PORT" >"$TMP/server-empty.log" 2>&1 &
PID=$!
i=0
until curl -fsS "http://127.0.0.1:$EMPTY_PORT/healthz" >/dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || { cat "$TMP/server-empty.log"; exit 1; }
    sleep 0.1
done
curl -fsS "http://127.0.0.1:$EMPTY_PORT/modem/signal-metrics" > "$TMP/empty.json"
python3 - "$TMP/empty.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as fh:
    data = json.load(fh)
assert data["status"] == "empty"
assert data["lte"]["neighbors"] == []
assert data["nr"]["neighbors"] == []
PY

echo "HTTP-TEST-OK"
