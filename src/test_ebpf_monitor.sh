#!/bin/bash

PASS=0; FAIL=0; WARN=0
BIN=./bootstrap

green(){ echo -e "\033[32m[PASS]\033[0m $1"; PASS=$((PASS+1)); }
red(){ echo -e "\033[31m[FAIL]\033[0m $1"; FAIL=$((FAIL+1)); }
yellow(){ echo -e "\033[33m[WARN]\033[0m $1"; WARN=$((WARN+1)); }
section(){ echo -e "\n\033[1m=== $1 ===\033[0m"; }

# ── Prerequisites ─────────────────────────
section "Prerequisites"

if [ -f "$BIN" ]; then
    green "Binary exists"
else
    red "Binary not found"
    exit 1
fi

if [ "$(id -u)" = "0" ]; then
    green "Running as root"
else
    red "Run using sudo"
    exit 1
fi

if uname -r | grep -qE '^([5-9]\.|[1-9][0-9]+\.)'; then
    green "Kernel OK"
else
    yellow "Kernel might be old"
fi

# ── Test 1 ─────────────────────────
section "Test 1: Startup"

timeout 5 $BIN > /tmp/t1.txt 2>&1 &
PID=$!
sleep 2
kill -INT $PID 2>/dev/null
wait $PID 2>/dev/null

if grep -q "eBPF Kernel Monitor" /tmp/t1.txt; then
    green "Startup works"
else
    red "Startup failed"
fi

# ── Test 2 ─────────────────────────
section "Test 2: Dashboard Sections"

timeout 8 $BIN > /tmp/t2.txt 2>&1 &
PID=$!
sleep 1

for i in $(seq 1 20); do cat /etc/hostname > /dev/null; done

sleep 3
kill -INT $PID 2>/dev/null
wait $PID 2>/dev/null

for s in "EVENT SUMMARY" "TOP EVENTS" "ACTIVITY" "PROCESS"; do
    if grep -q "$s" /tmp/t2.txt; then
        green "$s visible"
    else
        red "$s missing"
    fi
done

# ── Test 3 ─────────────────────────
section "Test 3: Syscall Load"

timeout 6 $BIN > /tmp/t3.txt 2>&1 &
PID=$!

yes > /dev/null &
LOAD=$!

sleep 4
kill $LOAD 2>/dev/null
kill -INT $PID 2>/dev/null
wait $PID 2>/dev/null

if grep -q "write" /tmp/t3.txt; then
    green "Write load detected"
else
    red "Write not detected"
fi

# ── Test 4 ─────────────────────────
section "Test 4: Read/Open/Close"

timeout 6 $BIN > /tmp/t4.txt 2>&1 &
PID=$!

for i in $(seq 1 30); do cat /etc/passwd > /dev/null; done

sleep 3
kill -INT $PID 2>/dev/null
wait $PID 2>/dev/null

if grep -q "read" /tmp/t4.txt; then green "read OK"; else red "read missing"; fi
if grep -q "openat" /tmp/t4.txt; then green "openat OK"; else red "openat missing"; fi
if grep -q "close" /tmp/t4.txt; then green "close OK"; else red "close missing"; fi

# ── Test 5 ─────────────────────────
section "Test 5: Scheduler Stress"

timeout 6 $BIN > /tmp/t5.txt 2>&1 &
PID=$!

yes > /dev/null &
yes > /dev/null &
sleep 4

killall yes 2>/dev/null
kill -INT $PID 2>/dev/null
wait $PID 2>/dev/null

if grep -q "sched" /tmp/t5.txt; then
    green "Scheduler activity detected"
else
    red "No scheduler activity"
fi

# ── Test 6 ─────────────────────────
section "Test 6: Lifecycle"

timeout 6 $BIN > /tmp/t6.txt 2>&1 &
PID=$!

for i in $(seq 1 50); do ls > /dev/null; done

sleep 2
kill -INT $PID 2>/dev/null
wait $PID 2>/dev/null

if grep -q "exec" /tmp/t6.txt || grep -q "lifecycle" /tmp/t6.txt; then
    green "Lifecycle tracked"
else
    yellow "Lifecycle weak (not critical)"
fi

# ── Summary ─────────────────────────
echo ""
echo "===================================="
echo "PASS: $PASS   FAIL: $FAIL   WARN: $WARN"
echo "===================================="

if [ $FAIL -eq 0 ]; then
    echo -e "\033[32mALL TESTS PASSED\033[0m"
else
    echo -e "\033[31mSome tests failed\033[0m"
fi
