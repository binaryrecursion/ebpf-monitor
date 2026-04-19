# eBPF Kernel Monitor

**CSC 206 Software Engineering Project — Group 11**  
**Submitted: April 21, 2026**

---

## Project Information

### Team Members

| Name | Roll No. | Email | Mobile | Individual Contribution |
|------|----------|-------|--------|------------------------|
| Anurag Kishor Patil | 24114015 | anurag.patil@example.com | +91-XXXXXXXXXX | BPF Kernel Programming, System Call Tracing |
| Prabhat Chandra Tiwari | 24114066 | prabhat.tiwari@example.com | +91-XXXXXXXXXX | Dashboard UI, Terminal Rendering, Layout Design |
| Vaibhav Kumar | 24114103 | vaibhav.kumar@example.com | +91-XXXXXXXXXX | Statistics Aggregation, P95 Histogram, Anomaly Detection |
| Samarth Maheshwari | 24114084 | samarth.maheshwari@example.com | +91-XXXXXXXXXX | Export Modules (JSON/CSV), Data Serialization |
| Vishal Kumar Shaw | 24114105 | vishal.shaw@example.com | +91-XXXXXXXXXX | Testing, Documentation, Build System |
| Rishabh Gupta | 24114077 | rishabh.gupta@example.com | +91-XXXXXXXXXX | Integration, Code Review, Performance Optimization |

> **Note:** Please update email addresses and mobile numbers in the table above before final submission.

---

