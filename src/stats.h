#ifndef STATS_H
#define STATS_H

#include "bootstrap.h"

#define MAX_STATS        256
#define ANOMALY_THRESHOLD 0.5   /* 50% deviation from baseline triggers anomaly */
#define BASELINE_ALPHA    0.2   /* EMA smoothing factor: 0=slow adapt, 1=no memory */

struct syscall_stat {
    char process[16];
    char event[32];
    int  pid;               /* PID of the process (last seen) */

    long count;
    long total_latency;
    long max_latency;
    long ctx_switches;
    double rate;

    /* --- adaptive baseline fields --- */
    double baseline_latency;  /* exponential moving average of avg latency (ns) */
    double deviation;         /* |current_avg - baseline| / baseline */
    int    is_anomaly;        /* 1 if deviation exceeds ANOMALY_THRESHOLD */
    int    baseline_ready;    /* 1 after first sample has seeded the baseline */
};

extern struct syscall_stat stats[MAX_STATS];
extern int stat_count;

void stats_update(const struct event *e);
void stats_compute_rates(double elapsed);
void stats_reset(void);

#endif