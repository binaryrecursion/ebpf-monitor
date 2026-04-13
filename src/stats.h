#ifndef STATS_H
#define STATS_H

#include "bootstrap.h"

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define MAX_STATS         512   /* max unique (process, event) slots   */
#define ANOMALY_THRESHOLD 0.5   /* 50% deviation triggers anomaly      */
#define BASELINE_ALPHA    0.2   /* EMA smoothing: 0=slow, 1=no memory  */

/* Sched delays > this (ns) are almost certainly sleep/idle, not real
   scheduling latency.  Still counted for rate but excluded from the
   latency average, max, and anomaly baseline.                         */
#define SCHED_NOISE_NS    (200ULL * 1000 * 1000)   /* 200 ms           */

/*
 * Simple latency histogram for approximate P95 calculation.
 * Buckets (in microseconds, upper edge):
 *   [0]  0–9 us        [1]  10–49 us       [2]  50–99 us
 *   [3]  100–499 us    [4]  500–999 us      [5]  1000–4999 us
 *   [6]  5000–9999 us  [7]  >= 10000 us
 */
#define LAT_BUCKETS 8
static const long lat_bucket_us[LAT_BUCKETS] = {
    10, 50, 100, 500, 1000, 5000, 10000, (long)9e18
};

/* ------------------------------------------------------------------ */
/* Per-(process, event) statistics                                     */
/* ------------------------------------------------------------------ */

struct syscall_stat {
    char process[16];
    char event[32];
    int  pid;

    long count;            /* total events seen                        */
    long total_latency;    /* sum of durations (ns)                    */
    long max_latency;      /* peak duration (ns)                       */
    long ctx_switches;     /* incremented for EVENT_SCHED only         */
    long exec_count;       /* incremented for EVENT_EXEC               */
    long drop_count;       /* high-noise sched samples excluded        */
    double rate;           /* events / second (recomputed each render) */

    /* Histogram for P95 approximation (counts per latency bucket) */
    long lat_hist[LAT_BUCKETS];

    /* --- adaptive baseline ----------------------------------------- */
    double baseline_latency; /* EMA of average latency (ns)            */
    double deviation;        /* |current_avg - baseline| / baseline    */
    int    is_anomaly;       /* 1 if deviation > ANOMALY_THRESHOLD     */
    int    baseline_ready;   /* 1 after first sample seeds baseline    */
};

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

extern struct syscall_stat stats[MAX_STATS];
extern int  stat_count;
extern long total_events_dropped;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

void  stats_update(const struct event *e);
void  stats_compute_rates(double elapsed);
void  stats_reset(void);
long  stats_p95_us(const struct syscall_stat *s);

#endif /* STATS_H */
