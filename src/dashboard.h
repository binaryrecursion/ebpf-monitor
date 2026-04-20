#ifndef DASHBOARD_H
#define DASHBOARD_H

#include "stats.h"

#define LATENCY_GREEN_US   10
#define LATENCY_YELLOW_US 100


void dashboard_render(double elapsed, double cpu_pct,
                      int active_pid, const char *active_comm,
                      long active_min_ms);

#endif 
