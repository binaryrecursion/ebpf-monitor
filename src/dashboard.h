#ifndef DASHBOARD_H
#define DASHBOARD_H

#include "stats.h"

/* ------------------------------------------------------------------ */
/* Terminal color codes                                                */
/* ------------------------------------------------------------------ */

#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_DIM     "\033[2m"

#define LATENCY_GREEN_US   10
#define LATENCY_YELLOW_US 100

static inline const char *latency_color(long us)
{
    if (us < LATENCY_GREEN_US)  return COLOR_GREEN;
    if (us < LATENCY_YELLOW_US) return COLOR_YELLOW;
    return COLOR_RED;
}

static inline const char *deviation_color(double dev)
{
    if (dev < 0.25)              return COLOR_GREEN;
    if (dev < ANOMALY_THRESHOLD) return COLOR_YELLOW;
    return COLOR_RED;
}

/*
 * Render the full dashboard.
 *
 * elapsed       - seconds since start (or last reset)
 * cpu_pct       - this process's CPU overhead %
 * active_pid    - PID filter (0 = none)
 * active_comm   - comm filter ("" = none)
 * active_min_ms - min-duration filter (0 = none)
 */
void dashboard_render(double elapsed, double cpu_pct,
                      int active_pid, const char *active_comm,
                      long active_min_ms);

#endif /* DASHBOARD_H */
