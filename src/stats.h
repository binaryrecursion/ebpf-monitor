#ifndef STATS_H
#define STATS_H

#include "bootstrap.h"

#define MAX_STATS 256

struct syscall_stat {
    char process[16];
    char event[32];

    long count;
    long total_latency;
    long max_latency;

    long ctx_switches;
    double rate;
};

extern struct syscall_stat stats[MAX_STATS];
extern int stat_count;

void stats_update(const struct event *e);
void stats_compute_rates(double elapsed);
void stats_reset();

#endif