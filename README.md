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
│  • sys_enter/exit_openat, read, write, close, mmap  │
│  • sched/sched_switch                               │
│                    │                                │
│     BPF Maps (Hash): exec_start, syscall_start,     │
│                      sched_start                    │
│     Rodata: min_duration_ns, target_pid,            │
│             target_comm  ← set from CLI at load     │
│                    │                                │
│             Ring Buffer (256 KB)                    │
└─────────────────────────────────────────────────────┘
                      │
┌─────────────────────────────────────────────────────┐
│                   User Space                        │
│                                                     │
│  bootstrap.c   — main loop, CLI args, CPU overhead  │
│  stats.c       — aggregation, EMA baseline, P95     │
│  dashboard.c   — CLI rendering (7 sections)         │
│  export.c      — JSON / CSV / anomaly log output    │
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
# Step 1 — compile BPF program
clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
    -c bootstrap.bpf.c -o bootstrap.bpf.o

# Step 2 — generate skeleton
bpftool gen skeleton bootstrap.bpf.o > bootstrap.skel.h

# Step 3 — compile and link
make

# Or all at once (auto-detects CPU architecture)
make clean && make
```

> **Note:** The Makefile auto-detects your CPU architecture (`x86_64`, `aarch64`, `arm`).
> You can override it with `make ARCH=arm64` if needed.

---

## Usage

```bash
# Trace everything
sudo ./bootstrap

# Trace only PID 1234
sudo ./bootstrap --pid 1234

# Trace only processes named "nginx"
sudo ./bootstrap --comm nginx

# Only report events longer than 5 ms
sudo ./bootstrap --min-dur 5

# Export to JSON and CSV on exit, log anomalies continuously
sudo ./bootstrap --export-json out.json --export-csv out.csv --log-anomalies anomalies.log

# Combine filters and export
sudo ./bootstrap --comm brave --export-json brave.json --log-anomalies brave_anomalies.log