## Table of Contents
1. [Prerequisites & Dependencies](#prerequisites--dependencies)
2. [Installation & Setup](#installation--setup)
3. [Build Instructions](#build-instructions)
4. [Usage Guide](#usage-guide)
5. [Configuration Options](#configuration-options)
6. [Overview](#overview)
7. [Project Motivation](#project-motivation)
8. [Features](#features)
9. [System Architecture](#system-architecture)
10. [Dashboard Interface](#dashboard-interface)
11. [Export Formats](#export-formats)
12. [Technical Implementation](#technical-implementation)
13. [Project Structure](#project-structure)
14. [Troubleshooting](#troubleshooting)
15. [Known Limitations](#known-limitations)
16. [Future Enhancements](#future-enhancements)
17. [License](#license)
18. [Acknowledgments](#acknowledgments)

---

## Prerequisites & Dependencies

### System Requirements

| Requirement | Minimum Version | Purpose |
|-------------|----------------|---------|
| **Linux Kernel** | ≥ 5.8 | eBPF CO-RE support, ring buffer API |
| **CPU Architecture** | x86_64, ARM64, ARM | Auto-detected by Makefile |
| **Root/Sudo Access** | Required | Loading eBPF programs requires CAP_BPF capability |

### Software Dependencies

| Package | Purpose | Installation |
|---------|---------|--------------|
| **clang** | Compile BPF programs | `sudo apt install clang` |
| **llvm** | LLVM toolchain for BPF | `sudo apt install llvm` |
| **libbpf-dev** | BPF library (CO-RE, ring buffer) | `sudo apt install libbpf-dev` |
| **bpftool** | Generate BPF skeleton headers | `sudo apt install linux-tools-common` |
| **libelf-dev** | ELF file handling (required by libbpf) | `sudo apt install libelf-dev` |
| **zlib1g-dev** | Compression library (required by libbpf) | `sudo apt install zlib1g-dev` |
| **gcc** | Compile userspace C code | `sudo apt install gcc` |
| **make** | Build automation | `sudo apt install make` |

### Verification Commands

```bash
# Check kernel version
uname -r

# Verify eBPF support
sudo mount | grep bpf

# Check clang version (should be ≥ 10.0)
clang --version

# Verify bpftool availability
which bpftool
```

---

## Installation & Setup

### Step 1: Install Dependencies

**On Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install -y clang llvm libbpf-dev linux-tools-common \
    linux-tools-$(uname -r) bpftool libelf-dev zlib1g-dev gcc make
```

**On Fedora/RHEL:**
```bash
sudo dnf install -y clang llvm libbpf-devel bpftool elfutils-libelf-devel \
    zlib-devel gcc make kernel-devel
```

**On Arch Linux:**
```bash
sudo pacman -S clang llvm libbpf bpf elfutils zlib gcc make
```

### Step 2: Clone or Extract Project

```bash
# If from repository
git clone https://github.com/your-org/ebpf-monitor.git
cd ebpf-monitor/src

# If from zip file
unzip ebpf-monitor-main.zip
cd ebpf-monitor-main/src
```

### Step 3: Verify File Permissions

```bash
# Ensure source files are readable
chmod 644 *.c *.h
chmod 755 Makefile
```

---

## Build Instructions

### Quick Build (Recommended)

```bash
cd src/
make clean && make
```

The Makefile automatically:
- Detects your CPU architecture (x86, ARM64, ARM)
- Compiles the BPF program (`bootstrap.bpf.c`)
- Generates the BPF skeleton header (`bootstrap.skel.h`)
- Compiles all userspace components
- Links the final `bootstrap` executable

### Manual Build Steps (Advanced)

If you need to build manually or override settings:

```bash
cd src/

# Step 1: Compile BPF program
clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
    -c bootstrap.bpf.c -o bootstrap.bpf.o

# For ARM64 architecture
clang -O2 -g -target bpf -D__TARGET_ARCH_arm64 \
    -c bootstrap.bpf.c -o bootstrap.bpf.o

# Step 2: Generate BPF skeleton
bpftool gen skeleton bootstrap.bpf.o > bootstrap.skel.h

# Step 3: Compile userspace code
gcc -O2 -g -Wall -Wextra -std=c11 \
    -I/usr/include -I/usr/include/bpf \
    -c bootstrap.c stats.c dashboard.c export.c vscreen.c layout.c theme.c

# Step 4: Link executable
gcc -O2 -g -o bootstrap \
    bootstrap.o stats.o dashboard.o export.o vscreen.o layout.o theme.o \
    -L/usr/lib/x86_64-linux-gnu -lbpf -lelf -lz -lm
```

### Build Verification

```bash
# Check if binary was created
ls -lh bootstrap

# Verify it's a valid ELF executable
file bootstrap

# Expected output:
# bootstrap: ELF 64-bit LSB executable, x86-64, dynamically linked...
```

### Cleaning Build Artifacts

```bash
make clean
# Removes: *.o, *.skel.h, bootstrap executable
```

---

## Usage Guide

### Basic Usage

```bash
# Monitor all processes (requires root)
sudo ./bootstrap

# Exit the monitor
# Press 'q' key
```

### Filtering Options

#### Filter by Process ID
```bash
# Monitor only PID 1234
sudo ./bootstrap --pid 1234

# Common use case: Monitor a specific service
# Step 1: Find the PID
ps aux | grep nginx
# Step 2: Monitor it
sudo ./bootstrap --pid 12345
```

#### Filter by Process Name
```bash
# Monitor all processes named "nginx"
sudo ./bootstrap --comm nginx

# Monitor browser processes
sudo ./bootstrap --comm firefox

# Note: Process names are truncated to 15 characters by the kernel
```

#### Filter by Minimum Duration
```bash
# Only show events taking longer than 5 milliseconds
sudo ./bootstrap --min-dur 5

# Only show slow I/O operations (>10ms)
sudo ./bootstrap --min-dur 10
```

#### Combine Multiple Filters
```bash
# Monitor specific process with duration filter
sudo ./bootstrap --pid 1234 --min-dur 5

# Monitor process name with duration threshold
sudo ./bootstrap --comm nginx --min-dur 10
```

### Export Options

#### JSON Export
```bash
# Export full snapshot to JSON on exit
sudo ./bootstrap --export-json output.json

# Press 'q' to quit and trigger export
# Or press 'e' for immediate export without quitting
```

**Sample JSON Output:**
```json
{
  "generated": "2026-04-19T10:30:15",
  "runtime_s": 120,
  "tracked_entries": 65,
  "total_events_dropped": 0,
  "stats": [
    {
      "process": "nginx",
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

#### CSV Export
```bash
# Export to CSV format
sudo ./bootstrap --export-csv output.csv

# Compatible with Excel, pandas, R
```

**Sample CSV Output:**
```csv
process,pid,event,count,rate_per_s,avg_latency_us,p95_latency_us,max_latency_us,ctx_switches,exec_count,baseline_latency_us,deviation_pct,is_anomaly
nginx,27787,write,5160,43.0,2,8,84,0,0,2,12.3,false
```

#### Anomaly Logging
```bash
# Continuously log anomalies to file
sudo ./bootstrap --log-anomalies anomalies.log

# Anomaly log is appended every 2-second render cycle
```

**Sample Anomaly Log:**
```
# eBPF Monitor session started 2026-04-19 10:30:15
# timestamp_s,process,pid,event,deviation_pct,baseline_us,current_avg_us
24,cpptools,19842,read,400.0,125028,625080
24,thermald,1203,sched,69.5,1960,3322
48,nginx,27787,write,55.2,2,4
# session ended 2026-04-19 10:32:15
```

#### Combined Export Example
```bash
# Monitor nginx with all export formats
sudo ./bootstrap \
    --comm nginx \
    --export-json nginx_metrics.json \
    --export-csv nginx_metrics.csv \
    --log-anomalies nginx_anomalies.log
```

### Interactive Controls

While the monitor is running, use these keyboard commands:

| Key | Action | Description |
|-----|--------|-------------|
| **q** | Quit | Exit the monitor and trigger final export (if configured) |
| **r** | Reset | Clear all statistics, baselines, and CPU overhead counter |
| **e** | Export | Immediately write JSON/CSV snapshots without quitting |

### Advanced Usage Examples

#### Example 1: Debug Slow Database Queries
```bash
# Monitor PostgreSQL process and log slow operations
sudo ./bootstrap \
    --comm postgres \
    --min-dur 10 \
    --export-json postgres_slow.json \
    --log-anomalies postgres_anomalies.log
```

#### Example 2: Monitor Docker Container
```bash
# Step 1: Find container process
docker top my-container

# Step 2: Monitor by PID
sudo ./bootstrap --pid 12345 --export-json container_metrics.json
```

#### Example 3: Continuous Production Monitoring
```bash
# Run in background with output redirection
sudo ./bootstrap \
    --comm myapp \
    --log-anomalies /var/log/ebpf-anomalies.log \
    > /dev/null 2>&1 &

# Later: bring to foreground
fg
# Press 'q' to quit
```

---

## Configuration Options

### Command-Line Arguments

| Argument | Type | Default | Description |
|----------|------|---------|-------------|
| `--pid PID` | Integer | All processes | Filter by process ID |
| `--comm NAME` | String (15 chars) | All processes | Filter by process name (exact match) |
| `--min-dur MS` | Integer | 0 | Minimum event duration in milliseconds |
| `--export-json FILE` | String | None | JSON export file path |
| `--export-csv FILE` | String | None | CSV export file path |
| `--log-anomalies FILE` | String | None | Anomaly log file path |
| `--theme N` | Integer (0-1) | 0 | Color theme: 0=Dark, 1=Catppuccin Mocha |
| `--help` | Flag | — | Display help message |

### Compile-Time Tunables

Edit `src/stats.h` to adjust:

```c
// Maximum tracked (process, event) pairs
#define MAX_STATS 512

// EMA baseline smoothing factor (0.0 - 1.0)
#define BASELINE_ALPHA 0.2

// Anomaly detection threshold (deviation %)
#define ANOMALY_THRESHOLD 0.5  // 50%

// Sched noise filtering threshold
#define SCHED_NOISE_NS 200000000  // 200ms
```

After changing values:
```bash
make clean && make
```

### Ring Buffer Size

Edit `src/bootstrap.bpf.c`:

```c
// Ring buffer size (must be power of 2)
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);  // Default: 256KB
} events SEC(".maps");
```

For high-throughput systems, increase to `512 * 1024` or `1024 * 1024`.


---

## Overview

**eBPF Kernel Monitor** is a real-time, kernel-level system performance monitoring tool that leverages Extended Berkeley Packet Filter (eBPF) technology to trace system calls, CPU scheduling events, and process lifecycle operations without modifying the Linux kernel.

The tool provides:
- **Zero-overhead kernel monitoring** through eBPF tracepoints
- **Real-time performance metrics** with adaptive anomaly detection
- **Interactive CLI dashboard** with live updates every 2 seconds
- **Multiple export formats** (JSON, CSV, anomaly logs)
- **Intelligent filtering** applied directly in kernel space
- **Statistical analysis** including P95 latency histograms

### Key Innovation

Unlike traditional monitoring tools that rely on userspace polling or `/proc` filesystem parsing (which introduces significant overhead), our solution uses eBPF to attach directly to kernel tracepoints, capturing events at the source with minimal performance impact (typically < 3% CPU overhead even under heavy workloads).

---

## Project Motivation

Modern distributed systems and cloud-native applications require deep visibility into system behavior for:
- **Performance debugging** — identifying bottlenecks in I/O operations
- **Anomaly detection** — detecting unusual latency spikes or resource contention
- **Capacity planning** — understanding system call patterns and resource utilization
- **Security monitoring** — tracking process execution and file access patterns

Traditional monitoring approaches have limitations:
- **Kernel modules** require recompilation for each kernel version and can crash the system
- **System tap/DTrace** have steep learning curves and platform dependencies  
- **Userspace tools** (`strace`, `perf`) introduce 10-100× performance overhead

**eBPF** solves these problems by:
1. Running sandboxed programs in kernel space with safety guarantees
2. Using CO-RE (Compile Once - Run Everywhere) for kernel portability
3. Filtering data at the source before crossing kernel/userspace boundary
4. Providing sub-microsecond precision event capture

---

## Features

### Core Capabilities

- **Real-time Event Tracing**
  - System calls: `openat`, `read`, `write`, `close`, `mmap`
  - CPU scheduling: context switches and on/off-CPU time
  - Process lifecycle: `exec`, `exit` events with runtime tracking

- **Statistical Analysis**
  - Average latency per (process, event) pair
  - P95 latency via 8-bucket histogram
  - Maximum latency tracking
  - Event rate calculation (events/second)

- **Adaptive Anomaly Detection**
  - EMA (Exponential Moving Average) baseline per metric
  - Configurable deviation thresholds (default: 50%)
  - Real-time alerting on dashboard
  - Continuous anomaly logging to file

- **Kernel-space Filtering**
  - Filter by process ID (`--pid`)
  - Filter by process name (`--comm`)
  - Minimum duration threshold (`--min-dur`)
  - Filters compiled into BPF program for zero overhead

- **Interactive Dashboard**
  - 7-section layout with color-coded metrics
  - Live updates every 2 seconds
  - Keyboard controls (quit, reset, export)
  - CPU overhead measurement and display

- **Multiple Export Formats**
  - JSON: Full snapshot with all metrics
  - CSV: Flat format for analysis tools
  - Anomaly logs: Timestamped deviation records

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         KERNEL SPACE (eBPF)                         │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    TRACEPOINT HOOKS                         │   │
│  │  • sched_process_exec      (process execution)              │   │
│  │  • sched_process_exit      (process termination)            │   │
│  │  • sys_enter_openat        (file open entry)                │   │
│  │  • sys_exit_openat         (file open exit)                 │   │
│  │  • sys_enter/exit_read     (read syscall)                   │   │
│  │  • sys_enter/exit_write    (write syscall)                  │   │
│  │  • sys_enter/exit_close    (close syscall)                  │   │
│  │  • sys_enter/exit_mmap     (memory map syscall)             │   │
│  │  • sched/sched_switch      (CPU scheduling)                 │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              ↓                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                   BPF FILTERING LOGIC                       │   │
│  │  • PID filter (rodata: target_pid)                          │   │
│  │  • Process name filter (rodata: target_comm[16])            │   │
│  │  • Minimum duration filter (rodata: min_duration_ns)        │   │
│  │  • Early rejection — unmatched events never reach userspace │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              ↓                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    BPF HASH MAPS                            │   │
│  │  • exec_start[pid]       → timestamp of exec entry          │   │
│  │  • syscall_start[pid]    → timestamp of syscall entry       │   │
│  │  • sched_start[pid]      → timestamp of CPU schedule-in     │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              ↓                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │              RING BUFFER (256 KB)                           │   │
│  │  • Low-latency kernel → userspace event delivery            │   │
│  │  • In-order guarantee (vs perf buffer)                      │   │
│  │  • Per-CPU buffering for scalability                        │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
└────────────────────────────┬────────────────────────────────────────┘
                             │ libbpf ring_buffer__poll()
                             ↓
┌─────────────────────────────────────────────────────────────────────┐
│                         USER SPACE (C)                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────────┐  ┌──────────────────┐  ┌──────────────────┐   │
│  │  bootstrap.c    │  │    stats.c       │  │  dashboard.c     │   │
│  │                 │  │                  │  │                  │   │
│  │ • Main loop     │→ │ • Aggregation    │→ │ • 7-section UI   │   │
│  │ • CLI parsing   │  │ • EMA baseline   │  │ • Color coding   │   │
│  │ • Ring buffer   │  │ • P95 histogram  │  │ • Sorting        │   │
│  │   polling       │  │ • Anomaly detect │  │ • Refresh        │   │
│  │ • CPU overhead  │  │ • 512 slot limit │  │ • Keyboard input │   │
│  └─────────────────┘  └──────────────────┘  └──────────────────┘   │
│                             ↓                                       │
│                    ┌──────────────────┐                             │
│                    │    export.c      │                             │
│                    │                  │                             │
│                    │ • JSON snapshot  │                             │
│                    │ • CSV export     │                             │
│                    │ • Anomaly log    │                             │
│                    └──────────────────┘                             │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Data Flow

1. **Kernel Events** → eBPF programs attached to tracepoints capture events
2. **Filtering** → Filters applied via BPF rodata (constant propagation optimization)
3. **Timestamp Capture** → Entry/exit timestamps stored in BPF hash maps
4. **Ring Buffer** → Events submitted to 256KB ring buffer
5. **Userspace Processing** → Poll ring buffer, aggregate statistics
6. **Dashboard Rendering** → Update CLI every 2 seconds
7. **Export** → On-demand or on-exit data serialization


---

## Dashboard Interface

The dashboard refreshes every 2 seconds and consists of 7 sections:

### Header Section

```
┌─────────────────────────────────────────────────────────────────────┐
│ eBPF Monitor | Runtime: 02:34 | CPU: 1.23% | Slots: 45/512          │
│ Filters: pid=1234 comm=nginx min-dur=5ms                            │
└─────────────────────────────────────────────────────────────────────┘
```

- **Runtime**: Total elapsed time since startup
- **CPU**: Overhead of this monitoring process (measured after BPF attach)
- **Slots**: Number of tracked (process, event) pairs out of maximum (512)
- **Filters**: Active kernel-space filters

### Section 1: Main Statistics Table

```
PROCESS    PID     EVENT    RATE/s  AVG(us)  P95(us)  MAX(us)  CTXSW  EXECS
nginx      1234    write    43.0    2        8        84       0      0
nginx      1234    read     18.5    5        12       156      0      0
! postgres 5678    write    12.3    450      2100     5200     120    0
```

- Sorted by event rate (descending)
- **`!`** marker indicates active anomaly
- **P95(us)** shows `--` when fewer than 20 samples collected
- **CTXSW**: Context switches for `sched` events
- **EXECS**: Execution count for `exec` events

### Section 2: Event Summary (Aggregated)

```
EVENT     AVG(us)  MAX(us)   COUNT    RATE/s
write     45       5200      12480    104.0
read      23       1560      5640     47.0
* openat  125      45000     2340     19.5
```

- Aggregated across all processes
- **`*`** marker when MAX ≥ 10× AVG (outlier skewing average)

### Section 3: Top Events by Rate

```
1. nginx:write          43.0/s
2. postgres:read        25.3/s
3. redis:write          18.7/s
```

Top 10 highest rate events.

### Section 4: Slowest Syscalls

```
1. postgres:write       5200 us
2. nginx:openat         1560 us
3. redis:read           890 us
```

Only genuine syscalls (excludes `sched`, `exec`, `lifecycle`).

### Section 5: Activity Graph

```
┌─────────────────────────────────────────┐
│ ████████░░░░░░░░░░░░                     │ write
│ ████░░░░░░░░░░░░░░░░                     │ read
│ ██░░░░░░░░░░░░░░░░░░                     │ openat
└─────────────────────────────────────────┘
```

Visual representation of event rates.

### Section 6: Anomaly Alerts

```
PROCESS    PID     EVENT    DEVIATION  BASELINE  CURRENT
postgres   5678    write    +125%      450 us    1012 us
nginx      1234    read     +67%       5 us      8 us
```

Events with >50% deviation from EMA baseline.

### Section 7: Process Lifecycle

```
PROCESS    EXECS  EXITS  AVG_LIFETIME(ms)
nginx      5      4      12450
worker     120    120    250
```

Tracks process creation/termination.

### Color Coding

| Color | Meaning |
|-------|---------|
| **Green** | Low latency (< 1ms) |
| **Yellow** | Moderate latency (1-10ms) |
| **Red** | High latency (> 10ms) |
| **Cyan** | Anomaly marker |
| **White** | Normal text |

---

## Export Formats

### JSON Format

Full statistical snapshot with all metrics:

```json
{
  "generated": "2026-04-19T14:22:01",
  "runtime_s": 120,
  "tracked_entries": 65,
  "total_events_dropped": 0,
  "stats": [
    {
      "process": "nginx",
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

**Use Cases:**
- Post-processing with Python/pandas
- Integration with monitoring systems (Prometheus, Grafana)
- Time-series analysis
- Machine learning feature extraction

### CSV Format

Flat tabular format suitable for spreadsheet analysis:

```csv
process,pid,event,count,rate_per_s,avg_latency_us,p95_latency_us,max_latency_us,ctx_switches,exec_count,baseline_latency_us,deviation_pct,is_anomaly
nginx,27787,write,5160,43.0,2,8,84,0,0,2,12.3,false
postgres,5678,read,2340,19.5,125,450,5200,0,0,110,15.8,false
```

**Use Cases:**
- Import into Excel/Google Sheets
- Statistical analysis with R
- Quick visualization with plotting tools

### Anomaly Log Format

Continuous timestamped anomaly records:

```
# eBPF Monitor session started 2026-04-19 14:22:01
# timestamp_s,process,pid,event,deviation_pct,baseline_us,current_avg_us
24,cpptools,19842,read,400.0,125028,625080
24,thermald,1203,sched,69.5,1960,3322
48,nginx,1234,write,55.2,2,4
120,postgres,5678,read,125.0,125,281
# session ended 2026-04-19 14:24:01
```

**Features:**
- Append-only (doesn't overwrite previous sessions)
- Session markers for multi-run analysis
- Timestamped at 2-second intervals
- Immediate write on detection (no buffering)

**Use Cases:**
- Long-term anomaly trend analysis
- Alerting pipelines
- Historical performance investigations

---

## Technical Implementation

### eBPF Program Architecture

#### Tracepoint Attachments

```c
// Process lifecycle events
SEC("tp/sched/sched_process_exec")
int handle_exec(struct trace_event_raw_sched_process_exec *ctx)

SEC("tp/sched/sched_process_exit")  
int handle_exit(struct trace_event_raw_sched_process_template *ctx)

// Syscall tracing (entry/exit pairs)
SEC("tp/syscalls/sys_enter_openat")
int trace_enter_openat(struct trace_event_raw_sys_enter *ctx)

SEC("tp/syscalls/sys_exit_openat")
int trace_exit_openat(struct trace_event_raw_sys_exit *ctx)

// CPU scheduling
SEC("tp/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx)
```

#### Kernel-Space Filtering

Filters are applied as **BPF rodata constants** which the verifier can optimize:

```c
// Read-only data section - set from userspace before load
const volatile int target_pid = 0;
const volatile char target_comm[16] = "";
const volatile u64 min_duration_ns = 0;

// Filter logic (compiled out if filter is not set)
if (target_pid != 0 && pid != target_pid)
    return 0;  // No ring buffer event generated

if (target_comm[0] != '\0' && memcmp(comm, target_comm, 15) != 0)
    return 0;
```

This approach means:
- Unmatched processes generate **zero overhead**
- No ring buffer traffic for filtered events
- No userspace CPU cost for processing unwanted data

### CO-RE (Compile Once - Run Everywhere)

Uses BPF Type Format (BTF) and `vmlinux.h`:

```c
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>

// Portable kernel struct access
struct task_struct *task = (void *)bpf_get_current_task();
u32 pid = BPF_CORE_READ(task, pid);
```

Benefits:
- Binary runs on any kernel ≥ 5.8 without recompilation
- No kernel headers needed at runtime
- Automatic field offset relocation

### Ring Buffer vs. Perf Buffer

Why we chose ring buffer:

| Feature | Ring Buffer | Perf Buffer |
|---------|-------------|-------------|
| Ordering | In-order guarantee | No ordering guarantee |
| Memory efficiency | Single shared buffer | Per-CPU buffers |
| Data consistency | Always readable | Can have gaps |
| API simplicity | Single poll call | Complex event parsing |

### Anomaly Detection Algorithm

```c
// Exponential Moving Average baseline
baseline = BASELINE_ALPHA * current_avg + (1 - BASELINE_ALPHA) * baseline

// Deviation calculation  
deviation = |current_avg - baseline| / baseline

// Threshold check
is_anomaly = (deviation > ANOMALY_THRESHOLD)
```

**Tunable parameters** (`stats.h`):
- `BASELINE_ALPHA = 0.2` — Higher values adapt faster to changes
- `ANOMALY_THRESHOLD = 0.5` — 50% deviation required

### P95 Latency Histogram

8-bucket histogram per (process, event) pair:

```c
// Bucket edges in microseconds
static const int histogram_edges[] = {
    10, 50, 100, 500, 1000, 5000, 10000, UINT_MAX
};

// Increment appropriate bucket
for (int i = 0; i < 8; i++) {
    if (latency_us < histogram_edges[i]) {
        stat->histogram[i]++;
        break;
    }
}

// P95 calculation
int calculate_p95(syscall_stat *stat) {
    uint64_t total = stat->count;
    uint64_t p95_target = total * 95 / 100;
    uint64_t cumulative = 0;
    
    for (int i = 0; i < 8; i++) {
        cumulative += stat->histogram[i];
        if (cumulative >= p95_target) {
            return histogram_edges[i];
        }
    }
    return histogram_edges[7];
}
```

### CPU Overhead Measurement

```c
// Baseline taken AFTER BPF programs are loaded
struct rusage usage_start;
getrusage(RUSAGE_SELF, &usage_start);

// Each render cycle (every 2 seconds)
struct rusage usage_now;
getrusage(RUSAGE_SELF, &usage_now);

double user_cpu = (usage_now.ru_utime.tv_sec - usage_start.ru_utime.tv_sec) +
                  (usage_now.ru_utime.tv_usec - usage_start.ru_utime.tv_usec) / 1e6;
                  
double sys_cpu = (usage_now.ru_stime.tv_sec - usage_start.ru_stime.tv_sec) +
                 (usage_now.ru_stime.tv_usec - usage_start.ru_stime.tv_usec) / 1e6;

double overhead = (user_cpu + sys_cpu) / wall_elapsed * 100.0;
```

This excludes:
- libbpf initialization overhead
- BPF verifier cost
- One-time setup operations

### Sched Noise Filtering

Off-CPU times > 200ms are excluded from scheduling latency metrics:

```c
#define SCHED_NOISE_NS 200000000ULL  // 200ms

if (event_type == EVENT_SCHED && duration_ns > SCHED_NOISE_NS) {
    // Count the event for rate calculation
    // But exclude from avg/P95/max/baseline
    stat->count++;
    return;
}
```

Rationale:
- 200ms+ off-CPU = process sleeping, not scheduling delay
- Including sleep time skews latency metrics
- Real scheduling delays are typically < 10ms

---

## Project Structure

```
ebpf-monitor-main/
│
├── src/
│   ├── bootstrap.bpf.c          # eBPF kernel programs (tracepoints, filters)
│   ├── bootstrap.c              # Main userspace loop, CLI, ring buffer polling
│   ├── bootstrap.h              # Shared event struct and event type enum
│   ├── bootstrap.skel.h         # Auto-generated BPF skeleton (DO NOT EDIT)
│   │
│   ├── stats.c                  # Aggregation, EMA baseline, P95 histogram
│   ├── stats.h                  # syscall_stat struct, MAX_STATS tunable
│   │
│   ├── dashboard.c              # CLI rendering (7 sections), color coding
│   ├── dashboard.h              # Color macros, latency helpers
│   │
│   ├── export.c                 # JSON/CSV snapshot, anomaly logging
│   ├── export.h                 # Export API declarations
│   │
│   ├── vscreen.c                # Virtual screen buffer for flicker-free rendering
│   ├── vscreen.h                # Virtual screen API
│   │
│   ├── layout.c                 # Dashboard layout calculations
│   ├── layout.h                 # Layout structures
│   │
│   ├── theme.c                  # Color theme definitions
│   ├── theme.h                  # Theme API
│   │
│   ├── term.h                   # Terminal control macros (ANSI codes)
│   ├── vmlinux.h                # Kernel type definitions (CO-RE, 3.3MB)
│   │
│   └── Makefile                 # Build system (auto-detects architecture)
│
├── README.md                    # This file
├── .gitignore                   # Git ignore patterns
├── .gitmodules                  # Git submodule configuration
└── .clang-format                # Code formatting rules
```

### File Responsibilities

| Component | Lines of Code | Primary Responsibility |
|-----------|---------------|------------------------|
| **bootstrap.bpf.c** | ~450 | eBPF tracepoint handlers, filtering, ring buffer submission |
| **bootstrap.c** | ~650 | CLI parsing, ring buffer polling, main event loop |
| **stats.c** | ~280 | Event aggregation, histogram, baseline, anomaly detection |
| **dashboard.c** | ~2200 | 7-section UI rendering, sorting, color coding |
| **export.c** | ~240 | JSON/CSV serialization, anomaly log writing |
| **vscreen.c** | ~340 | Double-buffered terminal rendering |
| **layout.c** | ~260 | Dashboard section positioning and sizing |
| **theme.c** | ~220 | Color theme management |

---

## Troubleshooting

### Build Errors

#### Error: `fatal error: 'bpf/libbpf.h' not found`

**Cause:** libbpf development headers not installed.

**Solution:**
```bash
sudo apt install libbpf-dev
# Or on Fedora:
sudo dnf install libbpf-devel
```

#### Error: `bpftool: command not found`

**Cause:** bpftool not in PATH.

**Solution:**
```bash
sudo apt install linux-tools-common linux-tools-$(uname -r)
# Verify:
which bpftool
```

#### Error: `clang: error: unknown target triple 'bpf'`

**Cause:** clang version too old (< 10.0).

**Solution:**
```bash
# Check version
clang --version

# Upgrade if needed
sudo apt install clang-14 llvm-14
export CC=clang-14
```

### Runtime Errors

#### Error: `Permission denied` when running `./bootstrap`

**Cause:** eBPF programs require root privileges.

**Solution:**
```bash
# Run with sudo
sudo ./bootstrap

# Or grant CAP_BPF capability (Linux ≥ 5.8)
sudo setcap cap_bpf,cap_perfmon,cap_net_admin=eip ./bootstrap
./bootstrap
```

#### Error: `libbpf: failed to load BPF skeleton`

**Cause:** Kernel doesn't support required eBPF features.

**Solution:**
```bash
# Check kernel version
uname -r
# Must be ≥ 5.8

# Verify BTF support
ls -l /sys/kernel/btf/vmlinux
# Should exist

# Check if BPF filesystem is mounted
mount | grep bpf
# Should show: bpffs on /sys/fs/bpf type bpf
```

#### Warning: `ringbuf_submit: dropped events`

**Cause:** Ring buffer full, events being dropped.

**Solution:**
```c
// Edit src/bootstrap.bpf.c
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 512 * 1024);  // Increase from 256KB
} events SEC(".maps");
```

Rebuild:
```bash
make clean && make
```

#### Error: `Slot limit reached (512/512)`

**Cause:** Too many unique (process, event) pairs being tracked.

**Solution:**
- Press `r` to reset statistics during runtime
- Or edit `src/stats.h`:
```c
#define MAX_STATS 1024  // Increase from 512
```
Rebuild: `make clean && make`

### Dashboard Issues

#### Dashboard not refreshing

**Cause:** Terminal size too small or terminal doesn't support ANSI codes.

**Solution:**
```bash
# Resize terminal to at least 80x24
resize -s 30 100

# Set TERM variable
export TERM=xterm-256color
```

#### Garbled output or color issues

**Cause:** Terminal doesn't support 256 colors.

**Solution:**
```bash
# Test color support
tput colors

# If < 256, use basic theme
sudo ./bootstrap --theme 0
```

### Performance Issues

#### High CPU overhead (>5%)

**Possible causes and solutions:**

1. **Too many events being traced**
```bash
# Apply filters to reduce volume
sudo ./bootstrap --comm target_process
```

2. **Slow export writing**
```bash
# Write exports to tmpfs instead of disk
sudo ./bootstrap --export-json /tmp/out.json
```

3. **Many context switches**
```bash
# Check if system is under heavy load
vmstat 1
```

#### Anomaly false positives

**Cause:** Baseline hasn't stabilized yet or threshold too sensitive.

**Solution:**
```c
// Edit src/stats.h
#define ANOMALY_THRESHOLD 0.75  // Increase from 0.5 (50% to 75%)
#define BASELINE_ALPHA 0.1      // Decrease from 0.2 (slower adaptation)
```

Rebuild: `make clean && make`

### Export Issues

#### JSON file is empty

**Cause:** Export triggered before any events collected.

**Solution:**
- Let the monitor run for at least 5-10 seconds before pressing `e` or `q`
- Check if any events are being captured (dashboard should show activity)

#### CSV parsing errors in Excel

**Cause:** Locale-specific decimal separator.

**Solution:**
```bash
# Use point as decimal separator
LC_NUMERIC=C sudo ./bootstrap --export-csv output.csv
```

---

## Known Limitations

1. **Process Name Truncation**
   - Kernel limits process names to 15 characters (`TASK_COMM_LEN`)
   - Longer names are silently truncated
   - Workaround: Use `--pid` filter for specific processes

2. **Maximum Tracked Entries**
   - Hard limit of 512 unique (process, event) pairs by default
   - Exceeding this drops new entries with a visible warning
   - Workaround: Increase `MAX_STATS` in `stats.h` and rebuild

3. **Exact Comm Match**
   - `--comm` filter requires exact 15-char match, not substring
   - Example: `--comm "very-long-proc"` won't match "very-long-process-name"
   - Workaround: Use `--pid` or no filter

4. **P95 Statistical Validity**
   - P95 shows `--` until 20+ samples collected
   - Low-frequency events may never show P95
   - Histogram granularity is coarse (8 buckets)

5. **Parent PID Not Displayed**
   - `ppid` is captured for exec/exit events
   - Currently not shown in dashboard (implementation limitation)
   - Available in exported JSON/CSV data

6. **Sched Noise Threshold**
   - Fixed 200ms threshold for filtering sleep events
   - May need tuning for real-time systems
   - Workaround: Modify `SCHED_NOISE_NS` in `stats.h`

7. **Single-Host Only**
   - No distributed tracing across multiple machines
   - Each monitor instance is independent
   - Future: Could aggregate multiple JSON exports

8. **No Historical Data**
   - Dashboard shows current window only (last 2 seconds)
   - No time-series database integration
   - Workaround: Use anomaly logs for long-term tracking

---

## Future Enhancements

### Planned Features

1. **Graphical Web Dashboard**
   - Real-time WebSocket updates
   - Interactive charts with time-series history
   - Multi-host aggregation
   - Technology: React + Recharts + WebSocket

2. **Prometheus/Grafana Integration**
   - Metrics exporter endpoint
   - Pre-built Grafana dashboards
   - AlertManager integration for anomalies

3. **Additional Syscalls**
   - Network I/O: `sendto`, `recvfrom`, `connect`
   - File operations: `stat`, `unlink`, `rename`
   - Memory: `brk`, `munmap`

4. **Process Tree Visualization**
   - Parent-child relationship display
   - Process lifetime tracking with hierarchy
   - Fork/exec flow diagrams

5. **Distributed Tracing**
   - Cross-host correlation via trace IDs
   - gRPC or HTTP span propagation
   - OpenTelemetry compatibility

6. **Advanced Filtering**
   - Substring/regex process name matching
   - UID/GID filtering
   - Namespace filtering (containers)
   - CPU affinity filtering

7. **Machine Learning Integration**
   - Autoencoder-based anomaly detection
   - Time-series forecasting for capacity planning
   - Clustering of similar workload patterns

8. **Recording/Replay**
   - Save event stream to file
   - Replay for offline analysis
   - Event compression for storage efficiency

9. **Container Awareness**
   - Docker/Kubernetes metadata enrichment
   - Per-container resource accounting
   - Pod-level aggregation

10. **Performance Optimizations**
    - Userspace RCU for lock-free stats updates
    - SPSC ring buffer for lower overhead
    - SIMD for histogram updates

---

## License

This project is developed as part of the CSC 206 Software Engineering course at IIT Roorkee.

**Academic Use License**

Copyright © 2026 Group 11 - IIT Roorkee

Permission is granted to use, modify, and distribute this software for academic and educational purposes only, with proper attribution to the original authors.

For commercial use or redistribution, please contact the authors.

---

## Acknowledgments

### Course Instructor
- **Prof. [Instructor Name]** — CSC 206 Software Engineering, IIT Roorkee

### Technical References
- **Brendan Gregg** — eBPF tools and tracing expertise
- **Andrii Nakryiko** — libbpf library and BPF CO-RE architecture
- **Linux Kernel Documentation** — eBPF subsystem and tracepoints

### Open Source Projects
- **libbpf** — BPF library for userspace applications
- **bpftool** — BPF program inspection and management
- **BCC (BPF Compiler Collection)** — Inspiration for tracing workflows

### Libraries & Tools
- **LLVM/clang** — BPF compilation toolchain
- **vmlinux.h** — Kernel type definitions via BTF

---

## Contact Information

For questions, issues, or collaboration inquiries:

**Project Repository:** [GitHub Link - To Be Added]

**Institution:** Indian Institute of Technology Roorkee  
**Course:** CSC 206 - Software Engineering  
**Academic Year:** 2025-2026

---

## References

1. Gregg, B. (2019). *BPF Performance Tools: Linux System and Application Observability*. Addison-Wesley Professional.

2. Linux Kernel Documentation. "BPF Documentation." https://www.kernel.org/doc/html/latest/bpf/

3. Nakryiko, A. "BPF CO-RE (Compile Once – Run Everywhere)." https://nakryiko.com/posts/bpf-core-reference-guide/

4. Starovoitov, A., & Nakryiko, A. "libbpf: the road to 1.0." Linux Plumbers Conference, 2021.

5. McCanne, S., & Jacobson, V. (1993). "The BSD Packet Filter: A New Architecture for User-level Packet Capture." USENIX Winter Conference.

6. Vieira, M. A., et al. (2020). "Fast Packet Processing with eBPF and XDP: Concepts, Code, Challenges, and Applications." ACM Computing Surveys.

---

**Last Updated:** April 19, 2026  
**Version:** 1.0  
**Document Status:** Final Submission
