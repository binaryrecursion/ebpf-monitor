# eBPF Kernel Monitor

**Group 11 — Software Engineering Project**

A kernel-level system performance monitoring tool built with eBPF. It traces system calls, CPU scheduling, and process lifecycle events in real time — without modifying the Linux kernel — and displays aggregated metrics with adaptive anomaly detection on a live CLI dashboard.

---

## Team

| Name | Roll No |
|---|---|
| Anurag Kishor Patil | 24114015 |
| Prabhat Chandra Tiwari | 24114066 |
| Vaibhav Kumar | 24114103 |
| Samarth Maheshwari | 24114084 |
| Vishal Kumar Shaw | 24114105 |
| Rishabh Gupta | 24114077 |

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    Kernel Space                     │
│                                                     │
│  Tracepoints:                                       │
│  • sched_process_exec / sched_process_exit          │
│  • sys_enter/exit_openat, read, write, close        │
│  • sched_switch                                     │
│                    │                               │
│              BPF Maps (Hash)                        │
│          exec_start / syscall_start                 │
│              sched_start                            │
│                    │                               │
│             Ring Buffer (256 KB)                    │
└─────────────────────────────────────────────────────┘
                      │  events streamed via ring buffer
┌─────────────────────────────────────────────────────┐
│                   User Space                        │
│                                                     │
│  bootstrap.c   — main loop, ring buffer consumer    │
│  stats.c       — aggregation + EMA baseline         │
│  dashboard.c   — CLI rendering (6 sections)         │
└─────────────────────────────────────────────────────┘
```

---

## Dependencies

| Dependency | Purpose |
|---|---|
| Linux kernel ≥ 5.8 with eBPF support | Runtime environment |
| `clang` / LLVM | Compile BPF programs |
| `libbpf` | BPF skeleton, ring buffer, CO-RE |
| `bpftool` | Generate BPF skeleton header |
| `libelf`, `zlib` | Required by libbpf |
| `gcc` | Compile user-space code |

Install on Ubuntu/Debian:
```bash
sudo apt install clang llvm libbpf-dev linux-tools-common bpftool libelf-dev zlib1g-dev
```

---

## Build

```bash
cd src

# Step 1 — compile BPF program
clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
    -c bootstrap.bpf.c -o bootstrap.bpf.o

# Step 2 — generate skeleton
bpftool gen skeleton bootstrap.bpf.o > bootstrap.skel.h

# Step 3 — compile and link user-space
make

# Or do all steps at once
make clean && make
```

---

## Usage

```bash
# Run with default settings (trace everything)
sudo ./bootstrap

# Show help
sudo ./bootstrap --help

# Only report events longer than 5 ms
sudo ./bootstrap --min-dur 5

# Enable libbpf debug output
sudo ./bootstrap --verbose
```

**Keyboard controls while running:**

| Key | Action |
|---|---|
| `q` | Quit |
| `r` | Reset all stats and baseline |

---

## Dashboard Sections

The dashboard refreshes every 2 seconds and shows 6 sections:

### 1. Main Table
Per-process, per-event breakdown. Columns: `PROCESS`, `PID`, `EVENT`, `RATE/s`, `AVG(us)`, `MAX(us)`, `CTXSW`. Rows marked with `!` have an active anomaly.

Latency colouring: green < 10 µs · yellow < 100 µs · red ≥ 100 µs.

### 2. Event Summary
All events aggregated across all processes. Shows average latency, max latency, and total event count per event type.

### 3. Top Events
Event types ranked by total rate (events/second).

### 4. Slowest Events
Per-process events ranked by maximum observed latency.

### 5. Activity Graph
Horizontal bar chart of event types by aggregated rate. Each event type appears exactly once.

### 6. Adaptive Baseline — Anomaly Alerts
Events whose current average latency has deviated more than 50% from their exponential moving average baseline. Shows deviation percentage, baseline value, and current value.

---

## How Anomaly Detection Works

Each `(process, event)` pair maintains a running baseline using an **exponential moving average (EMA)**:

```
baseline = α × current_avg + (1 − α) × baseline
```

where `α = 0.2` (configurable via `BASELINE_ALPHA` in `stats.h`).

Deviation is computed as:

```
deviation = |current_avg − baseline| / baseline
```

If `deviation > 0.5` (50%), the entry is flagged as an anomaly. This threshold is configurable via `ANOMALY_THRESHOLD` in `stats.h`.

The first observation seeds the baseline. Subsequent observations update it. Pressing `r` resets all baselines.

---

## Monitored Events

| Event | Tracepoint | What is measured |
|---|---|---|
| `openat` | `sys_enter/exit_openat` | File open latency |
| `read` | `sys_enter/exit_read` | Read syscall latency |
| `write` | `sys_enter/exit_write` | Write syscall latency |
| `close` | `sys_enter/exit_close` | Close syscall latency |
| `sched` | `sched/sched_switch` | CPU scheduling delay (time process waited to be scheduled) |
| `lifecycle` | `sched_process_exit` | Process total runtime (exec → exit) |

---

## Project Structure

```
src/
├── bootstrap.bpf.c      BPF kernel programs (tracepoint handlers)
├── bootstrap.c          User-space main loop, libbpf wiring, CLI args
├── bootstrap.h          Shared event struct and event_type enum
├── stats.c              Aggregation, EMA baseline, anomaly detection
├── stats.h              syscall_stat struct, constants, API
├── dashboard.c          CLI rendering (6 dashboard sections)
├── dashboard.h          Color macros, latency_color, deviation_color
├── vmlinux.h            Kernel type definitions (CO-RE)
├── bootstrap.skel.h     Auto-generated BPF skeleton (do not edit)
└── Makefile
```

---

## Design Decisions

**CO-RE (Compile Once, Run Everywhere):** The BPF programs use `BPF_CORE_READ()` from `bpf_core_read.h` and `vmlinux.h` for kernel struct access. This means the binary works across kernel versions without recompilation.

**Ring buffer over perf buffer:** `BPF_MAP_TYPE_RINGBUF` is used instead of the older perf buffer API. It has lower overhead and guarantees ordering within a single CPU.

**EMA over simple average:** A simple running average would slowly incorporate all historical data equally. EMA weights recent observations more heavily, making the baseline adapt to genuine long-term shifts while still detecting short-term spikes.

**Per-(process, event) baseline:** Each unique `(comm, filename)` pair has its own independent baseline. This prevents a slow process from masking a fast one being anomalous.

---

## Known Limitations

- Process names are truncated to 15 characters (kernel `TASK_COMM_LEN` limit).
- If more than 256 unique `(process, event)` pairs are seen, new ones are silently dropped (`MAX_STATS` limit).
- The `ppid` field is populated for exec/exit events only; syscall and sched events do not carry a parent PID.
- Context switch count (`CTXSW`) is only incremented for `sched` events, not for voluntary yields.