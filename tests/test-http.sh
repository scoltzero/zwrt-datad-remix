#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/zwrt-datad-http-test.$$
PORT=${ZWRT_TEST_PORT:-19460}
BIN=$TMP/zwrt-datad-test
LOG=$TMP/ubus.log
DATA=$TMP/data
SIM_FILE=$TMP/sim.json
TRAFFIC_FILE=$TMP/traffic.json
PID=

cleanup() {
    [ -n "$PID" ] && kill "$PID" 2>/dev/null || true
    [ -n "$PID" ] && wait "$PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM
mkdir -p "$TMP" "$DATA"

printf '%s\n' '{"sim_iccid":"8986000000000012345","current_sim_slot":"1","sim_imsi":"460001234567890","sim_states":"ready","Operator":"CUCC"}' >"$SIM_FILE"
printf '%s\n' '{"real_rx_speed":0,"real_tx_speed":0,"real_rx_bytes":1000,"real_tx_bytes":2000,"real_time":10}' >"$TRAFFIC_FILE"

cc -std=c11 -O0 -g -Wall -Wextra -Werror -Wno-unused-parameter \
   -Wno-format-truncation \
   -D_GNU_SOURCE -I"$ROOT/include" "$ROOT"/src/*.c -o "$BIN"

: > "$LOG"
PATH="$ROOT/tests/fake-bin:$PATH" UBUS_LOG="$LOG" \
    UBUS_SIM_FILE="$SIM_FILE" UBUS_TRAFFIC_FILE="$TRAFFIC_FILE" \
    ZWRT_DATAD_DATA_DIR="$DATA" \
    "$BIN" -i 100 -b 127.0.0.1 -p "$PORT" >"$TMP/server.log" 2>&1 &
PID=$!

i=0
until curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || { cat "$TMP/server.log"; exit 1; }
    sleep 0.1
done

printf '%s\n' '{"real_rx_speed":100,"real_tx_speed":50,"real_rx_bytes":5000,"real_tx_bytes":3000,"real_time":20}' >"$TRAFFIC_FILE"
sleep 0.4

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
state = json.loads(body)
assert status == 200 and isinstance(state, dict)
assert state["timezone"]["label"] == "UTC+08:00"
assert state["sim_traffic"]["available"] is True
assert len(state["sim_traffic"]["sims"]) == 1
sim = state["sim_traffic"]["sims"][0]
assert sim["slot"] == 1 and sim["iccid_tail"] == "2345"
assert sim["today_bytes"] == 5000
sim_id = sim["id"]

status, body = request("/settings/timezone?offset_minutes=-210&dst_minutes=0", "POST")
tz = json.loads(body)
assert status == 200 and tz["label"] == "UTC-03:30"

status, body = request("/settings/timezone?offset_minutes=330&dst_minutes=0", "POST")
tz = json.loads(body)
assert status == 200 and tz["label"] == "UTC+05:30"

status, body = request(
    f"/sim-traffic/config?sim_id={sim_id}&enabled=1&allowance_bytes=1000000&reset_day=15",
    "POST",
)
plan = json.loads(body)
assert status == 200 and plan["ok"] is True and plan["reset_day"] == 15

sim = json.loads(request("/sim-traffic")[1])["sims"][0]
assert sim["package_enabled"] is True
assert sim["allowance_bytes"] == 1000000
assert sim["remaining_bytes"] == 995000

status, body = request("/chart-metrics")
chart = json.loads(body)
assert status == 200
assert set(chart) == {
    "cpu_usage", "cpu_temp", "mem_used_pct", "rx_speed", "tx_speed",
    "battery_temp", "bat_uv", "bat_ua"
}
assert all(isinstance(value, int) for value in chart.values())

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
assert request("/chart-metrics", "POST")[0] == 405
assert request("/settings/timezone?offset_minutes=341&dst_minutes=0", "POST")[0] == 400
assert request("/sim-traffic/config")[0] == 405
assert request(
    f"/sim-traffic/config?sim_id={sim_id}&enabled=1&allowance_bytes=-1&reset_day=1",
    "POST",
)[0] == 400

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

printf '%s\n' '{"real_rx_speed":100,"real_tx_speed":50,"real_rx_bytes":9000,"real_tx_bytes":5000,"real_time":30}' >"$TRAFFIC_FILE"
RESTART_PORT=$((PORT + 1))
PATH="$ROOT/tests/fake-bin:$PATH" UBUS_LOG="$LOG" \
    UBUS_SIM_FILE="$SIM_FILE" UBUS_TRAFFIC_FILE="$TRAFFIC_FILE" \
    ZWRT_DATAD_DATA_DIR="$DATA" \
    "$BIN" -i 100 -b 127.0.0.1 -p "$RESTART_PORT" >"$TMP/server-restart.log" 2>&1 &
PID=$!
i=0
until curl -fsS "http://127.0.0.1:$RESTART_PORT/healthz" >/dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || { cat "$TMP/server-restart.log"; exit 1; }
    sleep 0.1
done
curl -fsS "http://127.0.0.1:$RESTART_PORT/state" > "$TMP/restart.json"
python3 - "$TMP/restart.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as fh:
    data = json.load(fh)
assert data["timezone"]["label"] == "UTC+05:30"
sim = data["sim_traffic"]["sims"][0]
assert sim["today_bytes"] == 11000
assert sim["allowance_bytes"] == 1000000
assert sim["remaining_bytes"] == 989000
PY

kill "$PID"
wait "$PID" 2>/dev/null || true
PID=
EMPTY_PORT=$((PORT + 2))
PATH="$ROOT/tests/fake-bin:$PATH" UBUS_LOG="$LOG" UBUS_NEIGHBOR_MODE=empty \
    UBUS_SIM_FILE="$SIM_FILE" UBUS_TRAFFIC_FILE="$TRAFFIC_FILE" \
    ZWRT_DATAD_DATA_DIR="$DATA" \
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
