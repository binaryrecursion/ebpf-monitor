#ifndef DASHBOARD_H
#define DASHBOARD_H

#include "stats.h"

/* ---------- terminal colors ---------- */

#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_DIM     "\033[2m"

/* Latency coloring: green < 10us, yellow < 100us, red >= 100us */
static inline const char *latency_color(long us)
{
    if (us < 10)
        return COLOR_GREEN;
    else if (us < 100)
        return COLOR_YELLOW;
    else
        return COLOR_RED;
}

/* Deviation coloring for anomaly display */
static inline const char *deviation_color(double dev)
{
    if (dev < 0.25)
        return COLOR_GREEN;
    else if (dev < ANOMALY_THRESHOLD)
        return COLOR_YELLOW;
    else
        return COLOR_RED;
}

/* ---------- dashboard API ---------- */

void dashboard_render(double elapsed);

#endif