#!/bin/bash

BIN=./bootstrap

echo "=== QUICK FUNCTIONAL TESTS ==="

# ---------- TEST 1: JSON ----------
echo -e "\n[TEST 1] JSON export"

$BIN --export-json /tmp/out.json &
PID=$!
sleep 6
kill -INT $PID 2>/dev/null
wait $PID 2>/dev/null

python3 - <<EOF
import json
try:
    d = json.load(open('/tmp/out.json'))
    print("entries:", len(d.get('stats', [])))
    if d.get('stats'):
        s = d['stats'][0]
        print("p95:", s.get('p95_latency_us'))
        print("deviation:", s.get('deviation_pct'))
        print("anomaly:", s.get('is_anomaly'))
    print("[PASS] JSON valid")
except Exception as e:
    print("[FAIL] JSON error:", e)
EOF

# ---------- TEST 2: CSV ----------
echo -e "\n[TEST 2] CSV export"

$BIN --export-csv /tmp/out.csv &
PID=$!
sleep 6
kill -INT $PID 2>/dev/null
wait $PID 2>/dev/null

if [ -f /tmp/out.csv ]; then
    echo "[PASS] CSV file exists"
    head -2 /tmp/out.csv
else
    echo "[FAIL] CSV not created"
fi

# ---------- TEST 3: ANOMALY ----------
echo -e "\n[TEST 3] Anomaly logging"

$BIN --log-anomalies /tmp/anom.log &
PID=$!

yes > /dev/null &
LOAD=$!

sleep 6
kill $LOAD 2>/dev/null
kill -INT $PID 2>/dev/null
wait $PID 2>/dev/null

if [ -f /tmp/anom.log ]; then
    echo "[PASS] Log created"
    echo "---- LOG ----"
    cat /tmp/anom.log
else
    echo "[FAIL] Log missing"
fi

# ---------- TEST 4: FILTER ----------
echo -e "\n[TEST 4] comm filter (cat)"

$BIN --comm cat > /tmp/filter.txt &
PID=$!

for i in $(seq 1 20); do cat /etc/hostname > /dev/null; done

sleep 4
kill -INT $PID 2>/dev/null
wait $PID 2>/dev/null

echo "---- FILTER OUTPUT (should mostly contain 'cat') ----"
grep cat /tmp/filter.txt | head

if grep -q -v "cat" /tmp/filter.txt; then
    echo "[WARN] Non-cat entries found"
else
    echo "[PASS] Filter working clean"
fi

echo -e "\n=== DONE ==="
