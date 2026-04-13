# eBPF Kernel Monitor

**Group 11 — Software Engineering Project**

A kernel-level system performance monitoring tool built with eBPF. Traces system calls, CPU scheduling, process exec/exit events in real time — without modifying the Linux kernel — and displays aggregated metrics with adaptive anomaly detection on a live CLI dashboard.

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
│  • sched/sched_switch                               │
│                    │                                │
│              BPF Maps (Hash)                        │
│       exec_start / syscall_start / sched_start      │
│                    │                                │
│             Ring Buffer (256 KB)                    │
└─────────────────────────────────────────────────────┘
                      │  events streamed via ring buffer
┌─────────────────────────────────────────────────────┐
│                   User Space                        │
│                                                     │
│  bootstrap.c   — main loop, ring buffer consumer    │
│  stats.c       — aggregation + EMA baseline         │
│  dashboard.c   — CLI rendering (7 sections)         │
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
| `r` | Reset all stats and baselines (takes effect immediately) |

---

## Dashboard Sections

The dashboard refreshes every 2 seconds and shows 7 sections:

### 1. Main Table
Per-process, per-event breakdown sorted by rate (highest first). Up to 12 rows shown. Columns: `PROCESS`, `PID`, `EVENT`, `RATE/s`, `AVG(us)`, `MAX(us)`, `CTXSW`, `EXECS`. Rows marked with `!` have an active anomaly.

- Latency colouring: green < 10 µs · yellow < 100 µs · red ≥ 100 µs
- `sched` AVG/MAX exclude off-CPU samples longer than 200 ms (sleep noise), so only true scheduling latency is averaged
- A warning is shown if the slot table (default 512 entries) fills up

### 2. Event Summary
All events aggregated across all processes. Shows average latency, max latency, total event count, and rate per event type.

### 3. Top Events By Rate
Event types ranked by total events/second.

### 4. Slowest Events
Per-process events ranked by maximum observed latency.

### 5. Activity Graph
Horizontal bar chart of event types by aggregated rate.

### 6. Adaptive Baseline — Anomaly Alerts
Events whose current average latency has deviated more than 50% from their EMA baseline. Shows deviation %, baseline value, and current value.

### 7. Process Lifecycle
Summary of `exec` and `lifecycle` (exit) events per process, with average process lifetime.

---

## How Anomaly Detection Works

Each `(process, event)` pair maintains a running baseline using an **exponential moving average (EMA)**:

```
baseline = α × current_avg + (1 − α) × baseline
```

where `α = 0.2` (`BASELINE_ALPHA` in `stats.h`).

Deviation:

```
deviation = |current_avg − baseline| / baseline
```

If `deviation > 0.5` (50%), the entry is flagged. Threshold configurable via `ANOMALY_THRESHOLD`.

---

## Sched Noise Filtering

The `sched_switch` tracepoint measures time a process spent **off-CPU** between two scheduling events. When a process is sleeping (e.g. `select`, `epoll_wait`), this time can be seconds long — this is not scheduling latency. Events with off-CPU time > 200 ms (`SCHED_NOISE_NS`) are counted toward the rate but excluded from the latency average and baseline. The threshold is configurable in `stats.h`.

---

## Monitored Events

| Event | Tracepoint | What is measured |
|---|---|---|
| `openat` | `sys_enter/exit_openat` | File open latency |
| `read` | `sys_enter/exit_read` | Read syscall latency |
| `write` | `sys_enter/exit_write` | Write syscall latency |
| `close` | `sys_enter/exit_close` | Close syscall latency |
| `sched` | `sched/sched_switch` | CPU scheduling delay (time process waited off-CPU, noise-filtered) |
| `exec` | `sched_process_exec` | Process exec events (count only) |
| `lifecycle` | `sched_process_exit` | Process total runtime (exec → exit) |

---

## Project Structure

```
src/
├── bootstrap.bpf.c      BPF kernel programs (tracepoint handlers)
├── bootstrap.c          User-space main loop, libbpf wiring, CLI args
├── bootstrap.h          Shared event struct and event_type enum
├── stats.c              Aggregation, EMA baseline, anomaly detection, noise filtering
├── stats.h              syscall_stat struct, constants (MAX_STATS=512), API
├── dashboard.c          CLI rendering (7 dashboard sections)
├── dashboard.h          Color macros, latency_color, deviation_color
├── vmlinux.h            Kernel type definitions (CO-RE)
├── bootstrap.skel.h     Auto-generated BPF skeleton (do not edit)
└── Makefile
```

---

## Design Decisions

**CO-RE (Compile Once, Run Everywhere):** Uses `BPF_CORE_READ()` and `vmlinux.h` for portability across kernel versions without recompilation.

**Ring buffer over perf buffer:** `BPF_MAP_TYPE_RINGBUF` gives lower overhead and ordering guarantees.

**EMA over simple average:** Weights recent observations more heavily, adapting to genuine long-term shifts while detecting short-term spikes.

**Per-(process, event) baseline:** Each unique `(comm, filename)` pair has its own independent baseline.

**Sched noise filtering:** Off-CPU times > 200 ms are excluded from latency averages. A sleeping process is not experiencing scheduling latency — it intentionally yielded the CPU.

**MAX_STATS = 512:** Doubled from 256 to reduce slot exhaustion in busy systems. When full, new slots are dropped and a visible warning is shown in the header and main table.

**Exec tracking:** `EVENT_EXEC` events are now counted in a dedicated `exec` slot, populating the `EXECS` column and the Process Lifecycle section.

---

## Known Limitations

- Process names truncated to 15 characters (kernel `TASK_COMM_LEN`).
- If more than 512 unique `(process, event)` pairs are observed, new ones are dropped with a visible warning. Press `r` to reset.
- `ppid` is populated for exec/exit events only.
- `CTXSW` counts scheduling events; it is not the kernel's voluntary context switch counter.