#ifndef DASHBOARD_H
#define DASHBOARD_H

#include "stats.h"

/* ------------------------------------------------------------------ */
/* Latency thresholds for color coding (shared with stats.h)          */
/* ------------------------------------------------------------------ */

#define LATENCY_GREEN_US   10
#define LATENCY_YELLOW_US 100

/*
 * Render the full dashboard.
 *
 * This call:
 *   1. Checks the current terminal size (responsive — no fixed width)
 *   2. Clears the virtual screen buffer
 *   3. Paints all panels via the vscreen cell API
 *   4. Flushes only changed cells to stdout (diff render, no flicker)
 *
 * Parameters:
 *   elapsed       - seconds since start (or last reset)
 *   cpu_pct       - this process's CPU overhead %
 *   active_pid    - PID filter (0 = none)
 *   active_comm   - comm filter ("" = none)
 *   active_min_ms - min-duration filter (0 = none)
 */
void dashboard_render(double elapsed, double cpu_pct,
                      int active_pid, const char *active_comm,
                      long active_min_ms);

#endif /* DASHBOARD_H */
