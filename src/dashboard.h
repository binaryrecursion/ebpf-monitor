#ifndef DASHBOARD_H
#define DASHBOARD_H

#include "stats.h"

/* ---------- terminal colors ---------- */

#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"

/* latency color helper */
static inline const char *latency_color(long us)
{
    if (us < 10)
        return COLOR_GREEN;
    else if (us < 100)
        return COLOR_YELLOW;
    else
        return COLOR_RED;
}

/* ---------- dashboard API ---------- */

void dashboard_render(double elapsed);

#endif