# Show full help
sudo ./bootstrap --help
```

**Keyboard controls while running:**

| Key | Action |
|---|---|
| `q` | Quit (triggers final export) |
| `r` | Reset all stats, baselines, and CPU overhead counter |
| `e` | Export snapshot immediately (JSON + CSV) |

---

## Dashboard Sections

Refreshes every 2 seconds.

**Header** shows: runtime, **CPU overhead of this process** (measured after BPF attach — excludes libbpf startup cost), slot usage, and active filters (pid / comm / min-dur) if set.

### 1. Main Table
Sorted by rate descending. Columns: `PROCESS`, `PID`, `EVENT`, `RATE/s`, `AVG(us)`, `P95(us)`, `MAX(us)`, `CTXSW`, `EXECS`.
- Rows marked `!` have active anomalies.
- `P95(us)` shows `--` when fewer than 20 samples have been recorded (not statistically meaningful yet).
- Sched AVG/P95/MAX exclude sleep noise (>200ms off-CPU).

### 2. Event Summary
All events aggregated across processes — avg latency (valid samples only), max, total count, rate.
Rows marked `*` when `MAX >= 10× AVG`, indicating a single outlier is skewing the aggregate — check per-process rows in Section 1 for accurate data.

### 3. Top Events By Rate

### 4. Slowest Syscalls (peak latency)
Shows only genuine syscall events (`openat`, `read`, `write`, `close`, `mmap`).
`sched`, `exec`, and `lifecycle` are excluded here because their latency semantics are incomparable to per-syscall cost.

### 5. Activity Graph

### 6. Adaptive Baseline — Anomaly Alerts
Events deviating >50% from their EMA baseline. Also appended to `--log-anomalies` file every cycle.

### 7. Process Lifecycle
Per-process exec count, exit count, average lifetime.

---

## Filtering

Filters are applied **inside the kernel** via BPF rodata — processes not matching the filter produce zero kernel overhead. Active filters are shown in the dashboard header.

```
--pid PID      trace only this process ID
--comm NAME    trace only processes whose name matches NAME (exact, 15 chars max)
```

Both filters can be combined. When no filter is set, all processes are traced.

---

## Export Formats

### JSON (`--export-json FILE`)
Full snapshot of all tracked `(process, event)` pairs with all stats including P95. Written on exit and on `e` keypress.

```json
{
  "generated": "2026-04-13T14:22:01",
  "runtime_s": 120,
  "tracked_entries": 65,
  "total_events_dropped": 0,
  "stats": [
    {
      "process": "brave",
      "pid": 27787,
      "event": "write",
      "count": 5160,
      "rate_per_s": 43.0,
      "avg_latency_us": 2,
      "p95_latency_us": 8,
      "max_latency_us": 84,
      "ctx_switches": 0,
      "exec_count": 0,
      "baseline_latency_us": 2,
      "deviation_pct": 12.3,
      "is_anomaly": false
    }
  ]
}
```

### CSV (`--export-csv FILE`)
Same data as a flat CSV, including `p95_latency_us`. Suitable for pandas, Excel, or any analysis tool.

### Anomaly Log (`--log-anomalies FILE`)
Appended every 2-second render cycle. Session start and end markers are written automatically.

```
# eBPF Monitor session started 2026-04-13 14:22:01
# timestamp_s,process,pid,event,deviation_pct,baseline_us,current_avg_us
24,cpptools,19842,read,400.0,125028,625080
24,thermald,1203,sched,69.5,1960,3322
# session ended 2026-04-13 14:24:01
```

---

## How Anomaly Detection Works

Each `(process, event)` pair maintains an **EMA baseline**:

```
baseline = α × current_avg + (1 − α) × baseline    (α = 0.2)
deviation = |current_avg − baseline| / baseline
anomaly   = deviation > 0.5  (50%)
```

Configurable via `BASELINE_ALPHA` and `ANOMALY_THRESHOLD` in `stats.h`.

---

## P95 Latency

A simple 8-bucket histogram is maintained per `(process, event)` pair to compute approximate P95 latency without storing individual samples. P95 is displayed in the main table and exported to JSON/CSV.

Bucket edges (µs): 10, 50, 100, 500, 1000, 5000, 10000, ∞

P95 is shown as `--` when fewer than 20 samples exist.

---

## CPU Overhead Measurement

The monitor's own CPU usage is displayed in the header (`CPU: X.XX%`).

**Implementation:** After BPF programs are loaded and attached (the expensive part), a `getrusage(RUSAGE_SELF)` snapshot is taken as a baseline. Each render cycle computes:

```
cpu_overhead = (user_cpu_consumed + sys_cpu_consumed) / wall_elapsed × 100%
```

This correctly excludes libbpf's one-time startup cost and reflects only the steady-state monitoring overhead. In typical use on an idle desktop this reads < 1%. Under a busy workload generating thousands of events/second it typically stays under 3%.

---

## Sched Noise Filtering

Off-CPU times > 200ms are excluded from latency averages, P95, max, and anomaly baseline (sleeping ≠ scheduling delay). These samples are still counted for rate calculation. Configurable via `SCHED_NOISE_NS` in `stats.h`.

---

## Monitored Events

| Event | Tracepoint | What is measured |
|---|---|---|
| `openat` | `sys_enter/exit_openat` | File open latency |
| `read` | `sys_enter/exit_read` | Read syscall latency |
| `write` | `sys_enter/exit_write` | Write syscall latency |
| `close` | `sys_enter/exit_close` | Close syscall latency |
| `mmap` | `sys_enter/exit_mmap` | Memory map syscall latency |
| `sched` | `sched/sched_switch` | CPU scheduling delay (noise-filtered) |
| `exec` | `sched_process_exec` | Process exec events |
| `lifecycle` | `sched_process_exit` | Process total runtime |

---

## Project Structure

```
src/
├── bootstrap.bpf.c      BPF kernel programs + PID/comm filter rodata
├── bootstrap.c          Main loop, CLI, CPU overhead measurement
├── bootstrap.h          Shared event struct and event_type enum
├── stats.c              Aggregation, EMA baseline, histogram, P95, anomaly
├── stats.h              syscall_stat struct, tunables (MAX_STATS=512)
├── dashboard.c          CLI rendering (7 sections) + filter status header
├── dashboard.h          Color macros, latency/deviation color helpers
├── export.c             JSON snapshot, CSV snapshot, anomaly log
├── export.h             Export API
├── vmlinux.h            Kernel type definitions (CO-RE)
├── bootstrap.skel.h     Auto-generated BPF skeleton (do not edit)
└── Makefile             Auto-detects CPU architecture
```

---

## Design Decisions

**Kernel-side filtering via rodata:** `target_pid` and `target_comm` are passed to the BPF program as read-only constants set before load. The BPF verifier treats them as constants and can optimize branches away. Filtering in the kernel means unmatched processes generate zero ring buffer traffic and zero user-space CPU cost. Active filters are shown in the dashboard header.

**CO-RE portability:** Uses `BPF_CORE_READ()` and `vmlinux.h` — the binary runs across kernel versions without recompilation.

**Ring buffer over perf buffer:** Lower overhead, in-order delivery.

**EMA baseline per (process, event):** Adapts to genuine long-term shifts while detecting short-term spikes. Pressing `r` resets all baselines and the CPU overhead counter simultaneously.

**Sched noise filtering:** Off-CPU > 200ms = sleeping, not scheduling latency. Excluded from averages, P95, and baseline.

**Syscall-only "Slowest Events" table:** `sched`, `exec`, and `lifecycle` entries are excluded from the slowest-events ranking because their latency semantics are not comparable to per-syscall cost. A 199ms sched sample is a near-sleep event, not a slow syscall.

**Outlier detection in Event Summary:** When `MAX >= 10× AVG` for an aggregated event row, a `*` marker and footnote warn that a single outlier is skewing the aggregate mean. The per-process breakdown in the main table gives the accurate view.

**P95 via histogram:** An 8-bucket histogram per stat slot gives approximate P95 without storing individual samples. Memory overhead: 64 bytes per slot (8 × 8-byte counters).

**CPU overhead baseline after attach:** `getrusage` baseline is taken after `bootstrap_bpf__attach()` completes. This excludes the one-time libbpf/BPF verifier cost (~50–200ms of CPU) which would otherwise inflate the reported %.

---

## Known Limitations

- Process names truncated to 15 characters (kernel `TASK_COMM_LEN`).
- If > 512 unique `(process, event)` pairs are seen, new ones are dropped with a visible warning. Press `r` to reset.
- `--comm` filter is an exact match on the 15-char kernel comm string. It is not a substring match.
- `ppid` is captured for exec/exit events but is not currently displayed in the dashboard.
- P95 shows `--` until at least 20 samples are recorded for a given `(process, event)` pair.
- The histogram-based P95 is approximate; bucket resolution is coarse at the high end (5000–10000µs and ≥10000µs share single buckets).