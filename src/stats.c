#include <string.h>
#include <stdio.h>
#include <time.h>

#include "stats.h"

struct syscall_stat stats[MAX_STATS];
int stat_count = 0;

static struct syscall_stat *get_stat(const char *proc, const char *event)
{
    for (int i = 0; i < stat_count; i++) {
        if (strcmp(stats[i].process, proc) == 0 &&
            strcmp(stats[i].event, event) == 0)
            return &stats[i];
    }

    if (stat_count >= MAX_STATS)
        return NULL;

    snprintf(stats[stat_count].process, sizeof(stats[stat_count].process), "%s", proc);
    snprintf(stats[stat_count].event, sizeof(stats[stat_count].event), "%s", event);

    stats[stat_count].count = 0;
    stats[stat_count].total_latency = 0;
    stats[stat_count].max_latency = 0;
    stats[stat_count].ctx_switches = 0;
    stats[stat_count].rate = 0;

    stat_count++;
    return &stats[stat_count - 1];
}

void stats_update(const struct event *e)
{
    if (e->filename[0] == '\0')
        return;

    /* ignore self */
    if (strcmp(e->comm, "bootstrap") == 0)
        return;

    /* ---------- PROCESS LIFECYCLE ---------- */
    if (e->type == EVENT_EXIT) {
        struct syscall_stat *s = get_stat(e->comm, "lifecycle");
        if (!s)
            return;

        s->count++;
        s->total_latency += e->duration_ns;

        if (e->duration_ns > s->max_latency)
            s->max_latency = e->duration_ns;

        return;
    }

    /* ---------- NORMAL EVENTS ---------- */
    if (e->type != EVENT_SYSCALL &&
        e->type != EVENT_SCHED)
        return;

    struct syscall_stat *s = get_stat(e->comm, e->filename);
    if (!s)
        return;

    s->count++;
    s->total_latency += e->duration_ns;

    if (e->duration_ns > s->max_latency)
        s->max_latency = e->duration_ns;

    if (e->type == EVENT_SCHED)
        s->ctx_switches++;
}

void stats_compute_rates(double elapsed)
{
    if (elapsed <= 0)
        return;

    for (int i = 0; i < stat_count; i++)
        stats[i].rate = stats[i].count / elapsed;
}

void stats_reset()
{
    stat_count = 0;
}